// AeroEdge Phase-6 gate — the low-code ExprRule DSL (spec 008 §6, resolving 005 §8).
//
// Proves the expression grammar (comparisons, boolean, arithmetic, tag refs, precedence) evaluates
// correctly over crafted working-set tags and routes as specified — "if expr holds, emit AlarmRaised
// and Stop", else Continue. Proves a MALFORMED expression is rejected at compile (configure) with a
// clear error and NO crash. Includes a 0-alloc-eval invariant gate (parse-once → hot eval allocates
// nothing, N1/N3), and an end-to-end deploy through the Runtime (accept a valid rule flow, reject a
// malformed one). Exit code 0 = OK.
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "aero/nodes/expr_rule_node.hpp"
#include "aero/runtime/runtime.hpp"
#include "aero/sdk/processing_context.hpp"

namespace {
volatile bool g_count = false;
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

using aero::nodes::ExprRuleNode;

namespace {

bool g_pass = true;
void check(const char* what, bool ok) {
    std::printf("%-46s : %s\n", what, ok ? "ok" : "BAD");
    g_pass = g_pass && ok;
}

// Build a context whose working set is the given (name,value) tags. Names are string literals (static
// storage), matching the Tag borrowing contract.
aero::ProcessingContext make_ctx(std::initializer_list<aero::Tag> tags) {
    aero::ProcessingContext ctx;
    for (auto t : tags) ctx.tags.push_back(t);
    return ctx;
}

// Evaluate `expr` over `tags` and return the routing result + how many events were staged.
struct Outcome { aero::NodeResult result; std::size_t events; };
Outcome run(const std::string& expr, std::initializer_list<aero::Tag> tags) {
    ExprRuleNode node(ExprRuleNode::compile(expr), "AlarmRaised");
    auto ctx = make_ctx(tags);
    const aero::NodeResult r = node.process(ctx);
    return {r, ctx.events.size()};
}

bool fires(const std::string& expr, std::initializer_list<aero::Tag> tags) {
    auto o = run(expr, tags);
    return o.result == aero::NodeResult::Stop && o.events == 1;
}
bool passes(const std::string& expr, std::initializer_list<aero::Tag> tags) {
    auto o = run(expr, tags);
    return o.result == aero::NodeResult::Continue && o.events == 0;
}

}  // namespace

int main() {
    // --- comparisons ------------------------------------------------------------------------------
    check("raw > 50 fires @ raw=60",  fires("raw > 50", {{"raw", 60}}));
    check("raw > 50 passes @ raw=40", passes("raw > 50", {{"raw", 40}}));
    check("raw >= 10 && raw < 20 fires @ 15", fires("raw >= 10 && raw < 20", {{"raw", 15}}));
    check("raw >= 10 && raw < 20 passes @ 25", passes("raw >= 10 && raw < 20", {{"raw", 25}}));
    check("raw == 0 fires @ 0",  fires("raw == 0", {{"raw", 0}}));
    check("raw != 0 passes @ 0", passes("raw != 0", {{"raw", 0}}));

    // --- arithmetic + precedence (mul before add, cmp after arithmetic) ---------------------------
    check("raw * 2 + 1 > 10 fires @ raw=5",  fires("raw * 2 + 1 > 10", {{"raw", 5}}));   // 11 > 10
    check("raw * 2 + 1 > 10 passes @ raw=4", passes("raw * 2 + 1 > 10", {{"raw", 4}}));  // 9  > 10 (no)
    check("(raw + 1) * 2 == 12 fires @ raw=5", fires("(raw + 1) * 2 == 12", {{"raw", 5}}));
    check("guarded div raw/0 passes",         passes("raw / 0 > 1", {{"raw", 100}}));    // /0 -> 0

    // --- boolean + unary + or ---------------------------------------------------------------------
    check("!(raw == 0) fires @ raw=5",       fires("!(raw == 0)", {{"raw", 5}}));
    check("raw < 0 || raw > 100 fires @ 150", fires("raw < 0 || raw > 100", {{"raw", 150}}));
    check("raw < 0 || raw > 100 passes @ 50", passes("raw < 0 || raw > 100", {{"raw", 50}}));
    check("-raw > -10 fires @ raw=5",         fires("-raw > -10", {{"raw", 5}}));         // -5 > -10

    // --- tag("name") reference form + multiple tags -----------------------------------------------
    check("tag(\"temp\") > 100 fires @ temp=120",
          fires("tag(\"temp\") > 100", {{"temp", 120}, {"raw", 0}}));
    check("temp - raw > 5 fires",  fires("temp - raw > 5", {{"temp", 20}, {"raw", 10}}));
    check("missing tag reads 0 -> passes", passes("ghost > 1", {{"raw", 99}}));

    // --- malformed expressions rejected at compile, no crash --------------------------------------
    for (const char* bad : {"raw >", "(1 + 2", "raw & 3", "* 5", "tag(raw)", "1 2 3", ""}) {
        auto prog = ExprRuleNode::compile(bad);
        const bool rejected = !prog.ok && !prog.error.empty();
        std::printf("malformed \"%s\" -> %s (%s)\n", bad, rejected ? "rejected" : "ACCEPTED?!",
                    prog.error.c_str());
        check("malformed rejected", rejected);
        // A node built from a bad program must not crash — it fails closed (Error), never Stop.
        ExprRuleNode node(std::move(prog), "AlarmRaised");
        auto ctx = make_ctx({{"raw", 1}});
        check("bad program -> process() Error", node.process(ctx) == aero::NodeResult::Error);
    }

    // --- 0-alloc eval gate (parse-once, N1/N3): warm once, then eval many times, assert 0 allocs ---
    {
        ExprRuleNode node(ExprRuleNode::compile("raw * 2 + 1 > 1000000"), "AlarmRaised");  // always false
        auto ctx = make_ctx({{"raw", 3}});
        node.process(ctx);  // warm
        constexpr long kN = 200000;
        g_allocs = 0;
        g_count = true;
        long fired = 0;
        for (long i = 0; i < kN; ++i) {
            if (node.process(ctx) == aero::NodeResult::Stop) ++fired;
        }
        g_count = false;
        std::printf("eval x%ld : heap allocations=%ld (expect 0), fired=%ld (expect 0)\n",
                    kN, g_allocs, fired);
        check("hot eval is 0-alloc", g_allocs == 0);
    }

    // --- end-to-end: deploy a rule flow through the Runtime, and reject a malformed rule at deploy --
    {
        aero::runtime::Runtime rt;
        const char* good = R"({
          "name": "rule_flow", "version": "1", "actor": { "kind": "edge", "key": 5 },
          "flow": [
            { "type_id": "aero.source.decode" },
            { "type_id": "aero.rule.expr", "config": { "expr": "raw > 50", "alarm": "HighAlarm" } },
            { "type_id": "aero.output.sum" }
          ]
        })";
        check("deploy valid rule flow", rt.deploy_json(good).has_value());
        (void)rt.undeploy();

        const char* bad = R"({
          "name": "rule_flow", "version": "1", "actor": { "kind": "edge", "key": 5 },
          "flow": [
            { "type_id": "aero.source.decode" },
            { "type_id": "aero.rule.expr", "config": { "expr": "raw >" } },
            { "type_id": "aero.output.sum" }
          ]
        })";
        auto r = rt.deploy_json(bad);
        check("deploy rejects malformed rule", !r.has_value());
        if (!r) std::printf("   deploy reject: %s\n", r.error().c_str());
    }

    std::printf("%s\n", g_pass ? "OK" : "FAIL");
    return g_pass ? 0 : 1;
}
