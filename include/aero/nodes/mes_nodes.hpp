// AeroEdge MES flow nodes (spec 012 §4). The two node types that let a low-code flow wire MES
// integration WITHOUT touching gateway internals. Both are ordinary INodes (005): they depend only on
// the SDK (StagedMesReport in ProcessingContext) — NOT on aero-mes — so aero-nodes stays below aero-mes
// in the layering (R1, no cycle). The flow actor drains ctx.mes_reports into the MesGateway outbox at
// commit; the gateway maps StagedMesReport → canonical MesReport and does the durable I/O (012 §3).
#pragma once

#include <string>

#include "aero/sdk/node.hpp"

namespace aero::nodes {

// Output (012 §4): stage a MES report for the gateway. `process` ONLY stages — it never does MES I/O
// (I1/N5); the actual at-least-once delivery is the MesGateway's, behind the durable outbox (M2/M3).
// The report value is folded from the working-set tags (a production count / measured value); `line`
// and `label` come from deploy config and are held here so their string_views are stable (0-alloc N1).
class MesReportNode final : public INode {
public:
    MesReportNode(std::string line, std::string label, StagedMesReport::Kind kind)
        : line_(std::move(line)), label_(std::move(label)), kind_(kind) {}

    NodeResult process(ProcessingContext& ctx) noexcept override {
        double value = 0.0;
        for (const auto& t : ctx.tags) value += t.value;  // fold tags → the reported figure
        ctx.mes_reports.push_back(StagedMesReport{kind_, line_, label_, value});  // stage only (I1)
        ctx.output.push_back(value);
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr NodeDescriptor kDesc{NodeCategory::Output, "aero.output.mes"};

private:
    std::string line_;   // stable backing for StagedMesReport::line (a string_view)
    std::string label_;  // stable backing for StagedMesReport::label
    StagedMesReport::Kind kind_;
};

// Source (012 §4): inject a received MES order into the flow as working-set tags. In a live runtime the
// order arrives from the MES via the adapter's command sink (M4) and is `tell`ed into the actor, which
// binds it here; Phase-10 surfaces the deploy-config order (qty/ref) each frame — a deterministic,
// testable Source that pins the node category + type_id (012 §4). The bound order is settable so a
// gateway-delivered MESOrderReceived can update it between frames (single-executor, race-free).
class MesOrderSourceNode final : public INode {
public:
    explicit MesOrderSourceNode(double order_qty) noexcept : order_qty_(order_qty) {}

    // Update the current order (from an inbound MES command, M4). Called on the actor's executor only.
    void set_order_qty(double qty) noexcept { order_qty_ = qty; }

    NodeResult process(ProcessingContext& ctx) noexcept override {
        ctx.tags.push_back(Tag{"order.qty", order_qty_});
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr NodeDescriptor kDesc{NodeCategory::Source, "aero.source.mes_order"};

private:
    double order_qty_ = 0.0;
};

}  // namespace aero::nodes
