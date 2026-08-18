// AeroEdge built-in node — aero.transform.set (020 §7): assign the result of a DSL expression into a
// named working-set tag. A near-copy of switch_node.hpp's own shape: reuses ExprRuleNode::Program
// wholesale (the same public Program::evaluate() aero.rule.expr/aero.flow.switch already call) — the
// third reuse of the same evaluator, not a new interpreter.
#pragma once

#include <array>
#include <string>
#include <string_view>

#include "aero/nodes/expr_rule_node.hpp"
#include "aero/sdk/node.hpp"

namespace aero::nodes {

class SetNode final : public INode {
public:
    using Program = ExprRuleNode::Program;

    [[nodiscard]] static Program compile(std::string_view expr) { return ExprRuleNode::compile(expr); }

    SetNode(Program prog, std::string tag) noexcept : prog_(std::move(prog)), tag_(std::move(tag)) {}

    NodeResult process(ProcessingContext& ctx) noexcept override {
        if (!prog_.ok) return NodeResult::Error;  // defensive: deploy validation rejects bad exprs
        // Evaluate BEFORE the search-and-overwrite below — self-reference (`change-by`: tag("x") + 1)
        // reads the OLD value this way, for free, from evaluation order alone (020 §7.1).
        const double v = prog_.evaluate(ctx);
        for (auto& t : ctx.tags) {
            if (t.name == tag_) { t.value = v; return NodeResult::Continue; }
        }
        // First write for this name. O(tags.size()) linear scan above, same tradeoff tag_value() itself
        // already makes — 0-alloc either way: ctx.tags is pre-reserve()'d and never reallocates here
        // (N1). tag_ is a std::string member, stable for the flow's lifetime (I6), so the pushed Tag's
        // string_view stays valid.
        ctx.tags.push_back(Tag{tag_, v});
        return NodeResult::Continue;
    }

    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    [[nodiscard]] bool valid() const noexcept { return prog_.ok; }
    [[nodiscard]] const std::string& error() const noexcept { return prog_.error; }

    static constexpr std::array<FieldSpec, 2> kFields{{
        {.key = "tag", .label = "Tag", .type = FieldType::String, .required = true,
         .help = "Working-set tag to write. Overwrites in place if it already exists."},
        {.key = "expr", .label = "Expression", .type = FieldType::String, .required = true,
         .help = "Same DSL as aero.rule.expr. Evaluated BEFORE the write, so self-reference "
                 "(tag(\"x\") + 1) reads the OLD value."},
    }};
    static constexpr NodeDescriptor kDesc{NodeCategory::Transform, "aero.transform.set", kFields};

private:
    Program prog_;
    std::string tag_;  // stable backing for the written Tag::name (a view) — the node is pinned in the
                        // CompiledPlan for the whole flow's lifetime (I6), same as StagedHttpRequest's
                        // url/method pattern.
};

}  // namespace aero::nodes
