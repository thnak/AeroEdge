// AeroEdge built-in nodes (spec 005 §2). The minimal Phase-1 set — one per category on the path
// Source → Transform → Output — proving the flow executor end-to-end. Each obeys N1–N6: noexcept,
// non-blocking, no alloc beyond the (reused) context buffers, sees only ProcessingContext.
#pragma once

#include <array>
#include <cstddef>

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

// Stateful transform (007 §6, resolving 005 §8): a moving average over the last K samples. The ring
// of recent samples is TRANSIENT node-instance state — because a CompiledFlow is owned by its actor
// (004 §2.1) each node instance is per-actor and single-executor-owned, so the ring is race-free and
// needs no lock. It is NOT durable: on deactivation / fenced migration a fresh node instance starts
// COLD and refills over the next K samples (S6). "Nodes compute" — this node holds only state it is
// willing to lose; a durable accumulation (a shift count, an energy total) is NOT held here, it is
// promoted to actor state and persisted by `commit` ("actors remember", 007 §6). Steady path is
// 0-alloc: the ring is a fixed std::array and it only writes the reused context buffers (N1).
template <std::size_t K>
class MovingAverageNode final : public INode {
    static_assert(K > 0, "MovingAverageNode window must be non-empty");

public:
    NodeResult process(ProcessingContext& ctx) noexcept override {
        double sample = 0.0;  // fold the current working-set tags into one sample
        for (const auto& tag : ctx.tags) {
            sample += tag.value;
        }
        ring_[head_] = sample;
        head_ = (head_ + 1) % K;
        if (count_ < K) {
            ++count_;  // window still filling after a cold (re)start
        }
        double sum = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            sum += ring_[i];
        }
        const double avg = sum / static_cast<double>(count_);
        ctx.output.push_back(avg);
        ctx.events.push_back(Event{"MovingAverage", avg});
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    // Transient-state depth: how many samples are in the window (0 when cold, K when warm). Lets a
    // test observe that node state was LOST and is rebuilding after a restart (007 §6).
    [[nodiscard]] std::size_t warm_samples() const noexcept { return count_; }

    static constexpr NodeDescriptor kDesc{NodeCategory::Transform, "aero.transform.moving_average"};

private:
    std::array<double, K> ring_{};  // TRANSIENT per-instance state (007 §6) — lost + rebuilt on restart
    std::size_t head_ = 0;
    std::size_t count_ = 0;
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
