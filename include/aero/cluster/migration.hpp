// AeroEdge cluster — FENCED MIGRATION: the load-bearing no-dual-drive safety gate (spec 010 §3). THIN
// over Quark (R0): the fence is Quark 012's fence TOKEN (already acquired inside `DurableState::recover`
// via `store.acquire_fence`, 007 §7); AeroEdge adds ONLY the one migration rule 010 §3 mandates on top
// of it — a migrated device actor must RE-ESTABLISH its driver connection on the new node before it
// resumes flows.
//
// THE INDUSTRIAL-SAFETY INVARIANT (010 §3): the same physical device is NEVER driven by two activations
// at once. Quark 012's store enforces it structurally: `acquire_fence(id)` hands out a strictly-greater
// token and marks it the current OWNER; the store REJECTS (`errc::unavailable`, `fenced_error`) any
// write carrying a token OLDER than the current owner. So when a device actor re-activates on node B and
// `recover()` acquires a FRESH fence f2 > the old activation's f1, node A is fenced out: its subsequent
// `checkpoint()` with f1 is rejected, and it can no longer commit device state. B is the sole writer.
//
//   activation on A: recover() → f1, owner=f1        A drives the device
//   migrate → activation on B: recover() → f2>f1, owner=f2   B drives the device; A is now fenced out
//   A's later checkpoint(f1): f1 < owner ⇒ REJECTED  (no dual-drive)
//   B's checkpoint(f2):       f2 == owner ⇒ ACCEPTED
//
// AeroEdge's ADDED rule (010 §3, not new distribution machinery): on (re)activation the actor re-opens
// its driver so flows resume against a LIVE device link on the new node. `EdgeActivation` binds the
// Quark-fenced `DurableState` recovery to that driver re-open in one call.
#pragma once

#include <utility>

#include "aero/core/persistent_actor.hpp"   // aero::DurableState — the Quark 012 fence lives inside recover()
#include "aero/sdk/driver.hpp"              // aero::IDriver, DriverConfig, DriverStatus (006)

#include "quark/core/ids.hpp"               // ActorId, FenceToken
#include "quark/core/persistence.hpp"        // Store concept

namespace aero::cluster {

// EdgeActivation — one activation of a device actor ON A NODE: its durable tier-1 state (007, over a
// Quark 012 store) + its device driver (006). Modelling migration as "construct a fresh EdgeActivation
// over the SAME store + ActorId on the new node" mirrors exactly how Quark hands an actor off — the new
// activation recovers committed state and acquires a fresh fence; the old one keeps its stale fence and
// is fenced out on its next write. `activate()` performs the two AeroEdge re-activation steps (010 §3)
// atomically: (1) Quark-fenced durable recovery, (2) driver re-open.
template <class State, class Store>
class EdgeActivation {
public:
    // `driver` and `store` are owned elsewhere (the runtime); EdgeActivation binds one actor's durable
    // state to one driver + the device connection config it re-dials on activation.
    EdgeActivation(Store& store, quark::ActorId id, IDriver& driver, DriverConfig cfg) noexcept
        : durable_(store, id), driver_(&driver), cfg_(cfg) {}

    struct ActivateResult {
        quark::FenceToken fence{};        // the FRESH fence this activation acquired (fences out prior ones)
        DriverStatus driver = DriverStatus::Error;  // driver re-open status on the new node
        bool recovered = false;           // did it recover committed state (vs cold-seed)?
    };

    // (Re)activate this actor on THIS node (010 §3). Order matters for safety:
    //   1. recover() — loads committed state AND acquires a FRESH Quark 012 fence (007 §7), which
    //      atomically fences out any prior activation (the no-dual-drive guarantee, 010 §3).
    //   2. re-open the driver — re-establish the device link on the new node so flows resume against a
    //      LIVE connection (010 §3 AeroEdge rule / 006 §8). Done only AFTER the fence is held, so the
    //      device is re-dialled by the sole legitimate owner.
    [[nodiscard]] ActivateResult activate(State initial) noexcept {
        durable_.recover(std::move(initial));                 // Quark 012 fence acquired here (007 §7)
        const DriverStatus ds = driver_->open(cfg_);          // AeroEdge 010 §3 rule: re-dial the device
        return {durable_.fence(), ds, durable_.recovered()};
    }

    // The durable tier-1 state (mutate in on_commit, then checkpoint — carries THIS activation's fence,
    // so a stale activation's checkpoint is rejected by the store).
    [[nodiscard]] DurableState<State, Store>& durable() noexcept { return durable_; }
    [[nodiscard]] const DurableState<State, Store>& durable() const noexcept { return durable_; }
    [[nodiscard]] IDriver& driver() noexcept { return *driver_; }
    [[nodiscard]] quark::FenceToken fence() const noexcept { return durable_.fence(); }

private:
    DurableState<State, Store> durable_;
    IDriver* driver_;
    DriverConfig cfg_;
};

}  // namespace aero::cluster
