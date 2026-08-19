// AeroEdge M9.3 driver — OpcUaSubscriptionDriver: the PUSH counterpart to OpcUaDriver (opcua_driver.hpp,
// which is pull/poll-only, 006 §6.1), closing 018 §8's "OPC-UA Subscriptions/MonitoredItems" backlog
// item. A SEPARATE class/type_id (`aero.driver.opcua_subscribe`), not a mode flag on OpcUaDriver: push
// vs pull is a different `IDriver` invocation contract (`run()` vs `poll()`, `DriverDescriptor::
// poll_driven`), and `DriverDescriptor` is a `static constexpr` per-CLASS value, not per-instance — so
// one driver class can't be "poll sometimes, push other times" without either a bigger interface change
// or a runtime probe of run()/poll(), neither of which this PR needs. Every driver class in this tree
// already commits to exactly one invocation model (GeneratorDriver push-only, ModbusTcpDriver/
// ModbusRtuDriver/OpcUaDriver pull-only) — this follows that same precedent.
//
// v1 SCOPE: connect to ONE endpoint, create ONE Subscription (open62541 default parameters — 500ms
// publishing interval, see UA_CreateSubscriptionRequest_default), and ONE MonitoredItem (data-change,
// UA_ATTRIBUTEID_VALUE) per configured NodeId. `run()` pumps `UA_Client_run_iterate()` in a loop until
// the StopToken fires; open62541's client has no internal thread of its own, so the data-change callback
// fires SYNCHRONOUSLY from inside that call, on this same thread — safe to push into the bound sink
// directly from the callback, no cross-thread handoff needed. NO write()/method-call surface in v1
// (`writable=false` — OpcUaDriver already owns that surface for the SAME endpoint if a deployment needs
// both push notifications and writes, run two driver instances); NO Events, only DataChange
// notifications; NO server-requested-parameter renegotiation (accepts whatever the server revises).
//
// PAYLOAD SHAPE — DELIBERATELY DIFFERENT FROM OpcUaDriver's POLL MODE: each Frame carries exactly ONE
// `{"nodeId": value}` JSON entry (one per data-change notification), not a batched multi-NodeId object.
// This means subscription mode has NO analogue to poll mode's "~3 NodeIds per poll" batching cap (018
// §3) — the per-entry payload only needs to fit ONE NodeId-string+value pair under kMaxFramePayload
// (checked per-entry at open()), so a deployment can subscribe to far more NodeIds than it could poll in
// one batch. `aero::nodes::JsonParseNode` still decodes each frame unchanged (same `{"id":value}` shape
// as poll mode's own object, just with exactly one key).
//
// RECONNECT (006 §8): same bounded-backoff shape as every other driver here. `run()`'s outer loop
// reconnects + recreates the Subscription/MonitoredItems from scratch on any connection loss — open62541
// does not resurrect a Subscription across a fresh TCP/session, so there is nothing to resume, only to
// rebuild. Detection leans on open62541's own `connectivityCheckInterval` (set in open(), 2s) rather than
// a hand-rolled probe: it drives a periodic ASYNC liveness read inside `UA_Client_run_iterate()` itself,
// closing the connection on failure — `connection_lost()` (via `UA_Client_getState`) then observes that.
// A bare channel/session-state check alone can look nominally healthy for a long time against a peer
// that vanishes without a clean TCP FIN (open62541's own subscription-inactivity heuristic alone took
// 30+ seconds to notice in this driver's own test); `client_cfg->timeout` is also shortened from
// open62541's 5s default (`kServiceCallTimeoutMs`) so a stalled connect/service call fails promptly too.
//
// COMPILED-OUT BUILDS (AERO_ENABLE_OPCUA=OFF): mirrors opcua_driver.hpp's own stub pattern exactly, so
// runtime.hpp's unconditional include still builds either way.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "aero/drivers/opcua_security.hpp"
#include "aero/sdk/driver.hpp"
#include "nlohmann/json.hpp"

// Config schema (015 U1), shared by both the AERO_OPCUA_ENABLED and the "not compiled in" stub branch
// below — mirrors opcua_driver.hpp's own kOpcUaFields (no `browse_root`: this driver has no browse mode).
namespace aero::drivers {
inline constexpr std::array<FieldSpec, 3> kOpcUaSubscribeFields{{
    {.key = "endpoint", .label = "Endpoint URL", .type = FieldType::String, .required = true},
    {.key = "node_ids", .label = "Node IDs", .type = FieldType::StringArray},
    {.key = "security", .label = "Security", .type = FieldType::Object,
     .tier2_hint = "opcua-security"},
}};
}  // namespace aero::drivers

#if defined(AERO_OPCUA_ENABLED) && AERO_OPCUA_ENABLED

#include <chrono>
#include <thread>

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>

namespace aero::drivers {

class OpcUaSubscriptionDriver final : public IDriver {
public:
    // `security` (default-constructed = disabled, see opcua_security.hpp) opts into Sign/SignAndEncrypt
    // over a client certificate (M9.4, 018 §8) — additive, existing 2-arg call sites keep working
    // unchanged at MessageSecurityMode::None.
    OpcUaSubscriptionDriver(std::string endpoint, std::vector<std::string> node_ids,
                             OpcUaSecurityConfig security = {}) noexcept
        : endpoint_(std::move(endpoint)), node_id_strings_(std::move(node_ids)), security_(std::move(security)) {}

    ~OpcUaSubscriptionDriver() override { close(); }

    // Config-time rejection, same posture as OpcUaDriver::open() — never a silently truncated frame.
    // Parses every configured NodeId string here too (dial happens in run(), not here — a bad `node_ids`
    // entry is a config error, independent of device reachability).
    DriverStatus open(const DriverConfig& cfg) noexcept override {
        if (!fits_payload_budget()) return DriverStatus::Error;
        if (!parse_node_ids()) return DriverStatus::Error;
        rate_hz_ = cfg.rate_hz;  // unused by this push driver — kept only for status/observability parity

        client_ = UA_Client_new();
        if (!client_) return DriverStatus::Error;
        UA_ClientConfig* client_cfg = UA_Client_getConfig(client_);
        if (!apply_security_config(client_cfg, security_)) {
            UA_Client_delete(client_);
            client_ = nullptr;
            return DriverStatus::Error;
        }
        // Shorter than open62541's own 5s defaults for BOTH knobs — a stalled connect/service-call
        // shouldn't eat 5 real seconds before this driver's own bounded backoff (006 §8) even gets a
        // chance to run, and a dead peer that never sends a clean TCP FIN needs an ACTIVE check to be
        // noticed promptly (passive channel/session state alone can look nominally healthy for a long
        // time — see connectivityCheckInterval's own doc comment: a periodic async read whose failure
        // closes the connection, which connection_lost() below then observes).
        client_cfg->timeout = kServiceCallTimeoutMs;
        client_cfg->connectivityCheckInterval = kConnectivityCheckIntervalMs;

        opened_ = true;
        return DriverStatus::Ok;
    }

    // PUSH producer loop (006 §6.2). Connects, creates a Subscription + one MonitoredItem per configured
    // NodeId, then pumps the client's event loop until stopped. On any connection loss it tears the
    // (now-dead) subscription/session down and retries the whole sequence with bounded backoff (006 §8).
    DriverStatus run(StreamSink<Frame> sink, StopToken stop) noexcept override {
        if (!opened_) return DriverStatus::Error;

        backoff_ms_ = kInitialBackoffMs;
        while (!stop.stop_requested()) {
            if (UA_Client_connect(client_, endpoint_.c_str()) != UA_STATUSCODE_GOOD) {
                if (!backoff_wait(stop)) return DriverStatus::Ok;  // StopToken fired mid-backoff
                continue;
            }
            backoff_ms_ = kInitialBackoffMs;  // reset on a successful connect

            const UA_CreateSubscriptionRequest sub_req = UA_CreateSubscriptionRequest_default();
            const UA_CreateSubscriptionResponse sub_resp =
                UA_Client_Subscriptions_create(client_, sub_req, nullptr, nullptr, nullptr);
            if (sub_resp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
                UA_Client_disconnect(client_);
                if (!backoff_wait(stop)) return DriverStatus::Ok;
                continue;
            }
            const UA_UInt32 sub_id = sub_resp.subscriptionId;

            // Stable addresses for the whole subscription's lifetime — open62541 stores this pointer as
            // each MonitoredItem's context and hands it back to data_change_callback() verbatim, so the
            // vector must not reallocate/move while any item is live (sized once, up front, never
            // resized afterward).
            std::vector<MonItemContext> mon_ctxs(node_ids_.size());
            for (std::size_t i = 0; i < node_ids_.size(); ++i) mon_ctxs[i] = MonItemContext{this, &sink, i};
            for (std::size_t i = 0; i < node_ids_.size(); ++i) {
                const UA_MonitoredItemCreateRequest mon_req =
                    UA_MonitoredItemCreateRequest_default(node_ids_[i]);
                // A per-item creation failure (e.g. unknown NodeId) just means that one NodeId never
                // notifies — not fatal to the whole subscription, mirrors poll mode's own "skip a bad
                // entry" posture (018 §5).
                (void)UA_Client_MonitoredItems_createDataChange(client_, sub_id, UA_TIMESTAMPSTORETURN_BOTH,
                                                                 mon_req, &mon_ctxs[i], &data_change_callback,
                                                                 nullptr);
            }

            // connectivityCheckInterval (set in open()) drives an async liveness read inside
            // run_iterate() itself — a dead connection surfaces via a bad run_iterate() return and/or
            // connection_lost() well before open62541's own (much slower) subscription-inactivity
            // heuristic would notice.
            while (!stop.stop_requested()) {
                const UA_StatusCode rc = UA_Client_run_iterate(client_, kIterateTimeoutMs);
                if (rc != UA_STATUSCODE_GOOD || connection_lost()) break;
            }

            UA_Client_Subscriptions_deleteSingle(client_, sub_id);
            UA_Client_disconnect(client_);
            if (stop.stop_requested()) return DriverStatus::Ok;
            if (!backoff_wait(stop)) return DriverStatus::Ok;  // reconnect loop continues above
        }
        return DriverStatus::Ok;
    }

    // PUSH driver only — no analogue to a non-looping single read (this is the "push" counterpart to
    // OpcUaDriver, not a hybrid).
    DriverStatus poll(StreamSink<Frame> /*sink*/) noexcept override { return DriverStatus::Unsupported; }

    // No write surface in v1 (see file banner) — a deployment needing both notifications and writes to
    // the same endpoint runs an OpcUaDriver instance alongside this one.

    void close() noexcept override {
        for (auto& id : node_ids_) UA_NodeId_clear(&id);
        node_ids_.clear();
        if (client_) {
            UA_Client_delete(client_);  // safe whether or not currently connected (run()'s loop always
            client_ = nullptr;          // disconnects before returning)
        }
        opened_ = false;
    }

    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr DriverDescriptor kDesc{"aero.driver.opcua_subscribe", /*writable*/ false,
                                             /*poll_driven*/ false, kOpcUaSubscribeFields};

private:
    static constexpr int kInitialBackoffMs = 200;
    static constexpr int kMaxBackoffMs = 5000;
    static constexpr UA_UInt32 kIterateTimeoutMs = 100;  // run_iterate's own internal wait budget per call
    // Both shorter than open62541's own 5s defaults — see open()'s comment on why.
    static constexpr UA_UInt32 kServiceCallTimeoutMs = 2000;
    static constexpr UA_UInt32 kConnectivityCheckIntervalMs = 2000;
    // Per-entry budget (see file banner: no multi-NodeId batching cap here, unlike poll mode) — quotes +
    // colon + braces + a generous ~15-digit double.
    static constexpr std::size_t kPerEntryOverhead = 24;

    [[nodiscard]] bool fits_payload_budget() const noexcept {
        for (const auto& s : node_id_strings_) {
            if (s.size() + kPerEntryOverhead > aero::kMaxFramePayload) return false;
        }
        return true;
    }

    [[nodiscard]] bool parse_node_ids() noexcept {
        node_ids_.clear();
        node_ids_.reserve(node_id_strings_.size());
        for (const auto& s : node_id_strings_) {
            UA_NodeId id;
            UA_NodeId_init(&id);
            UA_String ua_s = UA_String_fromChars(s.c_str());
            const UA_StatusCode rc = UA_NodeId_parse(&id, ua_s);
            UA_String_clear(&ua_s);
            if (rc != UA_STATUSCODE_GOOD) {
                UA_NodeId_clear(&id);
                for (auto& parsed : node_ids_) UA_NodeId_clear(&parsed);
                node_ids_.clear();
                return false;
            }
            node_ids_.push_back(id);
        }
        return true;
    }

    bool connection_lost() noexcept {
        UA_SecureChannelState cs;
        UA_SessionState ss;
        UA_StatusCode connect_status;
        UA_Client_getState(client_, &cs, &ss, &connect_status);
        return !(cs == UA_SECURECHANNELSTATE_OPEN && ss == UA_SESSIONSTATE_ACTIVATED);
    }

    // Sleeps up to the current backoff window in short slices so `stop` is observed promptly (006 §8),
    // then doubles the backoff for next time (capped). Returns false iff `stop` fired mid-wait.
    bool backoff_wait(const StopToken& stop) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoff_ms_);
        while (std::chrono::steady_clock::now() < deadline) {
            if (stop.stop_requested()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        backoff_ms_ = backoff_ms_ * 2 < kMaxBackoffMs ? backoff_ms_ * 2 : kMaxBackoffMs;
        return true;
    }

    static bool variant_to_double(const UA_Variant& v, double& out) noexcept {
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_DOUBLE])) {
            out = *static_cast<const UA_Double*>(v.data);
            return true;
        }
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_FLOAT])) {
            out = *static_cast<const UA_Float*>(v.data);
            return true;
        }
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_INT32])) {
            out = *static_cast<const UA_Int32*>(v.data);
            return true;
        }
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_UINT32])) {
            out = *static_cast<const UA_UInt32*>(v.data);
            return true;
        }
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_INT16])) {
            out = *static_cast<const UA_Int16*>(v.data);
            return true;
        }
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_UINT16])) {
            out = *static_cast<const UA_UInt16*>(v.data);
            return true;
        }
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_SBYTE])) {
            out = *static_cast<const UA_SByte*>(v.data);
            return true;
        }
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_BYTE])) {
            out = *static_cast<const UA_Byte*>(v.data);
            return true;
        }
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_INT64])) {
            out = static_cast<double>(*static_cast<const UA_Int64*>(v.data));
            return true;
        }
        if (UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_UINT64])) {
            out = static_cast<double>(*static_cast<const UA_UInt64*>(v.data));
            return true;
        }
        return false;
    }

    // Per-MonitoredItem context open62541 hands back verbatim to data_change_callback() (see run()'s own
    // comment on lifetime). `index` looks up the NodeId string for this item's JSON key.
    struct MonItemContext {
        OpcUaSubscriptionDriver* self;
        StreamSink<Frame>* sink;
        std::size_t index;
    };

    // Invoked SYNCHRONOUSLY from inside UA_Client_run_iterate() on run()'s own thread (open62541's
    // client has no internal thread) — safe to push into `sink` directly, no cross-thread handoff. A
    // non-numeric or missing value just skips this notification (mirrors poll mode's own per-entry skip,
    // never a crash).
    static void data_change_callback(UA_Client*, UA_UInt32, void*, UA_UInt32, void* mon_context,
                                      UA_DataValue* value) noexcept {
        auto* ctx = static_cast<MonItemContext*>(mon_context);
        if (value == nullptr || !value->hasValue) return;
        double v = 0.0;
        if (!variant_to_double(value->value, v)) return;

        nlohmann::json j = nlohmann::json::object();
        j[ctx->self->node_id_strings_[ctx->index]] = v;
        const std::string body = j.dump();
        // Bug guard — fits_payload_budget() at open() should already guarantee this per-entry.
        if (body.size() > aero::kMaxFramePayload) return;

        Frame frame{};
        frame.payload_len = static_cast<std::uint16_t>(body.size());
        std::memcpy(frame.payload.data(), body.data(), body.size());
        while (!ctx->sink->try_push(frame)) {
            std::this_thread::yield();  // lossless backpressure (006 §3): stall, never drop. Note: this
            // blocks INSIDE the run_iterate() callback, delaying the next iterate() call until credit
            // returns — an accepted v1 tradeoff (same "stall, never drop" posture used everywhere else),
            // not a correctness problem.
        }
    }

    std::string endpoint_;
    std::vector<std::string> node_id_strings_;  // config order; index-paired with node_ids_
    std::uint32_t rate_hz_ = 0;  // unused by this driver (see open())
    OpcUaSecurityConfig security_;  // M9.4: empty == disabled (MessageSecurityMode::None)

    bool opened_ = false;
    UA_Client* client_ = nullptr;
    std::vector<UA_NodeId> node_ids_;  // parsed at open(), cleared at close()

    int backoff_ms_ = kInitialBackoffMs;
};

}  // namespace aero::drivers

#else  // !AERO_OPCUA_ENABLED — mirrors opcua_driver.hpp's own "not compiled in" stub pattern exactly.

namespace aero::drivers {

class OpcUaSubscriptionDriver final : public IDriver {
public:
    OpcUaSubscriptionDriver(std::string /*endpoint*/, std::vector<std::string> /*node_ids*/,
                             OpcUaSecurityConfig /*security*/ = {}) noexcept {}

    DriverStatus open(const DriverConfig& /*cfg*/) noexcept override { return DriverStatus::Error; }
    DriverStatus run(StreamSink<Frame> /*sink*/, StopToken /*stop*/) noexcept override {
        return DriverStatus::Error;
    }
    DriverStatus poll(StreamSink<Frame> /*sink*/) noexcept override { return DriverStatus::Unsupported; }
    void close() noexcept override {}
    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr DriverDescriptor kDesc{"aero.driver.opcua_subscribe", /*writable*/ false,
                                             /*poll_driven*/ false, kOpcUaSubscribeFields};
};

}  // namespace aero::drivers

#endif  // AERO_OPCUA_ENABLED
