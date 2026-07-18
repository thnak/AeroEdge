// AeroEdge Phase-1 walking skeleton (spec 013 §7).
//
// One EdgeActor runs a compiled 3-node flow (Source → Scale(2) → Sum) triggered by a Command, on the
// REAL Quark engine. This proves the entire "thin over Quark" thesis end-to-end:
//   * a domain actor is a quark::Actor subclass via EdgeActorBase (001)
//   * a Command (ReceivePacket) triggers a Flow that walks a compiled DAG (002/004)
//   * the flow threads a ProcessingContext, stages output, emits events (003)
//   * state commits and is read back via ask; Sequential + mailbox FIFO make it deterministic
//
// Bring-up mirrors Quark sample 01 exactly (MessagePool → Activation → Engine → register → Router).
// Exit code 0 = OK (ctest gate).
#include <cstdio>
#include <memory>

#include "aero/core/edge_actor.hpp"
#include "aero/nodes/builtin_nodes.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/spawn.hpp"

using namespace quark;

// --- Messages ---------------------------------------------------------------------------------
struct ReceivePacket {
    std::int64_t raw;
};
struct GetLast {};    // ask → last committed output (double)
struct GetEvents {};  // ask → total events published (long)
struct GetFrames {};  // ask → frames processed (long)

// --- The domain actor: EdgeActorBase supplies the Command→Flow→Event glue; this adds the protocol.
struct EdgeActor : aero::EdgeActorBase<EdgeActor, Sequential> {
    using protocol = Protocol<ReceivePacket, Ask<GetLast, double>, Ask<GetEvents, long>,
                              Ask<GetFrames, long>>;

    void handle(const ReceivePacket& cmd) noexcept { process_frame(aero::Frame{cmd.raw}); }
    void handle(const Ask<GetLast, double>& m) noexcept { m.respond(last_output()); }
    void handle(const Ask<GetEvents, long>& m) noexcept { m.respond(events_published()); }
    void handle(const Ask<GetFrames, long>& m) noexcept { m.respond(frames_processed()); }
};

int main() {
    // --- Compile the flow ONCE (deploy-time): Source → Scale(2) → Sum. -------------------------
    aero::nodes::DecodeSourceNode source;
    aero::nodes::ScaleNode scale{2.0};
    aero::nodes::SumOutputNode sink;
    aero::CompiledFlow flow;
    flow.add(source).add(scale).add(sink);

    // --- Bring-up (Quark sample-01 shape). -----------------------------------------------------
    detail::MessagePool pool(1024);
    EdgeActor actor;
    actor.bind_flow(flow);  // wire before start(), so the ref is live on first handler run

    auto activation = std::make_unique<Activation>(&actor, EdgeActor::dispatch_table(), pool.sink());
    Engine<> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});
    register_actor<EdgeActor>(eng, /*key*/ 7, *activation);

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<EdgeActor> ref = router.get<EdgeActor>(7);
    eng.start();

    // --- Drive it: feed 1..100. Each ReceivePacket(n) → flow → output = n*2, one event. --------
    constexpr int kN = 100;
    for (int n = 1; n <= kN; ++n) {
        ref.tell(ReceivePacket{n});
    }

    // Sequential + mailbox FIFO: these asks observe ALL prior tells.
    result<double> last = block_on(ref.ask<double>(GetLast{}));      // expect 100*2 = 200
    result<long> events = block_on(ref.ask<long>(GetEvents{}));      // expect 100
    result<long> frames = block_on(ref.ask<long>(GetFrames{}));      // expect 100

    eng.stop();

    const double got_last = last.has_value() ? last.value() : -1.0;
    const long got_events = events.has_value() ? events.value() : -1;
    const long got_frames = frames.has_value() ? frames.value() : -1;

    std::printf("last output   : %.1f  (expected 200.0)\n", got_last);
    std::printf("events emitted : %ld   (expected 100)\n", got_events);
    std::printf("frames processed: %ld  (expected 100)\n", got_frames);

    const bool ok = got_last == 200.0 && got_events == 100 && got_frames == 100;
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
