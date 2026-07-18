// AeroEdge core — the durable-state seam (spec 007 §2 / §7).
//
// AeroEdge writes NO persistence machinery (S7): serialization is Quark 016 (`snapshot.hpp`
// encode/decode of a `Described` value), durability is a Quark 012 `Store` (InMemoryStore, FileStore,
// …), the consistent point is Quark's `snapshot_sequential`, and fencing is the store's fence token.
// This header is the THIN glue that binds one actor's committed member fields to that machinery.
//
// INTEGRATION PATH. Quark 012 exposes persistence as an EXPLICIT `Store` + snapshot/recover API — the
// `Persistent<Model, Mode>` policy tag is a *declared surface* (persistence.hpp: "detecting it inside
// the Actor<> CRTP policy pack … and driving recovery from activation … is a reported seam that lives
// in those core headers"); the Engine does NOT auto-persist on handler completion or auto-recover on
// activation. So AeroEdge carries the tag on the actor's policy pack to DECLARE intent (007 §2) and
// drives the write/recover explicitly through this seam — recovery at (re)activation, a Sync snapshot
// at each commit — exactly the pattern Quark sample 07 shows. All of it is OFF the hot path.
#pragma once

#include <utility>

#include "quark/core/activation.hpp"    // Activation, DispatchTable — the consistent-point guard
#include "quark/core/dispatch.hpp"
#include "quark/core/ids.hpp"           // ActorId, FenceToken
#include "quark/core/persistence.hpp"   // Store, SeqNo, FenceToken
#include "quark/core/snapshot.hpp"      // load_snapshot / snapshot_sequential (012 §Snapshot)

namespace aero {

// DurableState<State, Store> — tier-1 actor state made durable (007 §1). `State` is a Quark
// `Described` value type (QUARK_SERIALIZE); `Store` is any Quark 012 store. An actor holds one of
// these as a member, `recover()`s it on activation, mutates `state()` inside `on_commit`, and lets it
// `checkpoint()` the mutation `Sync` before the message completes (S3). This is the ONLY tier that
// survives restart/migration (S1); the flow context (tier-2) and shared cache (tier-3) are not here.
template <class State, class Store>
class DurableState {
public:
    DurableState(Store& store, quark::ActorId id) noexcept : store_(&store), id_(id) {}

    // Recovery-on-activation (007 §7): load the last snapshot (Quark 012 decodes it through the 016
    // migration chain), or seed `initial` on a cold start with no snapshot on record. Then acquire a
    // FRESH fence token (ADR-009 Restart-reload bump) to become the current owner — a superseded prior
    // activation's later writes are then rejected by the store (S: split-brain fence). State recovery
    // is Quark's; AeroEdge adds only the driver re-open below.
    void recover(State initial) noexcept {
        auto snap = quark::load_snapshot<State>(*store_, id_);
        if (snap && snap->has_value()) {
            state_ = std::move((*snap)->state);
            recovered_ = true;
        } else {
            state_ = std::move(initial);
            recovered_ = false;
        }
        fence_ = store_->acquire_fence(id_);  // become current owner; fence any stale writer out

        // Driver re-open hook (006 §8 / 007 §7): a connected driver would re-`open()` its device here
        // so flows resume against a live link. The Phase-2 GeneratorDriver has no connection, so this
        // is intentionally a no-op stub — networking is out of Phase-3 scope.
    }

    // The committed durable fields. Mutated in the actor's `on_commit` (the single write point, S1),
    // then made durable by `checkpoint()`.
    [[nodiscard]] State& state() noexcept { return state_; }
    [[nodiscard]] const State& state() const noexcept { return state_; }

    // Sync checkpoint (007 §2.2, S3): serialize + persist the current state so the mutation is durable
    // BEFORE the message completes. On a `Sequential` actor the commit point is a consistent point
    // (007 §2.3): `snapshot_sequential`'s `quiesce(Drain)` guard resolves synchronously and is a
    // no-op, so the snapshot is torn-state-free at no extra cost (mirrors Quark sample 07 ACT 1). The
    // throwaway `Activation` reaches that guard; it is off the hot path (commit, not execute).
    // Snapshot model subsumes no event tail (007 §2.1), so `through_seq` is 0.
    bool checkpoint() noexcept {
        quark::Activation act(nullptr, quark::DispatchTable{});
        auto rc = quark::snapshot_sequential<State>(act, *store_, id_, fence_,
                                                    /*through_seq=*/0, state_);
        durable_ok_ = static_cast<bool>(rc);
        return durable_ok_;
    }

    // Introspection for handlers / tests (not a durability seam).
    [[nodiscard]] bool recovered() const noexcept { return recovered_; }   // found a prior snapshot?
    [[nodiscard]] bool durable_ok() const noexcept { return durable_ok_; }  // last checkpoint durable?
    [[nodiscard]] quark::FenceToken fence() const noexcept { return fence_; }

private:
    Store* store_;
    quark::ActorId id_;
    State state_{};
    quark::FenceToken fence_{};
    bool recovered_ = false;
    bool durable_ok_ = false;
};

}  // namespace aero
