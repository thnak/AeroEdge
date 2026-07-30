// AeroEdge Transport — ChaosChannel: the adversary the resequencing shim (014 §5 C1) must defeat.
//
// A deterministic, SEEDABLE in-process channel that deliberately REORDERS, DUPLICATES and DELAYS the
// sequenced items handed to it — standing in for the worst an MQTT broker does across a reconnect /
// clean-session + QoS-1 redelivery. It is the test substrate whose scrambled output proves the
// Resequencer both NECESSARY (raw output is out-of-order + duplicated) and SUFFICIENT (resequenced
// output is strict FIFO, exactly once).
//
// TEST FIXTURE — not shipped: lives in tests/, pulled into no library. It moves `Sequenced<T>` (the
// §5 seq-stamped unit); a real MqttTransport carries the same seq in a small MessageFrame header field.
//
// DETERMINISM / SANITIZERS: single-threaded, driven by a seeded LCG (no std::random, no clock, no
// threads) — byte-for-byte reproducible and TSan-trivially clean (no shared mutable state across
// threads). Each pushed item is scheduled at `arrival_tick + jitter` where `jitter <= max_jitter`, so
// max displacement is BOUNDED by max_jitter: size the resequencer window > max_jitter and overflow is
// impossible. Items with equal release tick emit in insertion order — a stable, reproducible scramble.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <utility>

#include "aero/transport/resequencer.hpp"

namespace aero::testing {

// A tiny seeded LCG (Numerical Recipes constants). Deterministic pseudo-randomness with NO global state
// and NO std::random — the seed fully determines the chaos, so a failing run is exactly reproducible.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : state_(seed) {}
    std::uint32_t next() noexcept {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<std::uint32_t>(state_ >> 16);
    }
    // Uniform in [0, n).
    std::uint32_t below(std::uint32_t n) noexcept { return n == 0 ? 0 : next() % n; }

private:
    std::uint64_t state_;
};

template <class T>
class ChaosChannel {
public:
    struct Params {
        std::uint32_t max_jitter = 12;   // max reorder displacement (keep < resequencer window)
        std::uint32_t dup_percent = 25;  // % of items also delivered a SECOND time (QoS-1 replay)
    };

    ChaosChannel(std::uint64_t seed, Params params = {}) : rng_(seed), params_(params) {}

    // Register the downstream sink (the resequencer's `offer`, or a raw collector for the control run).
    void on_deliver(std::function<void(std::uint64_t seq, const T&)> sink) { sink_ = std::move(sink); }

    // Sender stamped `seq` on `value`. Schedule its (possibly duplicated) delivery with bounded jitter,
    // then flush everything now due. Reorders relative to seq because release tick = arrival + jitter.
    void push(std::uint64_t seq, T value) {
        const std::uint64_t arrival = tick_++;
        schedule(arrival + rng_.below(params_.max_jitter + 1), seq, value);
        if (rng_.below(100) < params_.dup_percent) {  // QoS-1 style duplicate, independent jitter
            schedule(arrival + rng_.below(params_.max_jitter + 1), seq, value);
        }
        drain_due(arrival);  // release everything whose tick has passed → streaming, bounded reorder
    }

    // End of stream: release every remaining scheduled item (still in bounded-reorder tick order).
    void flush() { drain_due(UINT64_MAX); }

    [[nodiscard]] std::uint64_t delivered() const noexcept { return delivered_; }

private:
    struct Item {
        std::uint64_t seq;
        T value;
    };

    void schedule(std::uint64_t at, std::uint64_t seq, const T& value) {
        pending_.emplace(at, Item{seq, value});
    }

    void drain_due(std::uint64_t now) {
        while (!pending_.empty() && pending_.begin()->first <= now) {
            const Item it = pending_.begin()->second;
            pending_.erase(pending_.begin());
            ++delivered_;
            if (sink_) sink_(it.seq, it.value);
        }
    }

    Lcg rng_;
    Params params_;
    std::multimap<std::uint64_t, Item> pending_;  // scheduled deliveries keyed by release tick
    std::function<void(std::uint64_t, const T&)> sink_;
    std::uint64_t tick_ = 0;
    std::uint64_t delivered_ = 0;
};

}  // namespace aero::testing
