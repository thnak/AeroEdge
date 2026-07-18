// AeroEdge SDK — ProcessingContext (spec 003).
//
// The per-Command mutable struct threaded by reference through a Flow's nodes. Created once per
// Command, reused across Commands on an actor (clear-not-free → amortized 0-alloc on the execute
// path, 003 §4), destroyed with the actor. Never copied, never serialized, never escapes the flow
// (I6). This is the Phase-1 shape: fields grow as node categories need them, but the lifetime and
// ownership rules are stable.
#pragma once

#include <cstdint>
#include <string>
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

// A report an Output node stages for the MES gateway (012 §4). Kept at the SDK layer — POD + string
// numbers/views only — so ANY node can stage one WITHOUT an upward include of aero-mes (which would be
// a cyclic dependency, R1). The MesGateway maps this to the canonical `MesReport` and owns the
// idempotency-key/timestamp assignment (012 §3). Views point at node-config / static storage so
// staging one is 0-alloc on the steady path (N1), exactly like SumOutputNode staging into `output`.
struct StagedMesReport {
    enum class Kind : std::uint8_t { Production, Alarm, TagSample };
    Kind kind = Kind::Production;
    std::string_view line;   // production line / device id (static or config storage)
    std::string_view label;  // metric name / alarm code
    double value = 0.0;      // produced count / measurement / severity
};

using TagCollection = std::vector<Tag>;
using EventBuffer = std::vector<Event>;

struct ProcessingContext {
    // --- input (borrowed; dies with the flow, I6) ---
    const Frame* frame = nullptr;

    // Raw frame bytes for byte-oriented Source/Transform nodes (JSON parse, CRC, Modbus register
    // decode — 005/006). The Phase-2 streaming Frame carries only a scalar; a byte payload is the
    // honest shape for a decode Source that parses a wire frame (003 §Frame). Owned here (cleared, not
    // freed) so a decode Source's input is a stable span for the flow's duration (I6).
    std::string payload;

    // --- working set + staged outputs (nodes write here; the actor commits/publishes after) ---
    TagCollection tags;       // decoded/normalized signals
    std::vector<double> output;  // Output nodes stage egress here
    EventBuffer events;       // Events to publish after commit (002)
    std::vector<StagedMesReport> mes_reports;  // MES reports staged by Output nodes (012 §4), drained
                                               // by the actor into the MesGateway outbox at commit

    // --- flow status ---
    bool failed = false;
    std::size_t failed_step = 0;

    // Reset for reuse on the next Command: clear buffers but KEEP capacity (amortized 0-alloc).
    void reset(const Frame* f) noexcept {
        frame = f;
        payload.clear();
        tags.clear();
        output.clear();
        events.clear();
        mes_reports.clear();
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
