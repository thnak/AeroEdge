// AeroEdge core — the CompiledFlow executor (spec 004).
//
// A Flow is compiled ONCE (nodes resolved + ordered) into an immutable plan; execution walks that
// plan per Command, threading one ProcessingContext (I3). Phase-1 supports a linear pipeline
// (Source → Transform → … → Output) with Stop/Error short-circuit; branch/fan-out (004 §2.2) and
// the deploy-time Flow Compiler validation (004 §2.1) land in Phase 5. Execution is a straight array
// walk: no graph resolution, no allocation, no locking — one virtual INode::process per step (I7).
#pragma once

#include <cstddef>
#include <vector>

#include "aero/sdk/node.hpp"
#include "aero/sdk/processing_context.hpp"

namespace aero {

class CompiledFlow {
public:
    // --- compile-time wiring (deploy, once) ---
    CompiledFlow& add(INode& node) {
        steps_.push_back(&node);
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return steps_.size(); }

    // --- execute (per Command, hot) ---
    void execute(ProcessingContext& ctx) const noexcept {
        for (std::size_t i = 0; i < steps_.size(); ++i) {
            const NodeResult r = steps_[i]->process(ctx);  // one indirect call per node (I7)
            if (r == NodeResult::Stop) break;              // Rule short-circuit — not a failure
            if (r == NodeResult::Error) {
                ctx.failed = true;
                ctx.failed_step = i;
                break;
            }
        }
    }

private:
    std::vector<INode*> steps_;  // pre-topologically-ordered plan (I3)
};

}  // namespace aero
