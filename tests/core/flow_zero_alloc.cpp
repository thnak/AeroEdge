// AeroEdge Phase-1 gate — the flow-execute path performs ZERO heap allocations on the steady path
// (I3 / 003 §4). We override global new/delete to count allocations, warm the reused
// ProcessingContext once (which grows its buffers), then execute the compiled flow many times and
// assert the allocation counter never moves. This is a pass/fail invariant gate, not a benchmark.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "aero/core/compiled_flow.hpp"
#include "aero/nodes/builtin_nodes.hpp"
#include "aero/nodes/loop_nodes.hpp"

namespace {
volatile bool g_count = false;  // only count inside the measured window
long g_allocs = 0;
}  // namespace

void* operator new(std::size_t n) {
    if (g_count) ++g_allocs;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main() {
    aero::nodes::DecodeSourceNode source;
    aero::nodes::ScaleNode scale{2.0};
    aero::nodes::SumOutputNode sink;
    aero::CompiledFlow flow;
    flow.add(source).add(scale).add(sink);

    aero::ProcessingContext ctx;
    ctx.reserve(/*tags*/ 8, /*out*/ 8, /*events*/ 8);  // cold pre-allocation (003 §4)

    // Warm-up: one execution so any first-touch growth happens before we start counting.
    aero::Frame warm{1};
    ctx.reset(&warm);
    flow.execute(ctx);

    // Measured window: N executions, reusing the context (reset = clear, keep capacity).
    constexpr long kN = 100000;
    g_allocs = 0;
    g_count = true;
    double checksum = 0.0;
    for (long i = 0; i < kN; ++i) {
        aero::Frame f{i};
        ctx.reset(&f);
        flow.execute(ctx);
        checksum += ctx.output.empty() ? 0.0 : ctx.output.back();
    }
    g_count = false;

    std::printf("executions     : %ld\n", kN);
    std::printf("heap allocations: %ld  (expected 0)\n", g_allocs);
    std::printf("checksum        : %.0f  (non-zero => work happened)\n", checksum);

    bool ok = g_allocs == 0 && checksum > 0.0;
    std::printf("%s\n", ok ? "OK" : "FAIL");

    // 020 §8.6: a loop's own gate — this file has never exercised the "same step called N times in one
    // Command" shape before loops existed, so it needs its own case, not just an assumption that
    // composing 0-alloc nodes stays 0-alloc under a jump-back. Minimal valid loop: loop_back's own step
    // jumps to ITSELF (an empty body — just increments a counter), so ~1000 loop_back passes all happen
    // inside ONE ProcessingContext::reset() + execute() call, not 1000 outer executions.
    {
        using aero::nodes::LoopBackNode;
        using aero::nodes::LoopStartNode;

        LoopStartNode loop_start(LoopStartNode::compile("0"), "i", /*max_iterations*/ 2000,
                                  std::chrono::milliseconds(5000));
        LoopBackNode loop_back("i", LoopBackNode::compile("1"), LoopBackNode::compile("999"));
        aero::CompiledFlow loop_flow;
        loop_flow.add(loop_start).add(loop_back, /*required_label*/ {}, /*loop_back_target*/ 1);

        aero::ProcessingContext lctx;
        lctx.reserve(/*tags*/ 8, /*out*/ 8, /*events*/ 8);

        aero::Frame warm{1};
        lctx.reset(&warm);
        loop_flow.execute(lctx);  // warm-up: first-touch growth happens here, before counting starts

        g_allocs = 0;
        g_count = true;
        aero::Frame f{2};
        lctx.reset(&f);
        loop_flow.execute(lctx);  // ONE Command, ~1000 internal loop_back passes
        g_count = false;

        double final_i = -1.0;
        for (const auto& t : lctx.tags) {
            if (t.name == "i") final_i = t.value;
        }

        std::printf("loop passes     : ~1000, in ONE Command\n");
        std::printf("heap allocations: %ld  (expected 0)\n", g_allocs);
        std::printf("final counter   : %.0f  (expected 1000)\n", final_i);

        const bool loop_ok = g_allocs == 0 && final_i == 1000.0;
        std::printf("%s\n", loop_ok ? "OK (loop)" : "FAIL (loop)");
        ok = ok && loop_ok;
    }

    return ok ? 0 : 1;
}
