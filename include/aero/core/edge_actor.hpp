// AeroEdge core — EdgeActorBase (spec 001 §4).
//
// The CRTP base that sits BETWEEN a user's domain actor and quark::Actor. It is still a Quark actor
// (adds no runtime — no thread, no lock, no queue, I1); it only adds the Command → Flow → Event glue:
//   * holds a bound CompiledFlow + a reusable ProcessingContext (amortized 0-alloc, 003 §4)
//   * process_frame(): build context → execute the DAG → commit staged output → publish events (002)
//
// The concrete actor declares `using protocol = ...` and its `handle(...)` overloads and calls
// process_frame() from the Command handler — exactly the pattern spec 001 §3 sketches. Phase-1
// commit/publish are minimal (last output + event count); Phase-3 replaces commit with the Quark 012
// persistence path and publish with the real event bus (002 §4).
#pragma once

#include "aero/core/compiled_flow.hpp"
#include "aero/sdk/processing_context.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"

namespace aero {

template <class Derived, class... Policies>
class EdgeActorBase : public quark::Actor<Derived, Policies...> {
public:
    // Wire the compiled flow before the engine starts (like sample 03's downstream ref).
    void bind_flow(const CompiledFlow& flow) noexcept { flow_ = &flow; }

    // Observability accessors (used by ask handlers / Phase-4 metrics).
    [[nodiscard]] double last_output() const noexcept { return last_output_; }
    [[nodiscard]] long events_published() const noexcept { return events_published_; }
    [[nodiscard]] long frames_processed() const noexcept { return frames_processed_; }
    [[nodiscard]] bool last_failed() const noexcept { return last_failed_; }

protected:
    // Run the bound flow for one frame under the single-executor invariant (I2). Called from a
    // Command handler on the concrete actor.
    void process_frame(const Frame& frame) noexcept {
        ctx_.reset(&frame);
        if (flow_ != nullptr) {
            flow_->execute(ctx_);
        }
        commit();
        publish();
        ++frames_processed_;
    }

    // Durable-state promotion hook (007 §2, S1) — the customization point of the single write point.
    // Default: no durable state (Phase-1 actors). A persistent Derived overrides this to promote
    // committed facts (ctx staged output/events) into durable member fields and checkpoint them via
    // the Quark 012 `Store` seam (see aero/core/persistent_actor.hpp). Resolved statically through the
    // CRTP `Derived` — off the flow-execute hot path (runs at commit, never inside execute()).
    void on_commit(const ProcessingContext&) noexcept {}

private:
    // Commit: apply staged output to committed actor state (I5), then invoke the durable-state
    // promotion hook (007 §2). Persistence work happens HERE — at the commit point, off the 0-alloc
    // execute path (Quark 012 "not on the hot path").
    void commit() noexcept {
        last_failed_ = ctx_.failed;
        if (!ctx_.output.empty()) {
            last_output_ = ctx_.output.back();
        }
        static_cast<Derived*>(this)->on_commit(ctx_);
    }

    // Publish: emit Events post-commit (002 §3). Phase-1 counts them; Phase-2/4 `tell` subscribers.
    void publish() noexcept { events_published_ += static_cast<long>(ctx_.events.size()); }

    const CompiledFlow* flow_ = nullptr;
    ProcessingContext ctx_;  // reused across Commands (amortized 0-alloc, 003 §4)
    double last_output_ = 0.0;
    long events_published_ = 0;
    long frames_processed_ = 0;
    bool last_failed_ = false;
};

}  // namespace aero
