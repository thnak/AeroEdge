// AeroEdge MES gateway — the transactional OUTBOX + the MesGateway actor (spec 012 §3, M2/M3).
//
// THE ONE PLACE AeroEdge LEANS HARD ON QUARK 012/017 (012 §3). A production/quality report is durably
// STAGED into the outbox (a Quark 012 `Store`, Sync-checkpointed) BEFORE it is acked — so an MES outage
// or a node crash DELAYS but never DROPS it (M3). The outbox drains at-least-once: each entry carries a
// stable idempotency key so a retried delivery is deduped MES-side; a `Retry` result keeps the entry
// durably and preserves order. THIN-OVER-QUARK (R0): AeroEdge writes NO queue and NO persistence engine
// — the outbox is a `DurableState<OutboxState, Store>` over the same Quark 012 seam the LineActor uses.
//
// M2 (flows never block on MES): a flow's MesReportNode only STAGES a StagedMesReport into ctx; the
// flow actor forwards it to the MesGateway actor via `tell(StageReport)`. The gateway — the SOLE owner
// of the adapter (012 §3) — does the durable staging + the blocking MES call on ITS lane, off every flow.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aero/core/persistent_actor.hpp"   // aero::DurableState — the Quark 012 durable-state seam
#include "aero/mes/mes.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"          // quark::Ask
#include "quark/core/dispatch.hpp"           // quark::Protocol, Sequential
#include "quark/core/ids.hpp"
#include "quark/core/serialize.hpp"          // QUARK_SERIALIZE

namespace aero::mes {

// One durably-staged, not-yet-acked report (012 §3). `Described` (QUARK_SERIALIZE) so it snapshots
// through the Quark 016 path. Mirror of MesReport minus the enum type (stored as a u8 for wire stability).
struct OutboxEntry {
    std::uint8_t kind = 0;         // MesReport::Kind
    std::string line;
    std::string label;
    double value = 0.0;
    std::uint64_t seq = 0;
    std::string idempotency_key;
};

// The durable tier-1 outbox state (007 §1): a monotonic seq + the FIFO of pending reports. Snapshotted
// Sync at each stage/ack so it survives restart + MES outage (M3). Bounded-outbox / backfill retention
// (012 §7 open question) is future work; Phase-10 keeps every un-acked report.
struct OutboxState {
    std::uint64_t next_seq = 0;
    std::vector<OutboxEntry> pending;
};

// QUARK_SERIALIZE must sit in the type's OWN namespace so the generated quark_describe is found by ADL
// from the Quark snapshot path (the Described concept resolves it unqualified via the type's namespace).
QUARK_SERIALIZE(OutboxEntry, (1, kind), (2, line), (3, label), (4, value), (5, seq), (6, idempotency_key))
QUARK_SERIALIZE(OutboxState, (1, next_seq), (2, pending))

// The transactional outbox over a Quark 012 `Store` (012 §3). Owned by the MesGateway. Staging is
// durable-before-ack (M3); draining is at-least-once with idempotency-key dedup MES-side.
template <class Store>
class Outbox {
public:
    Outbox(Store& store, quark::ActorId id) : durable_(store, id) {
        durable_.recover(OutboxState{});  // recovery-on-activation (007 §7): reload any pending on start
    }

    // Stage a report DURABLY BEFORE it is acked (M3). Assign the monotonic seq + a stable idempotency
    // key, append to the pending FIFO, and Sync-checkpoint — the report is on durable store before this
    // returns, so a crash/outage right after cannot lose it. Returns the idempotency key.
    std::string stage(MesReport r) {
        auto& st = durable_.state();
        r.seq = ++st.next_seq;
        if (r.idempotency_key.empty()) r.idempotency_key = make_key(r);
        st.pending.push_back(OutboxEntry{static_cast<std::uint8_t>(r.kind), r.line, r.label, r.value,
                                         r.seq, r.idempotency_key});
        (void)durable_.checkpoint();  // Sync durable (007 §2.2) — the report survives from here on
        return r.idempotency_key;
    }

    // Drain the pending FIFO to the MES at-least-once (M3). `Delivered`/`Rejected` → remove + persist;
    // `Retry` (outage/5xx) → STOP, keeping this entry and all after it durably in order for the next
    // drain. Checkpointing after each ack means a crash mid-drain resumes without re-sending an acked
    // one (and even if it did, the idempotency key dedups it MES-side). Returns the count DELIVERED.
    std::size_t drain(IMesAdapter& adapter) {
        std::size_t delivered = 0;
        auto& pending = durable_.state().pending;
        while (!pending.empty()) {
            const OutboxEntry& e = pending.front();
            MesReport r{static_cast<MesReport::Kind>(e.kind), e.line, e.label, e.value, e.seq,
                        e.idempotency_key};
            const MesResult res = adapter.report(r);
            if (res == MesResult::Retry) break;         // MES outage — keep it durably, retry later (M3)
            pending.erase(pending.begin());             // Delivered or Rejected(permanent) — drop it
            (void)durable_.checkpoint();                // durable after the ack so a mid-drain crash resumes
            if (res == MesResult::Delivered) ++delivered;
        }
        return delivered;
    }

    [[nodiscard]] std::size_t pending_count() const noexcept { return durable_.state().pending.size(); }
    [[nodiscard]] std::uint64_t staged_total() const noexcept { return durable_.state().next_seq; }
    [[nodiscard]] bool recovered() const noexcept { return durable_.recovered(); }

private:
    // Stable across retries (M3): keyed on the immutable (line, kind, seq) so re-sending yields the SAME
    // key and the MES dedups. seq is assigned once at stage() and never changes.
    static std::string make_key(const MesReport& r) {
        return r.line + ":" + r.kind_name() + ":" + std::to_string(r.seq);
    }

    aero::DurableState<OutboxState, Store> durable_;
};

// ---- MesGateway actor messages (012 §3) ----------------------------------------------------------
// From a flow actor → gateway (M2): stage a report. Carries the canonical MesReport by value (fits the
// Quark inline message slot); the flow only `tell`s — it never blocks on the MES.
struct StageReport { MesReport report; };
// A nudge to drain the outbox (a timer tick, or issued right after a StageReport). Draining runs the
// blocking adapter on the gateway lane, never a flow (M2).
struct DrainOutbox {};
// Ask → outbox observability (012 §4 / 009): staged total, still-pending, delivered so far.
struct GetOutboxStats {};
struct OutboxStats {
    std::uint64_t staged = 0;
    std::uint64_t pending = 0;
    std::uint64_t delivered = 0;
};

// The MesGateway actor (012 §3): SOLE owner of the adapter + the outbox. Sequential → all stage/drain
// serialize on one executor, so the (non-thread-safe) adapter client is only ever touched from here.
// A StageReport durably stages then drains; a DrainOutbox re-attempts a stuck outbox after the MES
// recovers. The adapter's blocking I/O runs on this actor's lane (012 §3) — never on a flow (M2).
template <class Store>
struct MesGatewayActor : quark::Actor<MesGatewayActor<Store>, quark::Sequential> {
    using protocol = quark::Protocol<StageReport, DrainOutbox, quark::Ask<GetOutboxStats, OutboxStats>>;

    MesGatewayActor(IMesAdapter& adapter, Store& store, quark::ActorId outbox_id)
        : adapter_(&adapter), outbox_(store, outbox_id) {}

    void handle(const StageReport& c) noexcept {
        (void)outbox_.stage(c.report);          // durable-before-ack (M3)
        delivered_ += outbox_.drain(*adapter_);  // best-effort drain now; a Retry leaves it pending
    }

    void handle(const DrainOutbox&) noexcept {
        delivered_ += outbox_.drain(*adapter_);  // re-drain after the MES recovers (M3)
    }

    void handle(const quark::Ask<GetOutboxStats, OutboxStats>& m) noexcept {
        m.respond(OutboxStats{outbox_.staged_total(),
                              static_cast<std::uint64_t>(outbox_.pending_count()), delivered_});
    }

private:
    IMesAdapter* adapter_;
    Outbox<Store> outbox_;
    std::uint64_t delivered_ = 0;
};

}  // namespace aero::mes
