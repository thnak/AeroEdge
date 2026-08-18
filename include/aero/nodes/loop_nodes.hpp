// AeroEdge built-in nodes — aero.flow.loop_start / aero.flow.loop_back (020 §8): a bounded,
// runtime-computed loop ("count X from start to end step by step") — the one construct in the doc's
// block taxonomy that needs genuine new CompiledFlow::execute() capability (a jump-back), not just a
// new node (see compiled_flow.hpp's loop_back_target/ctx.loop_continue). Bundled in one file (mirroring
// mes_nodes.hpp's pairing, not set_node.hpp/switch_node.hpp's one-node-per-file) because the two are
// genuinely co-dependent — a loop_back with no matching loop_start is meaningless.
//
// Both reuse ExprRuleNode::Program wholesale — the FOURTH reuse of the same evaluator (after
// aero.rule.expr, aero.flow.switch, aero.transform.set) — and both write their counter tag via the
// exact overwrite-in-place logic aero.transform.set already established (020 §7.1), factored once here
// since both new nodes need it.
#pragma once

#include <array>
#include <chrono>
#include <string>
#include <string_view>

#include "aero/nodes/expr_rule_node.hpp"
#include "aero/sdk/node.hpp"

namespace aero::nodes {

namespace loop_detail {

// Search-or-append write into ctx.tags — identical to SetNode::process()'s own logic (020 §7.1). Both
// loop_start (initial counter) and loop_back (each incremented counter) need it.
inline void overwrite_or_append(ProcessingContext& ctx, std::string_view name, double v) {
    for (auto& t : ctx.tags) {
        if (t.name == name) {
            t.value = v;
            return;
        }
    }
    ctx.tags.push_back(Tag{name, v});
}

}  // namespace loop_detail

class LoopStartNode final : public INode {
public:
    using Program = ExprRuleNode::Program;

    [[nodiscard]] static Program compile(std::string_view expr) { return ExprRuleNode::compile(expr); }

    LoopStartNode(Program start_prog, std::string counter_tag, std::size_t max_iterations,
                  std::chrono::milliseconds max_duration) noexcept
        : start_prog_(std::move(start_prog)),
          counter_tag_(std::move(counter_tag)),
          max_iterations_(max_iterations),
          max_duration_(max_duration) {}

    NodeResult process(ProcessingContext& ctx) noexcept override {
        if (!start_prog_.ok) return NodeResult::Error;  // defensive: deploy validation rejects bad exprs
        const double start_v = start_prog_.evaluate(ctx);
        loop_detail::overwrite_or_append(ctx, counter_tag_, start_v);
        // Fresh-per-Command budget (020 §8.2/§8.4) — loop_start is the one place that always runs
        // exactly once before the loop's first iteration, unlike loop_back which runs once PER
        // iteration and has no single moment to reset a budget to.
        ctx.loop_iterations_remaining = max_iterations_;
        ctx.loop_deadline = std::chrono::steady_clock::now() + max_duration_;
        return NodeResult::Continue;
    }

    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    [[nodiscard]] bool valid() const noexcept { return start_prog_.ok; }
    [[nodiscard]] const std::string& error() const noexcept { return start_prog_.error; }

    static constexpr std::array<FieldSpec, 4> kFields{{
        {.key = "counter_tag", .label = "Counter tag", .type = FieldType::String, .required = true,
         .help = "Working-set tag the loop counter is written to."},
        {.key = "start_expr", .label = "Start value", .type = FieldType::String, .required = true,
         .help = "Same DSL as aero.rule.expr. Evaluated once to initialize the counter."},
        {.key = "max_iterations", .label = "Max iterations", .type = FieldType::Int, .required = true,
         .has_min = true, .min = 1,
         .help = "Hard safety cap on pass count — required, no default, so every loop's author picks "
                 "one deliberately."},
        {.key = "max_duration_ms", .label = "Max duration (ms)", .type = FieldType::Int, .required = true,
         .has_min = true, .min = 1,
         .help = "A second, independent safety cap on wall-clock time — iteration count alone doesn't "
                 "bound an expensive body."},
    }};
    static constexpr NodeDescriptor kDesc{NodeCategory::Rule, "aero.flow.loop_start", kFields};

private:
    Program start_prog_;
    std::string counter_tag_;
    std::size_t max_iterations_;
    std::chrono::milliseconds max_duration_;
};

class LoopBackNode final : public INode {
public:
    using Program = ExprRuleNode::Program;

    [[nodiscard]] static Program compile(std::string_view expr) { return ExprRuleNode::compile(expr); }

    LoopBackNode(std::string counter_tag, Program step_prog, Program end_prog) noexcept
        : counter_tag_(std::move(counter_tag)),
          step_prog_(std::move(step_prog)),
          end_prog_(std::move(end_prog)) {}

    NodeResult process(ProcessingContext& ctx) noexcept override {
        if (!step_prog_.ok || !end_prog_.ok) return NodeResult::Error;

        // Evaluate step / read the OLD counter BEFORE overwriting — same evaluation-order trick
        // aero.transform.set's self-reference relies on (020 §7.1): reads the pre-update value for free
        // from evaluation order alone, no special-casing.
        const double step_v = step_prog_.evaluate(ctx);
        double old_counter = 0.0;
        for (const auto& t : ctx.tags) {
            if (t.name == counter_tag_) {
                old_counter = t.value;
                break;
            }
        }
        const double new_counter = old_counter + step_v;
        loop_detail::overwrite_or_append(ctx, counter_tag_, new_counter);

        // Ascending/descending decided by the step's sign; INCLUSIVE bound (Scratch/Blockly's own
        // "count from 1 to 10" convention — the doc's cited precedent for this construct).
        const double end_v = end_prog_.evaluate(ctx);
        const bool wants_continue = step_v >= 0.0 ? (new_counter <= end_v) : (new_counter >= end_v);

        // Every pass consumes one unit of budget regardless of outcome — saturating, never wraps (this
        // closes 020 §8.4's real max_iterations=0 underflow bug at the source, on top of the deploy-time
        // min=1 rejection: even a config bug that somehow bypassed validation can't hang here).
        if (ctx.loop_iterations_remaining > 0) --ctx.loop_iterations_remaining;
        const bool time_up = ctx.loop_deadline.has_value() &&
                              std::chrono::steady_clock::now() >= *ctx.loop_deadline;

        // Exhaustion is only a FAILURE if the loop still wants another pass and can't have one — a loop
        // that finishes naturally on exactly its last allowed pass (a tightly-bounded max_iterations
        // matching the real trip count is a normal, legitimate config) is a coincidence, not a bug.
        if (wants_continue && (ctx.loop_iterations_remaining == 0 || time_up)) {
            return NodeResult::Error;
        }
        ctx.loop_continue = wants_continue;
        return NodeResult::Continue;
    }

    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    [[nodiscard]] bool valid() const noexcept { return step_prog_.ok && end_prog_.ok; }

    static constexpr std::array<FieldSpec, 3> kFields{{
        {.key = "counter_tag", .label = "Counter tag", .type = FieldType::String, .required = true,
         .help = "Must match the paired loop_start's counter_tag."},
        {.key = "step_expr", .label = "Step", .type = FieldType::String, .default_string = "1",
         .help = "Same DSL as aero.rule.expr. Added to the counter each pass; sign decides ascending "
                 "vs. descending comparison against End."},
        {.key = "end_expr", .label = "End value (inclusive)", .type = FieldType::String, .required = true,
         .help = "Same DSL as aero.rule.expr. The loop continues while the counter hasn't passed this."},
    }};
    static constexpr NodeDescriptor kDesc{NodeCategory::Rule, "aero.flow.loop_back", kFields};

private:
    std::string counter_tag_;
    Program step_prog_;
    Program end_prog_;
};

}  // namespace aero::nodes
