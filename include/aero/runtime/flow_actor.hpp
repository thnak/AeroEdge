// AeroEdge runtime — the canonical deployed actor (generalizes the Phase-1/2 FlowActor).
//
// Phase-1's walking_skeleton and Phase-2's stream_ingest each declared a bespoke actor with an ad-hoc
// protocol. A Runtime that deploys ARBITRARY declarative Applications needs ONE reusable actor type:
// EdgeActorBase supplies the Command→Flow→Event glue (001), and this adds the minimal protocol so the
// Runtime can DRIVE it (one Command per inbound frame, `tell`ed by the driver bridge or the API) and
// OBSERVE it (one Ask returning a status snapshot). The flow itself is data — bound via bind_flow —
// so no per-Application actor subclass is ever generated (009 §2: topology is data, logic is compiled).
#pragma once

#include <cstdint>

#include "aero/core/edge_actor.hpp"
#include "aero/sdk/processing_context.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"  // quark::Ask
#include "quark/core/dispatch.hpp"   // quark::Protocol

namespace aero::runtime {

// Command: one inbound frame to run through the bound flow. The driver bridge converts each streamed
// frame into a ReceiveFrame tell (mailbox FIFO + Sequential → deterministic in-order execution, I2).
struct ReceiveFrame {
    std::int64_t raw = 0;
};

// Ask query + reply: a single snapshot of the actor's observable counters (009 status surface).
struct GetStatus {};
struct FlowStatus {
    long frames = 0;    // frames processed through the flow
    long events = 0;    // events published post-commit
    double last = 0.0;  // last committed output
    bool failed = false;
};

// The one deployed actor type. Sequential → single executor, mailbox FIFO makes status observe all
// prior frames (I2). The Runtime binds a CompiledFlow into it before start().
struct FlowActor : aero::EdgeActorBase<FlowActor, quark::Sequential> {
    using protocol = quark::Protocol<ReceiveFrame, quark::Ask<GetStatus, FlowStatus>>;

    void handle(const ReceiveFrame& cmd) noexcept { process_frame(aero::Frame{cmd.raw}); }

    void handle(const quark::Ask<GetStatus, FlowStatus>& m) noexcept {
        m.respond(FlowStatus{frames_processed(), events_published(), last_output(), last_failed()});
    }
};

}  // namespace aero::runtime
