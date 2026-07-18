// AeroEdge Transport — the FIFO / dedup resequencing shim (014 §3 C1 / §5, X2).
//
// WHY THIS EXISTS (load-bearing): Quark's `Transport` seam (010) promises the DistributedRouter above
// it per-`(from → target)` FIFO — the property ADR-011's FIFO-under-relay and 017 delivery proofs
// depend on. A substrate that can REORDER (MQTT across a reconnect / clean-session, any multi-path
// fabric) or DUPLICATE (MQTT QoS-1 redelivery) cannot give that on its own. 014 C1 is therefore
// non-negotiable: "a transport that can reorder MUST add a sequence-number + resequencing shim." This
// is that shim — the mechanism that turns an unreliable, order-scrambling substrate back into the
// per-sender FIFO stream Quark requires, BEFORE frames are handed up to the router.
//
// THE CONTRACT it restores:
//   * The SENDER stamps a strictly-monotonic per-link sequence number on every frame (SequenceStamper).
//   * The RECEIVER (Resequencer) holds a BOUNDED reorder buffer, releases frames STRICTLY in seq order
//     (buffering out-of-order arrivals until the gap fills), and DROPS DUPLICATES (any seq already
//     released, i.e. seq < next_expected, or a second copy of a still-buffered seq).
//   * Output is exactly the send order, each frame exactly once — 0 loss, 0 duplication, 0 inversion.
//
// BOUNDED MEMORY (014 §11 "resequencing shim bounds"): the reorder buffer is a fixed ring of `window`
// slots — memory is O(window), never O(stream). A broker preserves *some* order (per-topic /
// per-session), so real displacement is small and the window is small (default 64). An arrival further
// than `window` ahead of the next expected seq CANNOT be buffered without unbounded memory; the shim
// refuses to release out of order (that would violate C1), records it as an OVERFLOW, and drops it.
// Overflow means the substrate reordered further than the window was sized for — a configuration/health
// signal surfaced to the operator, never silently tolerated. With a correctly-sized window (>= the
// substrate's max displacement) overflow is 0. This shim is deliberately SEPARATE from Quark 017's
// idempotency/dedup (avoid double-buffering, 014 §11): it restores ORDER at the wire edge; 017 dedups
// at the actor. A broker transport composes THIS on receive; a stream-ordered transport (TCP/gRPC)
// needs no resequencer at all (HTTP/2 / single-connection order already gives C1).
//
// OFF THE HOT PATH (R0/layering): transport adapters are not the flow steady path — this uses ordinary
// std containers (a fixed vector ring), not the 0-alloc discipline INode::process owes (N1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace aero::transport {

// A value tagged with its per-`(from → to)` monotonic sequence number (the §5 ordering-shim header a
// real MqttTransport carries in a small frame field). `T` is whatever the substrate moves — an int in
// the C1 gate, a quark::MessageFrame in the adapter.
template <class T>
struct Sequenced {
    std::uint64_t seq = 0;
    T value{};
};

// SENDER half: hands out strictly-increasing sequence numbers for ONE link (one `(from → to)` pair).
// A transport keeps one stamper per destination. First frame is seq 0; monotonic, never repeats.
class SequenceStamper {
public:
    template <class T>
    [[nodiscard]] Sequenced<T> stamp(T value) noexcept {
        return Sequenced<T>{next_++, std::move(value)};
    }

    [[nodiscard]] std::uint64_t peek_next() const noexcept { return next_; }

private:
    std::uint64_t next_ = 0;
};

// The outcome of offering one arrival to the resequencer (per-frame classification, for stats/tests).
enum class OfferOutcome : std::uint8_t {
    Released,    // this arrival (possibly plus buffered successors) was released in order to the sink
    Buffered,    // out-of-order but within the window — held until the gap ahead of it fills
    Duplicate,   // seq already released, or a second copy of a still-buffered seq — dropped
    Overflow,    // further than `window` ahead of next-expected — cannot buffer without unbounded mem
};

// Running counters for the C1 gate and operator health. Every arrival lands in exactly one bucket
// besides `released` (which counts frames HANDED UP, including ones drained out of the buffer).
struct ResequencerStats {
    std::uint64_t released = 0;    // frames delivered up, strictly in order (== unique frames sent)
    std::uint64_t duplicates = 0;  // dropped as duplicate/stale (dedup rule)
    std::uint64_t overflow = 0;    // dropped past the window (substrate reordered beyond `window`)
    std::uint64_t max_buffered = 0;  // high-water occupancy of the reorder buffer (memory bound proof)
};

// RECEIVER half: per-`(from → to)` bounded reorder buffer that restores strict FIFO + drops dups.
// One Resequencer per source link. `T` moved through unchanged; only ORDER is corrected.
template <class T>
class Resequencer {
public:
    static constexpr std::size_t kDefaultWindow = 64;  // small: a broker keeps displacement small (§5)

    explicit Resequencer(std::size_t window = kDefaultWindow)
        : window_(window == 0 ? 1 : window), ring_(window_) {}

    // Offer one (possibly reordered / duplicated) arrival. Frames that become deliverable are handed
    // to `emit` (a `void(T&&)` sink) STRICTLY in sequence order, oldest first. Returns how the arrival
    // itself was classified. `emit` may be called 0..window_ times (one arrival can unblock a run of
    // buffered successors). Never releases out of order — that is the whole point.
    template <class Sink>
    OfferOutcome offer(std::uint64_t seq, T value, Sink&& emit) {
        // Dedup rule: anything at or below the last released seq is stale (already delivered). The very
        // first expected seq is 0, so `seq < next_` is the exact "seq <= last-released" test.
        if (seq < next_) {
            ++stats_.duplicates;
            return OfferOutcome::Duplicate;
        }
        // Past the bounded window: refuse to release out of order (C1) — record + drop. With a
        // window >= the substrate's max displacement this branch is never taken.
        if (seq >= next_ + window_) {
            ++stats_.overflow;
            return OfferOutcome::Overflow;
        }
        Slot& slot = ring_[seq % window_];
        if (slot.present && slot.seq == seq) {  // a second copy of a still-buffered future frame
            ++stats_.duplicates;
            return OfferOutcome::Duplicate;
        }
        slot.seq = seq;
        slot.value = std::move(value);
        slot.present = true;
        note_occupancy();

        // Drain the contiguous prefix starting at next_ — this is what enforces strict in-order release.
        if (seq != next_) return OfferOutcome::Buffered;  // gap ahead of next_ still open
        while (true) {
            Slot& head = ring_[next_ % window_];
            if (!head.present || head.seq != next_) break;
            head.present = false;
            ++next_;
            ++stats_.released;
            emit(std::move(head.value));
        }
        return OfferOutcome::Released;
    }

    [[nodiscard]] std::uint64_t next_expected() const noexcept { return next_; }
    [[nodiscard]] const ResequencerStats& stats() const noexcept { return stats_; }

private:
    struct Slot {
        std::uint64_t seq = 0;
        T value{};
        bool present = false;
    };

    void note_occupancy() noexcept {
        std::uint64_t occ = 0;
        for (const Slot& s : ring_) occ += s.present ? 1 : 0;
        if (occ > stats_.max_buffered) stats_.max_buffered = occ;
    }

    std::size_t window_;
    std::vector<Slot> ring_;        // fixed-size reorder buffer — bounded memory (O(window))
    std::uint64_t next_ = 0;        // next seq to release; next_-1 == last released (dedup boundary)
    ResequencerStats stats_{};
};

}  // namespace aero::transport
