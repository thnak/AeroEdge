// AeroEdge 019 §2/§3 gate — the flow graph model (edges[], topo-sort, branch routing). Two halves:
//
//   PART A (mirrors tests/core/flow_validation.cpp's expect_reject style, via Runtime::deploy_json):
//     schema/compiler validation over `edges[]` — cycle, duplicate id, unknown edge endpoint,
//     conflicting branch labels are all rejected at deploy (P1), never a crash, never a half-deploy.
//
//   PART B (mirrors tests/mes/mes_outbox.cpp's direct-CompiledFlow style): builds an Application in
//     C++ (not JSON) and calls flow_compiler::compile_flow()/CompiledFlow::execute() directly so the
//     resulting ProcessingContext can be inspected — proves fan-out runs BOTH branches (merge is free,
//     no new machinery) and aero.flow.switch runs ONLY the matching branch (required_label skip).
//
// Exit code 0 = OK.
#include <cstdio>
#include <string>

#include "aero/nodes/set_node.hpp"
#include "aero/runtime/flow_compiler.hpp"
#include "aero/runtime/runtime.hpp"
#include "aero/schema/application.hpp"

using aero::schema::Application;
using aero::schema::EdgeSpec;
using aero::schema::NodeSpec;

namespace {

int failures = 0;

// ---- PART A: deploy-time rejection (mirrors flow_validation.cpp) ---------------------------------

void expect_reject(const char* label, const std::string& json) {
    aero::runtime::Runtime rt;
    auto r = rt.deploy_json(json);
    const bool rejected = !r.has_value();
    const bool clean = !rt.deployed();
    std::printf("  %-28s : %s%s  %s\n", label, rejected ? "rejected" : "ACCEPTED(!)",
                clean ? "" : " +LEFT-DEPLOYED(!)", rejected ? r.error().c_str() : "");
    if (!rejected || !clean) ++failures;
}

void expect_accept(const char* label, const std::string& json) {
    aero::runtime::Runtime rt;
    auto r = rt.deploy_json(json);
    std::printf("  %-28s : %s  %s\n", label, r.has_value() ? "accepted" : "REJECTED(!)",
                r.has_value() ? "" : r.error().c_str());
    if (!r.has_value()) ++failures;
}

}  // namespace

int main() {
    std::printf("flow graph (019 sec2/3, reject invalid graphs / route branches correctly):\n");

    // A cycle (a <-> b) alongside a direct src->out edge so has_source/has_output both pass and the
    // cycle-detection path (not an earlier unrelated check) is what actually rejects this.
    expect_reject("cycle", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"a","type_id":"aero.transform.scale","config":{"factor":1}},
        {"id":"b","type_id":"aero.transform.scale","config":{"factor":1}},
        {"id":"out","type_id":"aero.output.sum"}],
      "edges":[
        {"from":"src","to":"a"},{"from":"a","to":"b"},{"from":"b","to":"a"},{"from":"src","to":"out"}]})");

    expect_reject("duplicate-id", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"src","type_id":"aero.output.sum"}],
      "edges":[{"from":"src","to":"src"}]})");

    expect_reject("unknown-edge-endpoint", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"out","type_id":"aero.output.sum"}],
      "edges":[{"from":"src","to":"out"},{"from":"src","to":"nope"}]})");

    expect_reject("conflicting-branch-labels", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"sw","type_id":"aero.flow.switch","config":{"expr":"raw > 0"}},
        {"id":"x","type_id":"aero.output.sum"}],
      "edges":[
        {"from":"src","to":"sw"},{"from":"sw","from_port":"true","to":"x"},{"from":"src","to":"x"}]})");

    // Two independent switch nodes: ctx.active_branch is a single field, not a stack (019 sec10) —
    // a second branch-producing node must be rejected at deploy, never silently misroute.
    expect_reject("two-branch-sources", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"sw1","type_id":"aero.flow.switch","config":{"expr":"raw > 0"}},
        {"id":"sw2","type_id":"aero.flow.switch","config":{"expr":"raw > 0"}},
        {"id":"a","type_id":"aero.output.sum"},
        {"id":"b","type_id":"aero.output.sum"}],
      "edges":[
        {"from":"src","to":"sw1"},{"from":"src","to":"sw2"},
        {"from":"sw1","from_port":"true","to":"a"},{"from":"sw2","from_port":"true","to":"b"}]})");

    // 020 §7.5: two unconditional aero.transform.set nodes writing the same tag — order-dependent,
    // rejected at deploy rather than letting whichever runs last silently win.
    expect_reject("tag-writer-collision-graph", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"s1","type_id":"aero.transform.set","config":{"tag":"x","expr":"raw"}},
        {"id":"s2","type_id":"aero.transform.set","config":{"tag":"x","expr":"raw*2"}},
        {"id":"out","type_id":"aero.output.sum"}],
      "edges":[
        {"from":"src","to":"s1"},{"from":"src","to":"s2"},
        {"from":"s1","to":"out"},{"from":"s2","to":"out"}]})");

    // Same collision, but in LINEAR (edges-empty) mode — every node is unconditional there too.
    expect_reject("tag-writer-collision-linear", R"({
      "name":"bad","version":"1",
      "flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.set","config":{"tag":"y","expr":"raw"}},
        {"type_id":"aero.transform.set","config":{"tag":"y","expr":"raw*2"}},
        {"type_id":"aero.output.sum"}]})");

    // 020 §7.5: a set node colliding with a Source node's own fixed tag ("raw") is the same bug —
    // whichever runs last silently wins the value everything downstream reads as "raw".
    expect_reject("tag-writer-collision-vs-source", R"({
      "name":"bad","version":"1",
      "flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.set","config":{"tag":"raw","expr":"raw*2"}},
        {"type_id":"aero.output.sum"}]})");

    // 020 §7.5: two set nodes on MUTUALLY EXCLUSIVE switch branches writing the same tag is legal — at
    // most one of them ever runs in a given Command, so there's no real collision.
    expect_accept("tag-writer-mutually-exclusive", R"({
      "name":"ok","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"sw","type_id":"aero.flow.switch","config":{"expr":"raw > 100"}},
        {"id":"s1","type_id":"aero.transform.set","config":{"tag":"x","expr":"raw*10"}},
        {"id":"s2","type_id":"aero.transform.set","config":{"tag":"x","expr":"raw*1"}},
        {"id":"out","type_id":"aero.output.sum"}],
      "edges":[
        {"from":"src","to":"sw"},
        {"from":"sw","from_port":"true","to":"s1"},{"from":"sw","from_port":"false","to":"s2"},
        {"from":"s1","to":"out"},{"from":"s2","to":"out"}]})");

    // 020 §4.3: aero.output.sum is terminal — nothing may follow it, in EITHER flow shape.
    expect_reject("terminal-not-last-linear", R"({
      "name":"bad","version":"1",
      "flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.output.sum"},
        {"type_id":"aero.transform.scale","config":{"factor":2}}]})");

    expect_reject("terminal-has-outgoing-edge", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"sum","type_id":"aero.output.sum"},
        {"id":"scale","type_id":"aero.transform.scale","config":{"factor":2}}],
      "edges":[{"from":"src","to":"sum"},{"from":"sum","to":"scale"}]})");

    // A NON-terminal Output (aero.output.mes) staying mid-chain is still legal — the flag must actually
    // distinguish node types, not blanket-reject anything after any Output.
    expect_accept("non-terminal-output-mid-chain", R"({
      "name":"ok","version":"1",
      "flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.output.mes","config":{"line":"L1"}},
        {"type_id":"aero.output.sum"}]})");

    // Valid graph, no branching — same shape as hello_flow.json but expressed as a graph, proving the
    // edges[] path accepts a well-formed linear graph too, not just the array-order fallback.
    expect_accept("valid-linear-graph", R"({
      "name":"ok","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"out","type_id":"aero.output.sum"}],
      "edges":[{"from":"src","to":"out"}]})");

    // 020 §8: loop rejections — a malformed/unsafe/unsupported loop is caught at deploy, never a
    // silently-wrong or hung runtime.
    expect_reject("loop-max-iterations-zero", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"ls","type_id":"aero.flow.loop_start",
         "config":{"counter_tag":"i","start_expr":"0","max_iterations":0,"max_duration_ms":1000}},
        {"id":"lb","type_id":"aero.flow.loop_back",
         "config":{"counter_tag":"i","step_expr":"1","end_expr":"9"}},
        {"id":"out","type_id":"aero.output.sum"}],
      "edges":[
        {"from":"src","to":"ls"},{"from":"ls","to":"lb"},
        {"from":"lb","from_port":"loop_back","to":"lb"},{"from":"lb","to":"out"}]})");

    expect_reject("loop-missing-max-duration", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"ls","type_id":"aero.flow.loop_start",
         "config":{"counter_tag":"i","start_expr":"0","max_iterations":20}},
        {"id":"lb","type_id":"aero.flow.loop_back",
         "config":{"counter_tag":"i","step_expr":"1","end_expr":"9"}},
        {"id":"out","type_id":"aero.output.sum"}],
      "edges":[
        {"from":"src","to":"ls"},{"from":"ls","to":"lb"},
        {"from":"lb","from_port":"loop_back","to":"lb"},{"from":"lb","to":"out"}]})");

    // Two loop_back edges — ctx.loop_iterations_remaining/loop_deadline are single fields, not a
    // stack, same structural reason "two branch sources" is rejected (019 sec10 precedent).
    expect_reject("loop-two-loop-back-edges", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"ls","type_id":"aero.flow.loop_start",
         "config":{"counter_tag":"i","start_expr":"0","max_iterations":20,"max_duration_ms":1000}},
        {"id":"body","type_id":"aero.transform.scale","config":{"factor":1}},
        {"id":"lb","type_id":"aero.flow.loop_back",
         "config":{"counter_tag":"i","step_expr":"1","end_expr":"9"}},
        {"id":"out","type_id":"aero.output.sum"}],
      "edges":[
        {"from":"src","to":"ls"},{"from":"ls","to":"body"},{"from":"body","to":"lb"},
        {"from":"lb","from_port":"loop_back","to":"body"},
        {"from":"lb","from_port":"loop_back","to":"ls"},
        {"from":"lb","to":"out"}]})");

    // A loop_start reached only via a "true"/"false" edge breaks its "always runs exactly once before
    // loop_back" assumption (020 sec8.7) — branch labels don't propagate transitively.
    expect_reject("loop-nested-in-branch", R"({
      "name":"bad","version":"1",
      "flow":[
        {"id":"src","type_id":"aero.source.decode"},
        {"id":"sw","type_id":"aero.flow.switch","config":{"expr":"raw > 0"}},
        {"id":"ls","type_id":"aero.flow.loop_start",
         "config":{"counter_tag":"i","start_expr":"0","max_iterations":20,"max_duration_ms":1000}},
        {"id":"lb","type_id":"aero.flow.loop_back",
         "config":{"counter_tag":"i","step_expr":"1","end_expr":"9"}},
        {"id":"out","type_id":"aero.output.sum"}],
      "edges":[
        {"from":"src","to":"sw"},{"from":"sw","from_port":"true","to":"ls"},{"from":"ls","to":"lb"},
        {"from":"lb","from_port":"loop_back","to":"lb"},{"from":"lb","to":"out"}]})");

    // A loop pair with no edges[] at all — array-order linear mode has no from_port concept, so a
    // loop_back's jump target could never resolve (020 sec8).
    expect_reject("loop-requires-graph-mode", R"({
      "name":"bad","version":"1",
      "flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.flow.loop_start",
         "config":{"counter_tag":"i","start_expr":"0","max_iterations":20,"max_duration_ms":1000}},
        {"type_id":"aero.flow.loop_back",
         "config":{"counter_tag":"i","step_expr":"1","end_expr":"9"}},
        {"type_id":"aero.output.sum"}]})");

    // ---- PART B: execution semantics (direct CompiledFlow, mirrors mes_outbox.cpp's ACT0 style) ----
    aero::NodeRegistry node_reg;
    aero::DriverRegistry driver_reg;
    aero::runtime::register_builtins(node_reg, driver_reg);

    // Fan-out: src -> {crc, mean} -> out. Both branches must run (merge is free — out just sees
    // whatever's in ctx.tags/output by the time it's scheduled, no matter how many branches fed it).
    {
        Application app;
        app.name = "fanout";
        app.version = "0.1.0";
        app.flow = {
            NodeSpec{.id = "src", .type_id = "aero.source.decode"},
            NodeSpec{.id = "crc", .type_id = "aero.transform.crc"},
            NodeSpec{.id = "mean", .type_id = "aero.transform.mean"},
            NodeSpec{.id = "out", .type_id = "aero.output.sum"},
        };
        app.edges = {
            EdgeSpec{.from = "src", .to = "crc"},
            EdgeSpec{.from = "src", .to = "mean"},
            EdgeSpec{.from = "crc", .to = "out"},
            EdgeSpec{.from = "mean", .to = "out"},
        };
        auto plan = aero::runtime::compile_flow(app, node_reg);
        bool ok = plan.has_value();
        if (ok) {
            aero::ProcessingContext ctx;
            aero::Frame frame{5};
            ctx.reset(&frame);
            plan->flow.execute(ctx);
            bool crc_ran = false;
            for (const auto& t : ctx.tags) if (t.name == "crc16") crc_ran = true;
            bool mean_ran = false;
            for (const auto& e : ctx.events) if (e.type == "Mean") mean_ran = true;
            const bool out_ran = !ctx.output.empty();
            ok = crc_ran && mean_ran && out_ran;
            std::printf("  %-28s : crc=%d mean=%d out=%d %s\n", "fanout-both-branches-ran", crc_ran,
                        mean_ran, out_ran, ok ? "ok" : "FAIL");
        } else {
            std::printf("  %-28s : compile REJECTED(!) %s\n", "fanout-both-branches-ran", plan.error().c_str());
        }
        if (!ok) ++failures;
    }

    // aero.flow.switch: src -> sw -> {hi (x10, "true"), lo (x1, "false")} -> out. Only the matching
    // branch's scale factor should show up in the final sum.
    {
        auto run_switch = [&](double raw, double expected_out) {
            Application app;
            app.name = "switch";
            app.version = "0.1.0";
            app.flow = {
                NodeSpec{.id = "src", .type_id = "aero.source.decode"},
                NodeSpec{.id = "sw", .type_id = "aero.flow.switch",
                         .config = {{"expr", "raw > 100"}}},
                NodeSpec{.id = "hi", .type_id = "aero.transform.scale", .config = {{"factor", 10.0}}},
                NodeSpec{.id = "lo", .type_id = "aero.transform.scale", .config = {{"factor", 1.0}}},
                NodeSpec{.id = "out", .type_id = "aero.output.sum"},
            };
            app.edges = {
                EdgeSpec{.from = "src", .to = "sw"},
                EdgeSpec{.from = "sw", .from_port = "true", .to = "hi"},
                EdgeSpec{.from = "sw", .from_port = "false", .to = "lo"},
                EdgeSpec{.from = "hi", .to = "out"},
                EdgeSpec{.from = "lo", .to = "out"},
            };
            auto plan = aero::runtime::compile_flow(app, node_reg);
            if (!plan) {
                std::printf("  switch(raw=%.0f) compile REJECTED(!) %s\n", raw, plan.error().c_str());
                ++failures;
                return;
            }
            aero::ProcessingContext ctx;
            aero::Frame frame{static_cast<std::int64_t>(raw)};
            ctx.reset(&frame);
            plan->flow.execute(ctx);
            const double got = ctx.output.empty() ? -1.0 : ctx.output[0];
            const bool ok = got == expected_out;
            std::printf("  switch(raw=%-5.0f)          : out=%.1f (expected %.1f) %s\n", raw, got,
                        expected_out, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        run_switch(150.0, 1500.0);  // > 100 -> true branch (x10) -> only "hi" ran
        run_switch(50.0, 50.0);     // <= 100 -> false branch (x1) -> only "lo" ran
    }

    // 020 §7.1: aero.transform.set — first write for a fresh tag name is a push_back.
    {
        Application app;
        app.name = "set-fresh";
        app.version = "0.1.0";
        app.flow = {
            NodeSpec{.id = "src", .type_id = "aero.source.decode"},
            NodeSpec{.id = "set", .type_id = "aero.transform.set",
                     .config = {{"tag", "doubled"}, {"expr", "raw * 2"}}},
            NodeSpec{.id = "out", .type_id = "aero.output.sum"},
        };
        auto plan = aero::runtime::compile_flow(app, node_reg);
        bool ok = plan.has_value();
        if (ok) {
            aero::ProcessingContext ctx;
            aero::Frame frame{5};
            ctx.reset(&frame);
            plan->flow.execute(ctx);
            double doubled = -1.0;
            std::size_t raw_count = 0;
            for (const auto& t : ctx.tags) {
                if (t.name == "doubled") doubled = t.value;
                if (t.name == "raw") ++raw_count;
            }
            ok = doubled == 10.0 && raw_count == 1;  // fresh write pushed, "raw" untouched
            std::printf("  %-28s : doubled=%.1f raw_count=%zu %s\n", "set-fresh-write", doubled,
                        raw_count, ok ? "ok" : "FAIL");
        } else {
            std::printf("  %-28s : compile REJECTED(!) %s\n", "set-fresh-write", plan.error().c_str());
        }
        if (!ok) ++failures;
    }

    // 020 §7.1: aero.transform.set — overwrite-in-place (not append) of an EXISTING tag, and
    // self-reference reads the OLD value (evaluate() runs before the overwrite). This is the SAME
    // node's own configured `tag` referencing itself in its own `expr` (the "change-by" pattern, §7.3)
    // — a single writer, not the two-different-nodes collision §7.5 above tests, so it's exercised
    // directly against SetNode rather than through compile_flow/validate_tag_writers.
    {
        using aero::nodes::SetNode;
        SetNode node(SetNode::compile("tag(\"counter\") + 1"), "counter");
        aero::ProcessingContext ctx;
        ctx.reset(nullptr);

        node.process(ctx);  // "counter" doesn't exist yet -> tag_value reads 0.0 -> writes 1.0 (push_back)
        double after_first = -1.0;
        std::size_t count_first = 0;
        for (const auto& t : ctx.tags) if (t.name == "counter") { after_first = t.value; ++count_first; }

        node.process(ctx);  // SAME ctx, not reset -> reads the OLD 1.0 -> writes 2.0 IN PLACE
        double after_second = -1.0;
        std::size_t count_second = 0;
        for (const auto& t : ctx.tags) if (t.name == "counter") { after_second = t.value; ++count_second; }

        const bool ok = after_first == 1.0 && count_first == 1 && after_second == 2.0 && count_second == 1;
        std::printf("  %-28s : after1=%.1f(n=%zu) after2=%.1f(n=%zu) %s\n", "set-overwrite-self-ref",
                    after_first, count_first, after_second, count_second, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }

    // 020 §8: a bounded loop — src -> loop_start(i=0) -> accumulate(sum += i) -> loop_back(i+=1,
    // continue while i<=9) -(loop_back)-> accumulate; -(else)-> out. Hand-worked expectation: the body
    // runs once per i in [0..9] inclusive (10 passes) before loop_back's post-increment check (i=10)
    // finally fails 10<=9, so accumulate sees i=0,1,...,9 -> sum=0+1+...+9=45, and the final counter
    // (loop_back's own last write) is 10, one past the inclusive bound.
    {
        Application app;
        app.name = "loop-accumulate";
        app.version = "0.1.0";
        app.flow = {
            NodeSpec{.id = "src", .type_id = "aero.source.decode"},
            NodeSpec{.id = "ls", .type_id = "aero.flow.loop_start",
                     .config = {{"counter_tag", "i"}, {"start_expr", "0"},
                                {"max_iterations", 20}, {"max_duration_ms", 5000}}},
            NodeSpec{.id = "acc", .type_id = "aero.transform.set",
                     .config = {{"tag", "sum"}, {"expr", "tag(\"sum\") + tag(\"i\")"}}},
            NodeSpec{.id = "lb", .type_id = "aero.flow.loop_back",
                     .config = {{"counter_tag", "i"}, {"step_expr", "1"}, {"end_expr", "9"}}},
            NodeSpec{.id = "out", .type_id = "aero.output.sum"},
        };
        app.edges = {
            EdgeSpec{.from = "src", .to = "ls"},
            EdgeSpec{.from = "ls", .to = "acc"},
            EdgeSpec{.from = "acc", .to = "lb"},
            EdgeSpec{.from = "lb", .from_port = "loop_back", .to = "acc"},
            EdgeSpec{.from = "lb", .to = "out"},
        };
        auto plan = aero::runtime::compile_flow(app, node_reg);
        bool ok = plan.has_value();
        if (ok) {
            aero::ProcessingContext ctx;
            aero::Frame frame{1};
            ctx.reset(&frame);
            plan->flow.execute(ctx);
            double sum = -1.0, i = -1.0;
            for (const auto& t : ctx.tags) {
                if (t.name == "sum") sum = t.value;
                if (t.name == "i") i = t.value;
            }
            ok = !ctx.failed && sum == 45.0 && i == 10.0;
            std::printf("  %-28s : sum=%.1f(expect 45) i=%.1f(expect 10) failed=%d %s\n",
                        "loop-accumulate-body-runs-N", sum, i, ctx.failed, ok ? "ok" : "FAIL");
        } else {
            std::printf("  %-28s : compile REJECTED(!) %s\n", "loop-accumulate-body-runs-N",
                        plan.error().c_str());
        }
        if (!ok) ++failures;
    }

    // 020 §8.4: the runaway-safety path — max_iterations set below the real trip count must fail the
    // Command (ctx.failed), reusing execute()'s existing Error handling, never hang or silently truncate.
    {
        Application app;
        app.name = "loop-runaway";
        app.version = "0.1.0";
        app.flow = {
            NodeSpec{.id = "src", .type_id = "aero.source.decode"},
            NodeSpec{.id = "ls", .type_id = "aero.flow.loop_start",
                     .config = {{"counter_tag", "i"}, {"start_expr", "0"},
                                {"max_iterations", 3}, {"max_duration_ms", 5000}}},
            NodeSpec{.id = "lb", .type_id = "aero.flow.loop_back",
                     .config = {{"counter_tag", "i"}, {"step_expr", "1"}, {"end_expr", "1000000"}}},
            NodeSpec{.id = "out", .type_id = "aero.output.sum"},
        };
        app.edges = {
            EdgeSpec{.from = "src", .to = "ls"},
            EdgeSpec{.from = "ls", .to = "lb"},
            EdgeSpec{.from = "lb", .from_port = "loop_back", .to = "lb"},
            EdgeSpec{.from = "lb", .to = "out"},
        };
        auto plan = aero::runtime::compile_flow(app, node_reg);
        bool ok = plan.has_value();
        if (ok) {
            aero::ProcessingContext ctx;
            aero::Frame frame{1};
            ctx.reset(&frame);
            plan->flow.execute(ctx);
            ok = ctx.failed;
            std::printf("  %-28s : failed=%d(expect 1) %s\n", "loop-runaway-hits-max-iterations",
                        ctx.failed, ok ? "ok" : "FAIL");
        } else {
            std::printf("  %-28s : compile REJECTED(!) %s\n", "loop-runaway-hits-max-iterations",
                        plan.error().c_str());
        }
        if (!ok) ++failures;
    }

    std::printf("%s\n", failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}
