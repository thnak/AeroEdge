// AeroEdge core — the CompiledFlow executor (spec 004).
//
// A Flow is compiled ONCE (nodes resolved + ordered) into an immutable plan; execution walks that
// plan per Command, threading one ProcessingContext (I3). Phase-1 supports a linear pipeline
// (Source → Transform → … → Output) with Stop/Error short-circuit; branch/fan-out (004 §2.2) and
// the deploy-time Flow Compiler validation (004 §2.1) land in Phase 5. Execution is a straight array
// walk: no graph resolution, no allocation, no locking — one virtual INode::process per step (I7).
#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "aero/sdk/node.hpp"
#include "aero/sdk/processing_context.hpp"

namespace aero {

class CompiledFlow {
public:
    // --- compile-time wiring (deploy, once) ---
    // `required_label` (019 §2/§3): empty runs unconditionally; non-empty (currently "true"/"false",
    // set by aero.flow.switch) runs only when it matches ctx.active_branch at execute time — the whole
    // branch mechanism, no graph walk or extra state beyond this one string_view per step.
    //
    // `loop_back_target` (020 §8.3): set ONLY on the one step that IS an aero.flow.loop_back node — the
    // step index execute() jumps to when that node asks for another pass. Never baked into the node
    // itself: a jump target doesn't exist until topological order is known, which is strictly after
    // node construction (see flow_compiler.hpp's CompiledPlan::wire(), the same timing required_label
    // is already resolved at).
    CompiledFlow& add(INode& node, std::string_view required_label = {},
                       std::optional<std::size_t> loop_back_target = std::nullopt) {
        steps_.push_back(Step{&node, required_label, loop_back_target});
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return steps_.size(); }

    // --- execute (per Command, hot) ---
    void execute(ProcessingContext& ctx) const noexcept {
        std::size_t i = 0;
        while (i < steps_.size()) {
            const Step& step = steps_[i];
            // Branch skip (019 §3): an O(1) string_view compare, 0-alloc — does not resolve any graph,
            // preserves I3. A skipped step is simply not called; it does not count as Stop/Error.
            if (!step.required_label.empty() && step.required_label != ctx.active_branch) {
                ++i;
                continue;
            }
            const NodeResult r = step.node->process(ctx);  // one indirect call per node (I7)
            if (r == NodeResult::Stop) break;              // Rule short-circuit — not a failure
            if (r == NodeResult::Error) {
                ctx.failed = true;
                ctx.failed_step = i;
                break;
            }
            // Loop redirect (020 §8.1/§8.3): this step asked to jump back. A stack scalar read/compare/
            // assign, 0-alloc (N1) — the whole runtime cost this feature adds to the hot path.
            if (ctx.loop_continue && step.loop_back_target) {
                i = *step.loop_back_target;
                ctx.loop_continue = false;
                continue;  // skip the ++i below — we already moved
            }
            ++i;
        }
    }

private:
    // Pre-topologically-ordered plan (I3). `Stop`/`Error` still abort the WHOLE remaining array, even
    // under fan-out — refining that to per-branch scoping is deferred (019 §"out of scope"), not built
    // speculatively.
    struct Step {
        INode* node;
        std::string_view required_label;
        std::optional<std::size_t> loop_back_target;
    };
    std::vector<Step> steps_;
};

}  // namespace aero
