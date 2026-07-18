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

// Command: hot-reload the bound flow (009 §4). Carries a pointer to a new CompiledFlow that the
// Runtime compiled + validated OFF to the side (the old flow still running). Because this is a normal
// mailbox Command on a Sequential actor, it lands in FIFO order AFTER every in-flight frame Command and
// BEFORE every later one (Quark 001/ADR-002 mailbox FIFO): the swap happens at a quiescent point
// BETWEEN messages — exactly Quark's Hot-Leaf pointer publish (ADR-008) — so no in-flight Command is
// dropped or duplicated. The pointed-to CompiledFlow is owned by the Runtime, kept alive across the
// swap (P2). This is why AeroEdge needs no explicit quiesce(Drain): on a Sequential actor that drain
// is a no-op resolving synchronously between messages (Quark 015), and the mailbox already IS that
// between-messages point — so a plain Command is the whole mechanism.
struct ReloadFlow {
    const aero::CompiledFlow* flow = nullptr;
};

// Ask query + reply: a single snapshot of the actor's observable counters (009 status surface).
struct GetStatus {};
struct FlowStatus {
    long frames = 0;         // frames processed through the flow
    long events = 0;         // events published post-commit
    double last = 0.0;       // last committed output
    bool failed = false;
    double output_sum = 0.0; // running sum of committed outputs (hot-reload tally: proves which flow
                             // ran each frame — a swap that dropped/dup'd/mis-ordered a frame changes it)
    long reloads = 0;        // count of ReloadFlow swaps applied (versioning observability, 009 §6)
};

// The one deployed actor type. Sequential → single executor, mailbox FIFO makes status observe all
// prior frames (I2). The Runtime binds a CompiledFlow into it before start().
struct FlowActor : aero::EdgeActorBase<FlowActor, quark::Sequential> {
    using protocol = quark::Protocol<ReceiveFrame, ReloadFlow, quark::Ask<GetStatus, FlowStatus>>;

    void handle(const ReceiveFrame& cmd) noexcept {
        process_frame(aero::Frame{cmd.raw});
        output_sum_ += last_output();  // tally after commit — the last committed output for this frame
    }

    // Hot-reload swap (009 §4 step 3): repoint the bound flow. Runs on the actor's own executor,
    // between messages (I2) — no frame is mid-flow, so the swap can never tear an in-flight execution.
    void handle(const ReloadFlow& cmd) noexcept {
        if (cmd.flow != nullptr) {
            bind_flow(*cmd.flow);
            ++reloads_;
        }
    }

    void handle(const quark::Ask<GetStatus, FlowStatus>& m) noexcept {
        m.respond(FlowStatus{frames_processed(), events_published(), last_output(), last_failed(),
                             output_sum_, reloads_});
    }

private:
    double output_sum_ = 0.0;
    long reloads_ = 0;
};

}  // namespace aero::runtime
