// AeroEdge built-in node — aero.flow.switch (019 §5): the one graph-model utility node that needs new
// runtime machinery. Fan-out and merge are free once `edges[]` exists (any node may have >1 outgoing
// edge; two branches writing into the same shared ProcessingContext have already "merged" by the time
// a downstream node runs) — a router that actively CHOOSES a path is the only thing today's INode
// contract (process() -> NodeResult, nothing richer) can't already express, hence this node.
//
// Reuses ExprRuleNode's expression engine (expr_rule_node.hpp) rather than a second parser/evaluator:
// `expr_detail::Program::evaluate()` is the same 0-alloc RPN interpreter aero.rule.expr calls. Unlike
// aero.rule.expr (match -> alarm + Stop), a switch never stops the flow — it only sets
// ctx.active_branch and continues; CompiledFlow::execute() skips any downstream step whose
// required_label doesn't match (compiled from the "true"/"false"-labeled edges leaving this node,
// flow_compiler.hpp's order_flow_graph()).
#pragma once

#include <array>
#include <string>
#include <string_view>

#include "aero/nodes/expr_rule_node.hpp"
#include "aero/sdk/node.hpp"

namespace aero::nodes {

class SwitchNode final : public INode {
public:
    using Program = ExprRuleNode::Program;

    [[nodiscard]] static Program compile(std::string_view expr) { return ExprRuleNode::compile(expr); }

    explicit SwitchNode(Program prog) noexcept : prog_(std::move(prog)) {}

    NodeResult process(ProcessingContext& ctx) noexcept override {
        if (!prog_.ok) return NodeResult::Error;  // defensive: deploy validation rejects bad exprs
        // Both branches are static literals (not node-owned storage) — 0-alloc to set, and valid for
        // the whole flow execution regardless of this node's own lifetime (N1).
        ctx.active_branch = (prog_.evaluate(ctx) != 0.0) ? kTrueLabel : kFalseLabel;
        return NodeResult::Continue;  // a router, never a Stop — unlike aero.rule.expr
    }

    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    [[nodiscard]] bool valid() const noexcept { return prog_.ok; }
    [[nodiscard]] const std::string& error() const noexcept { return prog_.error; }

    static constexpr std::string_view kTrueLabel = "true";
    static constexpr std::string_view kFalseLabel = "false";

    static constexpr std::array<FieldSpec, 1> kFields{{
        {.key = "expr", .label = "Expression", .type = FieldType::String, .required = true,
         .help = "Same DSL as aero.rule.expr. Routes to edges labeled \"true\"/\"false\" — never "
                 "stops the flow (019 §5).",
         .tier2_hint = "expr-tree"},
    }};
    static constexpr NodeDescriptor kDesc{NodeCategory::Rule, "aero.flow.switch", kFields};

private:
    Program prog_;
};

}  // namespace aero::nodes
