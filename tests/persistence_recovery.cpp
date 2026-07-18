// AeroEdge Phase-3 gate — durable actor state survives a simulated restart (spec 007 §2/§6/§7).
//
// The load-bearing proof of the phase: a device/line actor's committed state (a production counter +
// last committed value) is snapshotted `Sync` to a Quark 012 `Store` at each `commit`, and a FRESH
// actor instance backed by the SAME store RECOVERS it on activation — the state survives a restart.
// It also proves the "nodes compute, actors remember" split (007 §6): the actor's DURABLE counter
// recovers, while a stateful node's TRANSIENT window is lost and rebuilds cold after the restart.
//
//   ACT 1 — run a LineActor on the REAL Quark engine, feed N Commands; each commit promotes facts into
//           durable actor state and `Sync`-checkpoints them. Assert the store holds them (Sync gate).
//   ACT 2 — SIMULATED RESTART: tear the engine down, construct a FRESH LineActor over the SAME store.
//           Assert the durable counter recovered but the node's transient window is cold; then run a
//           2nd engine and feed one more Command to show durable accumulation CONTINUES from recovery.
//   ACT 3 — FileStore (crash-durable WAL) round-trip through the same DurableState seam.
//
// Exit code 0 = OK (ctest gate). Prints "FAIL" on any mismatch.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "aero/core/edge_actor.hpp"
#include "aero/core/persistent_actor.hpp"
#include "aero/nodes/builtin_nodes.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/file_store.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"
#include "quark/core/spawn.hpp"

using namespace quark;

// --- Durable tier-1 state (007 §1): decisions/facts, NOT the telemetry firehose (007 §3, S4). -----
struct LineState {
    std::uint64_t produced = 0;  // production counter — a durable fact that must survive restart
    double last_avg = 0.0;       // last committed moving average
};
QUARK_SERIALIZE(LineState, (1, produced), (2, last_avg))  // 016 describe path → snapshottable

// --- Messages ---------------------------------------------------------------------------------
struct ReceivePacket { std::int64_t raw; };
struct GetProduced {};  // ask → durable production counter (std::uint64_t)
struct GetLastAvg {};   // ask → last committed moving average (double)
struct GetWarm {};      // ask → transient node window depth (std::uint64_t)

// --- The persistent line actor. Owns its per-actor flow (Decode → MovingAverage<4>) and its durable
// state. `on_commit` is the single write point (S1): it promotes committed facts into actor state and
// `Sync`-persists them; the node holds only transient window state it is willing to lose (007 §6).
// Templated on the `Store` so the same actor runs over InMemoryStore and the crash-durable FileStore.
template <class Store>
struct LineActor : aero::EdgeActorBase<LineActor<Store>, Sequential,
                                       Persistent<Snapshot, PersistMode::Sync>> {  // 007 §2 declared intent
    using protocol = Protocol<ReceivePacket, Ask<GetProduced, std::uint64_t>,
                              Ask<GetLastAvg, double>, Ask<GetWarm, std::uint64_t>>;

    LineActor(Store& store, ActorId id) : durable_(store, id) {
        flow_.add(source_).add(avg_);      // per-actor flow (004 §2.1): node state is per-actor
        this->bind_flow(flow_);
        durable_.recover(LineState{});      // recovery-on-activation (007 §7); cold ⇒ seed zeros
    }

    void handle(const ReceivePacket& c) noexcept { this->process_frame(aero::Frame{c.raw}); }
    void handle(const Ask<GetProduced, std::uint64_t>& m) noexcept { m.respond(durable_.state().produced); }
    void handle(const Ask<GetLastAvg, double>& m) noexcept { m.respond(durable_.state().last_avg); }
    void handle(const Ask<GetWarm, std::uint64_t>& m) noexcept {
        m.respond(static_cast<std::uint64_t>(avg_.warm_samples()));
    }

    // Single write point (S1): promote ctx facts → durable actor state, then Sync-checkpoint (S3).
    void on_commit(const aero::ProcessingContext& ctx) noexcept {
        if (ctx.failed) return;
        ++durable_.state().produced;                               // "actors remember": durable count
        if (!ctx.output.empty()) durable_.state().last_avg = ctx.output.back();
        (void)durable_.checkpoint();                               // Sync: durable before msg completes
    }

    aero::nodes::DecodeSourceNode source_;
    aero::nodes::MovingAverageNode<4> avg_;  // TRANSIENT per-actor node state (007 §6)
    aero::CompiledFlow flow_;
    aero::DurableState<LineState, Store> durable_;
};

// Bring an already-constructed LineActor up on a fresh engine, feed `values`, and return what the
// asks observe. Sequential + mailbox FIFO ⇒ the asks observe every prior tell (deterministic).
struct ObservedState { std::uint64_t produced; double last_avg; std::uint64_t warm; };

template <class Store>
ObservedState run_engine(LineActor<Store>& actor, std::uint32_t key,
                     const std::vector<std::int64_t>& values) {
    detail::MessagePool pool(1024);
    auto activation = std::make_unique<Activation>(&actor, LineActor<Store>::dispatch_table(), pool.sink());
    Engine<> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});
    register_actor<LineActor<Store>>(eng, key, *activation);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<LineActor<Store>> ref = router.get<LineActor<Store>>(key);
    eng.start();

    for (std::int64_t v : values) ref.tell(ReceivePacket{v});
    auto produced = block_on(ref.template ask<std::uint64_t>(GetProduced{}));
    auto avg = block_on(ref.template ask<double>(GetLastAvg{}));
    auto warm = block_on(ref.template ask<std::uint64_t>(GetWarm{}));
    eng.stop();

    return ObservedState{produced.value_or(0), avg.value_or(-1.0), warm.value_or(999)};
}

int main() {
    bool ok = true;
    const ActorId id{TypeKey{0x11FE}, 7};
    InMemoryStore store;

    // ===== ACT 1 — accumulate durable state on the real engine, Sync-checkpointed each commit ======
    {
        LineActor<InMemoryStore> a1(store, id);
        ok &= !a1.durable_.recovered();                    // cold start: nothing on record yet

        // Feed 1..10. produced -> 10; moving avg of last 4 = (7+8+9+10)/4 = 8.5; window warm (4).
        auto r = run_engine(a1, /*key*/ 7, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
        ok &= (r.produced == 10 && r.last_avg == 8.5 && r.warm == 4);

        // Sync durability gate (S3): the value is in the STORE (durable) — not just in the live actor.
        auto snap = load_snapshot<LineState>(store, id);
        const bool durable = snap && snap->has_value() && (*snap)->state.produced == 10 &&
                             (*snap)->state.last_avg == 8.5;
        ok &= durable;
        std::printf("[act1] produced=%llu last_avg=%.2f warm=%zu | store.produced=%llu (Sync durable: %s)\n",
                    (unsigned long long)r.produced, r.last_avg, (std::size_t)r.warm,
                    (unsigned long long)(durable ? (*snap)->state.produced : 0),
                    durable ? "yes" : "NO");
    }  // a1 + engine destroyed = simulated crash/deactivation

    // ===== ACT 2 — SIMULATED RESTART: fresh actor over the SAME store ==============================
    {
        LineActor<InMemoryStore> a2(store, id);            // recovery-on-activation (007 §7)

        // Durable tier-1 state recovered; the transient node window is NOT part of recovery (cold).
        const bool recovered = a2.durable_.recovered() && a2.durable_.state().produced == 10 &&
                               a2.durable_.state().last_avg == 8.5 && a2.avg_.warm_samples() == 0;
        ok &= recovered;

        // Feed ONE more (value 100). Durable count CONTINUES 10 -> 11. The moving-average node started
        // COLD: with only 1 sample its window depth is 1 and its avg is 100 — proving the transient
        // node state was LOST and is rebuilding, while durable actor state survived (007 §6).
        auto r = run_engine(a2, /*key*/ 7, {100});
        ok &= (recovered && r.produced == 11 && r.warm == 1 && r.last_avg == 100.0);
        std::printf("[act2] recovered produced=10 last_avg=8.50 (durable survived: %s) | "
                    "after +1: produced=%llu warm=%zu avg=%.1f (transient rebuilt cold: %s)\n",
                    recovered ? "yes" : "NO", (unsigned long long)r.produced, (std::size_t)r.warm,
                    r.last_avg, (r.warm == 1 && r.last_avg == 100.0) ? "yes" : "NO");
    }

    // ===== ACT 3 — FileStore (crash-durable WAL) round-trip through the same seam ==================
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::current_path() / "aeroedge_persist_tmp";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const ActorId fid{TypeKey{0xF11E}, 3};
        {
            FileStore fstore(dir.string());
            LineActor<FileStore> a(fstore, fid);
            auto r = run_engine(a, /*key*/ 3, {2, 4, 6, 8});   // produced=4; avg of all 4 = 5.0
            ok &= (r.produced == 4 && r.warm == 4 && r.last_avg == 5.0);
        }
        // Reopen the WAL in a brand-new FileStore + fresh actor: the durable state must reload.
        {
            FileStore fstore2(dir.string());
            LineActor<FileStore> a2(fstore2, fid);
            const bool reloaded = a2.durable_.recovered() && a2.durable_.state().produced == 4 &&
                                  a2.durable_.state().last_avg == 5.0;
            ok &= reloaded;
            std::printf("[act3] FileStore reopened: produced=%llu last_avg=%.2f (WAL durable: %s)\n",
                        (unsigned long long)a2.durable_.state().produced, a2.durable_.state().last_avg,
                        reloaded ? "yes" : "NO");
        }
        fs::remove_all(dir);  // clean up the temp WAL dir
    }

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
