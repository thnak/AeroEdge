// AeroEdge runtime — the Flow Compiler: validate-then-build at deploy (spec 009 §3, 004 §2.1).
//
// P1 (009 §9): "a flow is compiled and validated at deploy; an invalid definition never reaches a
// running actor." This is the single validate-then-build seam both deploy() and reload() call BEFORE
// touching the live engine — so a bad Application is rejected as a value (std::expected error), never a
// crash and never a half-deploy. It runs OFF any hot path (once per deploy/reload, I3).
//
// What it checks today (004 §2.1 step 1, the achievable subset for the Phase-4/5 LINEAR pipeline):
//   * node resolution — every flow node's type_id resolves in the registry (005 §5);
//   * non-empty flow — a flow with no steps has nothing to run;
//   * category shape — the pipeline has at least one Source and one Output node (NodeDescriptor
//     ::category), i.e. data enters and egress is staged; a decode→…→output canonical shape (004 §1);
//   * per-node config — the built-in nodes' required config is present + well-typed (scale needs a
//     numeric 'factor'; moving_average needs a 'window' >= 1), so a misconfigured node is caught at
//     deploy, not by producing garbage at runtime.
//
// Deferred (noted honestly, R5): full DAG acyclicity + slot-type matching + topological ordering
// (004 §2.1 steps 1–2) are future work — Phase-4/5 flows are LINEAR (schema is an ordered array), so
// there is no cycle to detect and adjacency is positional. The config checks are keyed to the known
// built-in type_ids; a generic per-node config schema (015) that lets ANY extension declare its
// required fields is the general form and lands with the extension model (Phase 6, 008 §5).
#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aero/core/compiled_flow.hpp"
#include "aero/core/registry.hpp"
#include "aero/nodes/expr_rule_node.hpp"
#include "aero/schema/application.hpp"
#include "aero/sdk/node.hpp"
#include "nlohmann/json.hpp"

namespace aero::runtime {

// The output of a successful compile: the owned node instances + the immutable CompiledFlow whose
// steps point into them (004 §2.1 result). Node lifetimes and the flow are kept together so the
// Runtime can own a plan as one unit and swap it wholesale on hot-reload (009 §4). Heap-held by the
// Runtime so the flow's address is stable while the actor holds a `const CompiledFlow*` (I3/ADR-008).
struct CompiledPlan {
    std::vector<std::unique_ptr<aero::INode>> nodes;  // owns the flow's node instances
    aero::CompiledFlow flow;                          // steps hold INode* into `nodes` (stable)

    // Re-point the flow at the owned nodes. Called once after all nodes are constructed; the flow
    // stores raw INode* which stay valid as long as `nodes` (and therefore this plan) lives.
    //
    // `order` (019 §2): indices into `nodes`, in the sequence steps should run — empty means natural
    // construction order (today's linear behaviour, unchanged, 019 G6). `labels[i]` is the
    // required_label for `nodes[i]` (see CompiledFlow::add) — must reference STABLE storage (a static
    // literal, never a view into the Application that produced this plan, which does not outlive it).
    void wire(const std::vector<std::size_t>& order = {},
              const std::vector<std::string_view>& labels = {}) {
        flow = aero::CompiledFlow{};
        if (order.empty()) {
            for (auto& n : nodes) {
                flow.add(*n);
            }
            return;
        }
        for (std::size_t idx : order) {
            const std::string_view label = idx < labels.size() ? labels[idx] : std::string_view{};
            flow.add(*nodes[idx], label);
        }
    }
};

// Per-node config validation for the built-in nodes (009 §3 "invalid node config"). Keyed by type_id
// because the built-ins' factories default missing fields silently (registry.hpp) — the compiler is
// where a REQUIRED field's absence becomes a deploy-time error instead of a silent wrong result.
inline std::expected<void, std::string> validate_node_config(const std::string& type_id,
                                                             const nlohmann::json& cfg) {
    if (type_id == "aero.transform.scale") {
        if (!cfg.contains("factor")) {
            return std::unexpected("node 'aero.transform.scale' requires a numeric 'factor'");
        }
        if (!cfg["factor"].is_number()) {
            return std::unexpected("node 'aero.transform.scale' config 'factor' must be a number");
        }
    } else if (type_id == "aero.transform.moving_average") {
        if (!cfg.contains("window")) {
            return std::unexpected("node 'aero.transform.moving_average' requires a 'window'");
        }
        if (!cfg["window"].is_number_unsigned() && !cfg["window"].is_number_integer()) {
            return std::unexpected("node 'aero.transform.moving_average' config 'window' must be an integer");
        }
        if (cfg["window"].get<long long>() < 1) {
            return std::unexpected("node 'aero.transform.moving_average' 'window' must be >= 1");
        }
    } else if (type_id == "aero.rule.expr") {
        // Low-code Rule DSL (008 §6): the expression must be present, a string, and PARSE — a
        // malformed rule is caught at deploy (parse-once), never producing a broken node at runtime.
        if (!cfg.contains("expr") || !cfg["expr"].is_string()) {
            return std::unexpected("node 'aero.rule.expr' requires a string 'expr'");
        }
        auto prog = aero::nodes::ExprRuleNode::compile(cfg["expr"].get<std::string>());
        if (!prog.ok) {
            return std::unexpected("node 'aero.rule.expr' invalid expression: " + prog.error);
        }
    } else if (type_id == "aero.flow.switch") {
        // Same DSL, same parse-once-at-deploy posture as aero.rule.expr (019 §5).
        if (!cfg.contains("expr") || !cfg["expr"].is_string()) {
            return std::unexpected("node 'aero.flow.switch' requires a string 'expr'");
        }
        auto prog = aero::nodes::ExprRuleNode::compile(cfg["expr"].get<std::string>());
        if (!prog.ok) {
            return std::unexpected("node 'aero.flow.switch' invalid expression: " + prog.error);
        }
    } else if (type_id == "aero.transform.set") {
        // 020 §7.2: same parse-once-at-deploy posture as aero.rule.expr/aero.flow.switch — PLUS a
        // non-empty 'tag' (the write target), which the read-only DSL nodes don't need.
        if (!cfg.contains("tag") || !cfg["tag"].is_string() || cfg["tag"].get<std::string>().empty()) {
            return std::unexpected("node 'aero.transform.set' requires a non-empty string 'tag'");
        }
        if (!cfg.contains("expr") || !cfg["expr"].is_string()) {
            return std::unexpected("node 'aero.transform.set' requires a string 'expr'");
        }
        auto prog = aero::nodes::ExprRuleNode::compile(cfg["expr"].get<std::string>());
        if (!prog.ok) {
            return std::unexpected("node 'aero.transform.set' invalid expression: " + prog.error);
        }
    }
    return {};
}

// 020 §7.5: the statically-known tag name a node writes into ctx.tags, if any. Only nodes with a FIXED
// (deploy-time-known) name are covered — Source nodes decoding a byte payload into data-dependent names
// (aero.source.json/.modbus/.modbus_bits: one tag per JSON key / register index) have no name knowable
// before a real frame arrives, so they're honestly excluded here, not falsely cleared as collision-free.
// aero.transform.scale/.mean/.minmax/.sum don't introduce a NEW tag name at all (Scale mutates existing
// tags in place; the others write ctx.output, not ctx.tags), so they're absent from this list too.
inline std::optional<std::string> static_tag_write(const std::string& type_id, const nlohmann::json& cfg) {
    if (type_id == "aero.source.decode") return std::string("raw");
    if (type_id == "aero.transform.crc") return std::string("crc16");
    if (type_id == "aero.source.mes_order") return std::string("order.qty");
    if (type_id == "aero.transform.set" && cfg.contains("tag") && cfg["tag"].is_string()) {
        return cfg["tag"].get<std::string>();
    }
    return std::nullopt;
}

// A new, dedicated compiler pass (020 §7.5) — own function, own single responsibility, not folded into
// validate_node_config's per-node shape checks or order_flow_graph's topology logic. Rejects two
// DIFFERENT nodes writing the same tag name unless they're mutually exclusive: v1 supports at most one
// branch-producing node per flow (order_flow_graph's have_branch_source check above), so any two
// non-empty, DIFFERENT labels are always the "true"/"false" pair of that one switch — no separate
// branch-source tracking needed here, it's already enforced upstream. `labels[i]` is empty for every
// node in linear (edges-empty) mode, so any two linear-mode writers of the same tag always collide.
inline std::expected<void, std::string> validate_tag_writers(
        const schema::Application& app, const std::vector<std::string_view>& labels) {
    struct Writer { std::size_t index; std::string_view label; };
    std::unordered_map<std::string, std::vector<Writer>> writers_by_tag;

    for (std::size_t i = 0; i < app.flow.size(); ++i) {
        auto tag = static_tag_write(app.flow[i].type_id, app.flow[i].config);
        if (!tag) continue;
        const std::string_view label = i < labels.size() ? labels[i] : std::string_view{};
        writers_by_tag[*tag].push_back(Writer{i, label});
    }

    for (const auto& [tag, ws] : writers_by_tag) {
        for (std::size_t a = 0; a < ws.size(); ++a) {
            for (std::size_t b = a + 1; b < ws.size(); ++b) {
                const bool mutually_exclusive =
                    !ws[a].label.empty() && !ws[b].label.empty() && ws[a].label != ws[b].label;
                if (!mutually_exclusive) {
                    return std::unexpected("nodes '" + app.flow[ws[a].index].id + "' and '" +
                        app.flow[ws[b].index].id + "' both write tag '" + tag + "' and are not "
                        "mutually exclusive — this makes the tag's value order-dependent");
                }
            }
        }
    }
    return {};
}

// The result of ordering a real graph (019 §2): a topological order over app.flow's indices, plus each
// node's required_label (see CompiledFlow::add) — empty for an unconditional node, or the STATIC
// "true"/"false" literal for one reached only via a labeled edge from aero.flow.switch.
struct GraphOrder {
    std::vector<std::size_t> order;
    std::vector<std::string_view> labels;
};

// Validate `app.edges` against `app.flow` and produce a schedulable order (019 §2). Only called when
// `app.edges` is non-empty — an empty-edges Application takes the original linear path unchanged (G6).
// Kahn's algorithm does topo-sort AND cycle detection in one pass: repeatedly dequeue a zero-in-degree
// node, decrement its successors'; anything left over after the queue drains is a cycle.
inline std::expected<GraphOrder, std::string> order_flow_graph(
        const schema::Application& app, const std::vector<std::unique_ptr<INode>>& nodes) {
    // v1 scope (019 §5): only aero.flow.switch produces labeled edges, and only ever "true"/"false" —
    // so labels reference these two static literals, never a view into `app` (which does not outlive
    // the CompiledPlan this order feeds). Any other from_port is rejected below, not silently accepted.
    static constexpr std::string_view kTrue = "true";
    static constexpr std::string_view kFalse = "false";

    const std::size_t n = app.flow.size();

    std::unordered_map<std::string, std::size_t> id_to_index;
    id_to_index.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!id_to_index.emplace(app.flow[i].id, i).second) {
            return std::unexpected("duplicate node id: '" + app.flow[i].id + "'");
        }
    }

    std::vector<std::vector<std::pair<std::size_t, std::string_view>>> adj(n);  // from-idx -> {to-idx, label}
    std::vector<int> in_degree(n, 0);
    // v1 supports exactly one active branch decision per Command (ctx.active_branch is a single field,
    // not a stack — 019 §10 "multiple independent switch points"). Track which node index is the
    // source of a labeled edge; a SECOND distinct source would let one switch's decision silently
    // overwrite another's before its own labeled steps are checked (P1: reject at deploy, never let a
    // node route on the wrong switch's answer).
    bool have_branch_source = false;
    std::size_t branch_source = 0;
    for (const auto& e : app.edges) {
        const auto from_it = id_to_index.find(e.from);
        if (from_it == id_to_index.end()) {
            return std::unexpected("edge references unknown node id: '" + e.from + "'");
        }
        const auto to_it = id_to_index.find(e.to);
        if (to_it == id_to_index.end()) {
            return std::unexpected("edge references unknown node id: '" + e.to + "'");
        }
        std::string_view label;
        if (!e.from_port.empty()) {
            if (e.from_port == "true") label = kTrue;
            else if (e.from_port == "false") label = kFalse;
            else {
                return std::unexpected("edge.from_port '" + e.from_port + "' is not supported yet "
                                       "(only \"true\"/\"false\", from aero.flow.switch)");
            }
            if (!have_branch_source) {
                have_branch_source = true;
                branch_source = from_it->second;
            } else if (branch_source != from_it->second) {
                return std::unexpected("flow has more than one branch-producing node ('" +
                                       app.flow[branch_source].id + "' and '" + e.from + "') — only one "
                                       "active switch point per flow is supported yet (019 sec3/sec10)");
            }
        }
        adj[from_it->second].push_back({to_it->second, label});
        ++in_degree[to_it->second];
    }

    std::vector<std::size_t> queue;
    for (std::size_t i = 0; i < n; ++i) {
        if (in_degree[i] == 0) queue.push_back(i);
    }
    if (queue.size() != 1) {
        return std::unexpected("flow must have exactly one root (a node with no incoming edge); found " +
                               std::to_string(queue.size()));
    }
    if (nodes[queue[0]]->descriptor().category != NodeCategory::Source) {
        return std::unexpected("flow's root node '" + app.flow[queue[0]].id + "' must be a Source node");
    }

    std::vector<std::vector<std::string_view>> incoming_labels(n);
    std::vector<std::size_t> order;
    order.reserve(n);
    std::vector<int> remaining = in_degree;
    for (std::size_t qi = 0; qi < queue.size(); ++qi) {
        const std::size_t u = queue[qi];
        order.push_back(u);
        for (const auto& [v, label] : adj[u]) {
            incoming_labels[v].push_back(label);
            if (--remaining[v] == 0) {
                queue.push_back(v);
            }
        }
    }
    if (order.size() != n) {
        return std::unexpected("flow contains a cycle");
    }

    std::vector<std::string_view> labels(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (incoming_labels[i].empty()) continue;  // the root — no incoming edge, always unconditional
        const std::string_view first = incoming_labels[i].front();
        for (const std::string_view l : incoming_labels[i]) {
            if (l != first) {
                return std::unexpected("node '" + app.flow[i].id + "' is reached by edges with "
                                       "conflicting branch labels — not supported yet");
            }
        }
        labels[i] = first;
    }

    return GraphOrder{std::move(order), std::move(labels)};
}

// Validate + compile an Application's flow into a CompiledPlan (009 §3). Never throws, never
// half-builds: any failure returns an error value with the old deployment untouched (P1).
inline std::expected<CompiledPlan, std::string> compile_flow(const schema::Application& app,
                                                             const NodeRegistry& registry) {
    if (app.flow.empty()) {
        return std::unexpected("flow is empty: an Application must declare at least one node");
    }

    CompiledPlan plan;
    plan.nodes.reserve(app.flow.size());

    bool has_source = false;
    bool has_output = false;
    for (const auto& ns : app.flow) {
        if (!registry.contains(ns.type_id)) {
            return std::unexpected("unknown node type_id: '" + ns.type_id + "'");
        }
        if (auto cfg_ok = validate_node_config(ns.type_id, ns.config); !cfg_ok) {
            return std::unexpected(cfg_ok.error());
        }
        auto node = registry.create(ns.type_id, ns.config);
        if (!node) {
            return std::unexpected("flow node: " + node.error());
        }
        if (!*node) {  // defensive: an extension factory (native, 008 §2) may return null on failure
            return std::unexpected("flow node '" + ns.type_id + "' failed to construct");
        }
        switch ((*node)->descriptor().category) {
            case aero::NodeCategory::Source: has_source = true; break;
            case aero::NodeCategory::Output: has_output = true; break;
            default: break;
        }
        plan.nodes.push_back(std::move(*node));
    }

    // Category shape (004 §1 canonical flow): data must enter (a Source) and egress must be staged (an
    // Output). A flow that is all transforms would compute values nothing consumes / nothing feeds.
    if (!has_source) {
        return std::unexpected("flow has no Source node: nothing introduces data into the pipeline");
    }
    if (!has_output) {
        return std::unexpected("flow has no Output node: the pipeline stages no egress");
    }

    if (app.edges.empty()) {
        // Every node is unconditional in linear mode (no switch exists yet) — an empty-labels vector
        // makes validate_tag_writers treat any two same-tag writers as an unconditional collision.
        if (auto tw = validate_tag_writers(app, std::vector<std::string_view>(app.flow.size())); !tw) {
            return std::unexpected(tw.error());
        }
        plan.wire();  // linear, array order IS the DAG — unchanged from today (019 G6)
        return plan;
    }

    auto order = order_flow_graph(app, plan.nodes);
    if (!order) {
        return std::unexpected(order.error());
    }
    if (auto tw = validate_tag_writers(app, order->labels); !tw) {
        return std::unexpected(tw.error());
    }
    plan.wire(order->order, order->labels);
    return plan;
}

}  // namespace aero::runtime
