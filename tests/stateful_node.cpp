// AeroEdge Phase-3 invariant gate — the stateful-node rule (007 §6, resolving 005 §8): "nodes
// compute, actors remember". A node holds only TRANSIENT per-instance state it is willing to lose on
// deactivation/migration; DURABLE accumulation is promoted to actor state (tier 1). This is a pure,
// engine-free gate for that split: it drives a MovingAverageNode directly through a reused
// ProcessingContext, then simulates a restart by discarding the node and starting a FRESH one.
//
//   1. Warm a MovingAverageNode<4> with samples → the window fills, the average reflects the window.
//   2. Alongside it, a "promoted" durable counter (standing in for tier-1 actor state) accumulates.
//   3. RESTART: construct a brand-new node (transient window LOST → cold, depth 0), but carry the
//      durable counter forward (it survived). Feed one sample: the node rebuilds cold (depth 1,
//      average == the single new sample), while the durable counter continues from where it was.
//
// S6 in one assertion: transient node state does NOT survive the restart; promoted actor state does.
// Exit code 0 = OK (ctest gate). Prints "FAIL" on any mismatch.
#include <cstdint>
#include <cstdio>

#include "aero/nodes/builtin_nodes.hpp"
#include "aero/sdk/processing_context.hpp"

using aero::Frame;
using aero::ProcessingContext;
using aero::Tag;
using aero::nodes::MovingAverageNode;

// Feed one sample through the node via the context and return the average it staged.
static double feed(MovingAverageNode<4>& node, ProcessingContext& ctx, double sample) {
    Frame f{static_cast<std::int64_t>(sample)};
    ctx.reset(&f);
    ctx.tags.push_back(Tag{"raw", sample});  // one working-set tag = one sample (a Source would decode)
    node.process(ctx);
    return ctx.output.back();
}

int main() {
    bool ok = true;
    ProcessingContext ctx;
    ctx.reserve(/*tags*/ 4, /*out*/ 4, /*events*/ 4);

    // Tier-1 stand-in: a durable counter the ACTOR owns and would persist (007 §6). It is NOT held by
    // the node; here it simply survives the node's destruction to model "promoted to actor state".
    std::uint64_t produced = 0;

    // --- Warm the node (transient window fills). Samples 10,20,30,40,50 → last-4 avg = 35. ---------
    {
        MovingAverageNode<4> node;
        double avg = 0.0;
        for (double s : {10.0, 20.0, 30.0, 40.0, 50.0}) {
            avg = feed(node, ctx, s);
            ++produced;  // actor remembers each production
        }
        const bool warm = node.warm_samples() == 4 && avg == 35.0;  // (20+30+40+50)/4
        ok &= warm && produced == 5;
        std::printf("[warm]    window depth=%zu avg=%.1f (expected 4, 35.0); produced=%llu\n",
                    node.warm_samples(), avg, (unsigned long long)produced);
    }  // node destroyed = deactivation/migration: its TRANSIENT window is gone

    // --- RESTART: a FRESH node starts cold; the durable counter carried over. ----------------------
    {
        MovingAverageNode<4> node;                    // brand-new instance (007 §6: rebuilt cold)
        ok &= node.warm_samples() == 0;               // transient state did NOT survive
        const double avg = feed(node, ctx, 100.0);    // 1 sample → depth 1, avg == the sample
        ++produced;                                   // durable counter CONTINUES (11? no — 6)
        const bool cold_rebuild = node.warm_samples() == 1 && avg == 100.0;
        ok &= cold_rebuild && produced == 6;          // actor state survived: 5 + 1
        std::printf("[restart] fresh window depth=%zu avg=%.1f (expected 1, 100.0); produced=%llu "
                    "(durable survived: %s; transient rebuilt cold: %s)\n",
                    node.warm_samples(), avg, (unsigned long long)produced,
                    produced == 6 ? "yes" : "NO", cold_rebuild ? "yes" : "NO");
    }

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
