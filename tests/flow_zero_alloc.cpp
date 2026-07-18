// AeroEdge Phase-1 gate — the flow-execute path performs ZERO heap allocations on the steady path
// (I3 / 003 §4). We override global new/delete to count allocations, warm the reused
// ProcessingContext once (which grows its buffers), then execute the compiled flow many times and
// assert the allocation counter never moves. This is a pass/fail invariant gate, not a benchmark.
#include <cstdio>
#include <cstdlib>
#include <new>

#include "aero/core/compiled_flow.hpp"
#include "aero/nodes/builtin_nodes.hpp"

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

    const bool ok = g_allocs == 0 && checksum > 0.0;
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
