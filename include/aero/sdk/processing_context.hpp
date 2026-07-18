// AeroEdge SDK — ProcessingContext (spec 003).
//
// The per-Command mutable struct threaded by reference through a Flow's nodes. Created once per
// Command, reused across Commands on an actor (clear-not-free → amortized 0-alloc on the execute
// path, 003 §4), destroyed with the actor. Never copied, never serialized, never escapes the flow
// (I6). This is the Phase-1 shape: fields grow as node categories need them, but the lifetime and
// ownership rules are stable.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace aero {

// The triggering payload. Phase-1: a decoded scalar. Real frames are byte spans viewing the Quark
// 024 stream slot / a shard payload arena, valid only for the flow's duration (006 §4).
struct Frame {
    std::int64_t raw = 0;
};

// A named signal in the working set. `name` points at static storage (a node's literal).
struct Tag {
    std::string_view name;
    double value = 0.0;
};

// An immutable notification, appended during the flow and published post-commit (002 §3).
struct Event {
    std::string_view type;
    double value = 0.0;
};

using TagCollection = std::vector<Tag>;
using EventBuffer = std::vector<Event>;

struct ProcessingContext {
    // --- input (borrowed; dies with the flow, I6) ---
    const Frame* frame = nullptr;

    // --- working set + staged outputs (nodes write here; the actor commits/publishes after) ---
    TagCollection tags;       // decoded/normalized signals
    std::vector<double> output;  // Output nodes stage egress here
    EventBuffer events;       // Events to publish after commit (002)

    // --- flow status ---
    bool failed = false;
    std::size_t failed_step = 0;

    // Reset for reuse on the next Command: clear buffers but KEEP capacity (amortized 0-alloc).
    void reset(const Frame* f) noexcept {
        frame = f;
        tags.clear();
        output.clear();
        events.clear();
        failed = false;
        failed_step = 0;
    }

    // Cold pre-allocation so the steady execute path never grows a buffer.
    void reserve(std::size_t tags_cap, std::size_t out_cap, std::size_t ev_cap) {
        tags.reserve(tags_cap);
        output.reserve(out_cap);
        events.reserve(ev_cap);
    }
};

}  // namespace aero
