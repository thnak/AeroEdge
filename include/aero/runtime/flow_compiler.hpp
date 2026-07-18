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
#include <string>
#include <vector>

#include "aero/core/compiled_flow.hpp"
#include "aero/core/registry.hpp"
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
    void wire() {
        flow = aero::CompiledFlow{};
        for (auto& n : nodes) {
            flow.add(*n);
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
    }
    return {};
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

    plan.wire();
    return plan;
}

}  // namespace aero::runtime
