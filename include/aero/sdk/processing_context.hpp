// AeroEdge SDK — ProcessingContext (spec 003).
//
// The per-Command mutable struct threaded by reference through a Flow's nodes. Created once per
// Command, reused across Commands on an actor (clear-not-free → amortized 0-alloc on the execute
// path, 003 §4), destroyed with the actor. Never copied, never serialized, never escapes the flow
// (I6). This is the Phase-1 shape: fields grow as node categories need them, but the lifetime and
// ownership rules are stable.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aero {

// Upper bound on a Frame's byte payload (006 §4). Fixed-size, no heap: a Frame flows through a Quark
// 024 StreamActivation ring slot AND (as runtime::ReceiveFrame, runtime/flow_actor.hpp) an actor
// mailbox `tell()`. The ring slot has no size cap, but the mailbox does: `tell()` sends through
// quark::detail::MessagePool, whose inline cell is a HARD, non-configurable 192 bytes
// (quark/detail/message_pool.hpp kMaxPayload — "oversized messages are a compile error at the send
// site"). ReceiveFrame carries raw (8B) + payload_len (2B) + this array, so the array itself must stay
// <= ~182B to keep the whole struct under 192B; 128 is a round number comfortably inside that budget
// (leaves headroom, still 64 Modbus holding registers — ample for a single poll response, 006 §6.1).
inline constexpr std::size_t kMaxFramePayload = 128;

// The triggering payload: a scalar (raw) plus an optional byte payload for byte-oriented Source nodes
// (ModbusDecodeNode, JsonParseNode — nodes/compute_nodes.hpp). Real frames are byte spans viewing the
// Quark 024 stream slot / a shard payload arena, valid only for the flow's duration (006 §4). Kept
// trivially copyable (fixed array, no heap) so it stays cheap to copy through the stream ring + mailbox.
struct Frame {
    std::int64_t raw = 0;
    std::uint16_t payload_len = 0;
    std::array<std::byte, kMaxFramePayload> payload{};
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

// A request an Output node stages for the HTTP egress actor (019 slice: aero.output.http). Same
// SDK-layer reasoning as StagedMesReport: POD, no upward include of the egress module (R1). `url` and
// `method` are views into the node's own config storage (stable, 0-alloc to stage — N1); `body` is
// built fresh per Command from the working-set tags, so it is owned, not a view (the same documented
// exception JsonParseNode already takes for allocation on the steady path, 005 §7).
struct StagedHttpRequest {
    std::string_view url;
    std::string_view method;         // "GET"|"POST"|"PUT"|"PATCH"|"DELETE"
    std::string_view headers_json;   // a flat {"Key":"Value",...} object, or "{}" — node config storage
    std::int32_t timeout_ms = 2000;
    std::string body;                // owned — built fresh per Command from the working-set tags
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
    std::vector<StagedHttpRequest> http_requests;  // HTTP requests staged by aero.output.http, drained
                                                    // into the HttpEgressActor (019 slice)

    // --- flow status ---
    bool failed = false;
    std::size_t failed_step = 0;

    // Reset for reuse on the next Command: clear buffers but KEEP capacity (amortized 0-alloc).
    void reset(const Frame* f) noexcept {
        frame = f;
        if (f != nullptr) {
            payload.assign(reinterpret_cast<const char*>(f->payload.data()), f->payload_len);
        } else {
            payload.clear();
        }
        tags.clear();
        output.clear();
        events.clear();
        mes_reports.clear();
        http_requests.clear();
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
