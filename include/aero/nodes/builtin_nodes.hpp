// AeroEdge built-in nodes (spec 005 §2). The minimal Phase-1 set — one per category on the path
// Source → Transform → Output — proving the flow executor end-to-end. Each obeys N1–N6: noexcept,
// non-blocking, no alloc beyond the (reused) context buffers, sees only ProcessingContext.
#pragma once

#include "aero/sdk/node.hpp"

namespace aero::nodes {

// Source: decode the frame into a working-set tag. Real decoders parse byte spans (006); Phase-1
// lifts the scalar into a tag named "raw".
class DecodeSourceNode final : public INode {
public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        if (ctx.frame == nullptr) {
            return NodeResult::Error;
        }
        ctx.tags.push_back(Tag{"raw", static_cast<double>(ctx.frame->raw)});
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr NodeDescriptor kDesc{NodeCategory::Source, "aero.source.decode"};
};

// Transform: scale every tag by a configured factor (config read once — Phase-1 via ctor; the
// configure()/NodeConfig path lands with the registry in Phase 5).
class ScaleNode final : public INode {
public:
    explicit ScaleNode(double factor) noexcept : factor_(factor) {}

    NodeResult process(ProcessingContext& ctx) noexcept override {
        for (auto& tag : ctx.tags) {
            tag.value *= factor_;
        }
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr NodeDescriptor kDesc{NodeCategory::Transform, "aero.transform.scale"};

private:
    double factor_;
};

// Output: sum the tags, stage the result for egress, and emit a TagChanged event (published
// post-commit by the actor). A real Output stages to an EgressActor via tell (004 §4); Phase-1
// stages into the context.
class SumOutputNode final : public INode {
public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        double sum = 0.0;
        for (const auto& tag : ctx.tags) {
            sum += tag.value;
        }
        ctx.output.push_back(sum);
        ctx.events.push_back(Event{"TagChanged", sum});
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr NodeDescriptor kDesc{NodeCategory::Output, "aero.output.sum"};
};

}  // namespace aero::nodes
