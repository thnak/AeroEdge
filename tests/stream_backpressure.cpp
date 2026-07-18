// AeroEdge Phase-2 gate — backpressure, NOT shedding (spec 006 §3; the load-bearing Phase-2 gate).
//
// A fast producer against a SMALL ring with a SLOW drain. Each round the producer fills the ring to
// credit depletion (try_push -> false) and STALLS; the consumer then drains only a small budget
// through the flow, retiring frames (returning credit) so the producer can resume. We assert the
// lossless-stall contract of the Quark 024 credit ring, ending in a flow:
//   * dropped == 0    — try_push never sheds; a false just breaks to drain, the frame is re-pushed
//   * missing == 0    — every frame processed exactly once DESPITE repeated stalls (lossless)
//   * stalls  > 0     — backpressure actually engaged (the producer WAS throttled)
//   * occupancy bounded by capacity — resident memory is bounded by the window (022 invariant)
//
// This is deterministic single-thread ping-pong (Quark sample 06 shape): filling to depletion before
// each drain GUARANTEES a stall every round, so the gate is a hard pass/fail, not timing-sensitive.
// The producer uses StreamSink::try_push (the driver's lossless-backpressure primitive) directly, so
// the "never drop on no-credit" property is asserted on the exact call a real driver's run loop uses.
//
// Frame lifetime (006 §4, D5): the flow runs on the slot BEFORE batch.retire() returns its credit.
// Exit code 0 = OK (ctest gate).
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <vector>

#include "aero/core/edge_actor.hpp"
#include "aero/nodes/builtin_nodes.hpp"
#include "aero/sdk/driver.hpp"
#include "quark/core/stream_activation.hpp"

using namespace quark;

struct FlowActor : aero::EdgeActorBase<FlowActor, Sequential> {
    void ingest(const aero::Frame& f) noexcept { process_frame(f); }
};

int main() {
    constexpr std::int64_t kN = 100'000;    // total frames to push through
    constexpr std::uint32_t kCapacity = 16; // SMALL ring: credit depletes fast -> frequent stalls
    constexpr std::uint32_t kBudget = 4;    // SLOW drain: only 4 frames retired per turn

    aero::nodes::DecodeSourceNode source;
    aero::nodes::ScaleNode scale{2.0};
    aero::nodes::SumOutputNode sink_node;
    aero::CompiledFlow flow;
    flow.add(source).add(scale).add(sink_node);

    FlowActor actor;
    actor.bind_flow(flow);

    StreamActivation<aero::Frame>::Config cfg;
    cfg.capacity = kCapacity;
    std::pmr::monotonic_buffer_resource mr;
    StreamActivation<aero::Frame> act(cfg, &mr);
    auto tok = open_stream(act);
    if (!tok) {
        std::printf("open_stream failed\nFAIL\n");
        return 1;
    }
    aero::StreamSink<aero::Frame> sink(std::move(tok.value()));
    auto& ch = act.channel();

    std::vector<std::uint8_t> seen(static_cast<std::size_t>(kN), 0);
    std::int64_t next_push = 0, expected = 0, processed = 0;
    std::int64_t inversions = 0, dup = 0, bad_output = 0, dropped = 0;
    std::uint64_t stalls = 0, max_occupancy = 0;

    while (processed < kN) {
        // FAST producer: push until credit depletes (backpressure) or the source is exhausted.
        // On try_push == false the frame is NOT dropped — we break and drain, then re-push it (§3/D2).
        bool stalled = false;
        while (next_push < kN) {
            const aero::Frame f{next_push};
            if (!sink.try_push(f)) {   // no credit -> stall (the frame stays queued for the next round)
                stalled = true;
                break;
            }
            ++next_push;               // accepted losslessly; advance only on success
        }
        if (stalled) ++stalls;
        if (ch.occupancy() > max_occupancy) max_occupancy = ch.occupancy();

        // SLOW drain: retire only kBudget frames through the flow; retire() returns credit.
        StreamBatch<aero::Frame> batch(ch, kBudget);
        while (const aero::Frame* f = batch.next()) {
            const std::int64_t id = f->raw;
            if (id != expected) ++inversions;
            expected = id + 1;
            if (id >= 0 && id < kN) {
                if (seen[static_cast<std::size_t>(id)]) ++dup;
                else seen[static_cast<std::size_t>(id)] = 1;
            }
            actor.ingest(*f);  // flow runs on the live slot BEFORE retire() (006 §4, D5)
            if (actor.last_output() != static_cast<double>(id) * 2.0) ++bad_output;
            ++processed;
            batch.retire();
        }
    }

    std::int64_t missing = 0;
    for (std::int64_t i = 0; i < kN; ++i)
        if (!seen[static_cast<std::size_t>(i)]) ++missing;

    std::printf("pushed %lld frames through a %u-slot ring (budget %u) into a flow\n",
                (long long)kN, kCapacity, kBudget);
    std::printf("  dropped frames  : %lld  (expected 0 — try_push stalls, never sheds)\n",
                (long long)dropped);
    std::printf("  missing frames  : %lld  (expected 0 — lossless despite stalls)\n", (long long)missing);
    std::printf("  FIFO inversions : %lld  (expected 0)\n", (long long)inversions);
    std::printf("  duplicates      : %lld  (expected 0)\n", (long long)dup);
    std::printf("  bad flow output : %lld  (expected 0)\n", (long long)bad_output);
    std::printf("  backpressure stalls: %llu  (>0 — producer was throttled, losslessly)\n",
                (unsigned long long)stalls);
    std::printf("  max occupancy   : %llu  (bounded by capacity %u)\n",
                (unsigned long long)max_occupancy, kCapacity);

    const bool ok = dropped == 0 && missing == 0 && inversions == 0 && dup == 0 && bad_output == 0 &&
                    processed == kN && stalls > 0 && max_occupancy <= kCapacity;
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
