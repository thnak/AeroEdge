// AeroEdge M9b driver — OpcUaDriver: a real OPC-UA client PULL driver (018 §Multi-protocol southbound,
// 006 §6.1), over open62541 (github.com/open62541/open62541, MPL-2.0), the standard open-source C99
// OPC-UA SDK. Same posture as ModbusTcpDriver (drivers/modbus_tcp_driver.hpp, this milestone's worked
// reference): PULL, not PUSH — every read happens inside one `poll()` call, `run()` is a hard
// Unsupported. v1 SCOPE (explicit, not an oversight): connect to ONE configured endpoint, read a fixed
// set of configured NodeIds via UA_Client_readValueAttribute, write a scalar to any NodeId via
// UA_Client_writeValueAttribute (M9.1 PR E, 018 §8 — the OPC-UA counterpart to ModbusTcpDriver's FC06).
// NO security policies (Sign/SignAndEncrypt is backlog), NO Subscriptions/MonitoredItems, NO browsing,
// NO method calls — pure poll-configured-NodeIds plus single-NodeId scalar write only.
//
// REUSE, NOT REBUILD (the whole point): this driver's job stops at serializing the polled NodeId->value
// results into a FLAT JSON OBJECT (e.g. {"ns=2;s=Temperature":23.5}) written into the Frame's byte
// payload — `aero::nodes::JsonParseNode` (nodes/compute_nodes.hpp, "aero.source.json", UNMODIFIED)
// already decodes exactly that shape into Tags downstream. No new decode node needed.
//
// PAYLOAD BUDGET (006 §4, aero::kMaxFramePayload == 128 — see processing_context.hpp's banner for why):
// a serialized JSON object of NodeId-string -> number entries is tight against 128 bytes. open() rejects
// a `node_ids` list whose WORST-CASE serialized size can't fit, using a conservative ~40-bytes-per-entry
// estimate (`"ns=X;s=SomeLongName":123.456789,` ballpark) — so a v1 deployment realistically gets ~3
// NodeIds per poll. This is a real, documented v1 constraint, not a bug.
//
// RECONNECT (006 §8): same bounded-backoff shape as ModbusTcpDriver — 200ms initial, doubling, capped at
// 5s, reset to 200ms the instant a (re)connect succeeds. On a read failure, `UA_Client_getState` is
// consulted to tell a genuine connection loss (channel/session no longer OPEN/ACTIVATED — reconnect
// needed) apart from a per-NodeId, connection-healthy failure (unknown NodeId, non-numeric value, ...),
// which just skips that one entry for this poll cycle rather than tearing down the connection.
//
// COMPILED-OUT BUILDS (AERO_ENABLE_OPCUA=OFF, root CMakeLists.txt): AERO_OPCUA_ENABLED is then undefined
// and this header compiles a stub with the IDENTICAL class/method surface whose `open()` always returns
// DriverStatus::Error — mirrors aero/pal/tls.hpp's "not compiled in" stub pattern exactly, so code that
// unconditionally includes this header (runtime.hpp's register_builtins()) still builds either way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "aero/sdk/driver.hpp"
#include "nlohmann/json.hpp"

#if defined(AERO_OPCUA_ENABLED) && AERO_OPCUA_ENABLED

#include <chrono>
#include <thread>

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>

namespace aero::drivers {

// OpcUaDriver — pure client-POLL over open62541 (v1 scope, see file banner). Construction takes
// ownership of its config (endpoint + node_ids) at build time — the factory in
// runtime.hpp::register_builtins() reads these straight out of the deploy-time JSON, mirroring
// ModbusTcpDriver's own constraint that DriverConfig's narrow endpoint/frame_count/rate_hz fields don't
// fit this driver's shape.
class OpcUaDriver final : public IDriver {
public:
    OpcUaDriver(std::string endpoint, std::vector<std::string> node_ids) noexcept
        : endpoint_(std::move(endpoint)), node_id_strings_(std::move(node_ids)) {}

    ~OpcUaDriver() override { close(); }

    // Config-time rejection (never a silently truncated/corrupted frame, per Part 1's
    // Frame::payload_len/payload budget) — see file banner for the ~40-bytes/entry worst-case estimate.
    // Parses every configured NodeId string here too (a malformed NodeId string is also a config error,
    // independent of device reachability) — dial LAST, so a bad `node_ids` entry never even attempts a
    // socket. Mirrors ModbusTcpDriver: `opened_` is armed before the dial attempt, so a reachability
    // failure here still leaves the driver armed for a later poll() to retry (see ensure_connected()).
    DriverStatus open(const DriverConfig& cfg) noexcept override {
        if (!fits_payload_budget()) return DriverStatus::Error;
        if (!parse_node_ids()) return DriverStatus::Error;

        rate_hz_ = cfg.rate_hz;  // advisory poll-interval hint only (see ModbusTcpDriver's own banner)
        opened_ = true;

        client_ = UA_Client_new();
        if (!client_) return DriverStatus::Error;
        UA_ClientConfig_setDefault(UA_Client_getConfig(client_));

        backoff_ms_ = kInitialBackoffMs;
        next_attempt_at_ = std::chrono::steady_clock::now();
        connected_ = try_connect();
        return connected_ ? DriverStatus::Ok : DriverStatus::Error;
    }

    // PULL driver only (006 §6.1) — this class never loops; poll() is the whole surface.
    DriverStatus run(StreamSink<Frame> /*sink*/, StopToken /*stop*/) noexcept override {
        return DriverStatus::Unsupported;
    }

    // One read cycle over every configured NodeId, non-looping. A genuine connection loss (detected via
    // UA_Client_getState after a failed read) tears the session down so the NEXT poll() reconnects
    // (bounded backoff, see file banner) rather than wedging against a dead channel. A per-NodeId
    // failure that ISN'T a connection loss (unknown NodeId, non-numeric type) just skips that entry —
    // never crashes, never corrupts the frame. On success, pushes exactly one Frame, retrying try_push
    // on backpressure (never dropping a frame, 006 §3).
    DriverStatus poll(StreamSink<Frame> sink) noexcept override {
        if (!opened_) return DriverStatus::Error;
        if (!connected_ && !ensure_connected()) return DriverStatus::Error;

        nlohmann::json j = nlohmann::json::object();
        bool conn_lost = false;
        for (std::size_t i = 0; i < node_ids_.size(); ++i) {
            UA_Variant value;
            UA_Variant_init(&value);
            const UA_StatusCode rc = UA_Client_readValueAttribute(client_, node_ids_[i], &value);
            if (rc == UA_STATUSCODE_GOOD) {
                double v = 0.0;
                if (variant_to_double(value, v)) j[node_id_strings_[i]] = v;
                // else: value isn't numeric-convertible -> skip this entry for this poll cycle only.
            } else if (connection_lost()) {
                conn_lost = true;
                UA_Variant_clear(&value);
                break;
            }
            // else: a well-formed OPC-UA error over a healthy connection (e.g. bad NodeId) -> skip.
            UA_Variant_clear(&value);
        }

        if (conn_lost) {
            teardown_session();  // (006 §8) -> reconnect w/ backoff on a later poll()
            return DriverStatus::Error;
        }

        const std::string body = j.dump();
        // Should have been caught by open()'s config-time budget check; treat as a bug guard, never a
        // silently truncated frame.
        if (body.size() > aero::kMaxFramePayload) return DriverStatus::Error;

        Frame frame{};
        frame.payload_len = static_cast<std::uint16_t>(body.size());
        std::memcpy(frame.payload.data(), body.data(), body.size());

        while (!sink.try_push(frame)) {
            std::this_thread::yield();  // lossless backpressure (006 §3): stall, never drop
        }
        return DriverStatus::Ok;
    }

    // A device-directed write (006 §7) via UA_Client_writeValueAttribute — the OPC-UA counterpart to
    // ModbusTcpDriver's FC06. `cmd.target` is the NodeId string, parsed the same way as a configured
    // `node_ids` entry (UA_NodeId_parse — a malformed target is a clean Error before any I/O); `cmd.value`
    // is written as a UA_Double, matching this driver's own read-side convention of normalizing every
    // value to double (variant_to_double()). Same connection-loss detection and teardown posture as
    // poll() (006 §8): a genuine channel/session loss tears the session down for the next poll()/write()
    // to reconnect; a well-formed OPC-UA error over a healthy connection (bad NodeId, type mismatch,
    // access denied) is a clean DriverStatus::Error, connection stays up.
    DriverStatus write(const DeviceCommand& cmd) noexcept override {
        if (!opened_) return DriverStatus::Error;

        // Parse the target BEFORE touching the connection (mirrors ModbusTcpDriver::write()): a
        // malformed target is a config error, not a device/connection error, so it short-circuits
        // without a reconnect attempt even when the driver is currently disconnected.
        UA_NodeId id;
        UA_NodeId_init(&id);
        UA_String ua_target = UA_String_fromChars(std::string(cmd.target).c_str());
        const UA_StatusCode parse_rc = UA_NodeId_parse(&id, ua_target);
        UA_String_clear(&ua_target);
        if (parse_rc != UA_STATUSCODE_GOOD) {
            UA_NodeId_clear(&id);
            return DriverStatus::Error;
        }

        if (!connected_ && !ensure_connected()) {
            UA_NodeId_clear(&id);
            return DriverStatus::Error;
        }

        UA_Variant value;
        UA_Variant_init(&value);
        UA_Double v = static_cast<UA_Double>(cmd.value);
        const UA_StatusCode variant_rc = UA_Variant_setScalarCopy(&value, &v, &UA_TYPES[UA_TYPES_DOUBLE]);
        if (variant_rc != UA_STATUSCODE_GOOD) {
            UA_NodeId_clear(&id);
            return DriverStatus::Error;
        }

        const UA_StatusCode rc = UA_Client_writeValueAttribute(client_, id, &value);
        UA_Variant_clear(&value);
        UA_NodeId_clear(&id);

        if (rc != UA_STATUSCODE_GOOD) {
            if (connection_lost()) {
                teardown_session();  // (006 §8) -> reconnect w/ backoff on a later poll()/write()
                return DriverStatus::Error;
            }
            return DriverStatus::Error;  // well-formed device-level error, connection stays healthy
        }
        return DriverStatus::Ok;
    }

    void close() noexcept override {
        for (auto& id : node_ids_) UA_NodeId_clear(&id);
        node_ids_.clear();
        if (client_) {
            if (connected_) UA_Client_disconnect(client_);
            UA_Client_delete(client_);
            client_ = nullptr;
        }
        connected_ = false;
        opened_ = false;
    }

    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr DriverDescriptor kDesc{"aero.driver.opcua", /*writable*/ true};

private:
    // Connect-attempt timing is entirely open62541's own (UA_Client_connect blocks internally per its own
    // ClientConfig timeout, no separate dial_tcp()-style timeout knob needed here — unlike ModbusTcpDriver
    // over a raw socket).
    static constexpr int kInitialBackoffMs = 200;
    static constexpr int kMaxBackoffMs = 5000;
    // Conservative worst-case per-entry serialized size (quoted NodeId string + colon + a long-ish
    // double + comma), see file banner. 2 accounts for the object's own "{" "}".
    static constexpr std::size_t kBytesPerEntryEstimate = 40;

    [[nodiscard]] bool fits_payload_budget() const noexcept {
        return node_id_strings_.size() * kBytesPerEntryEstimate + 2 <= aero::kMaxFramePayload;
    }

    // Parses every configured NodeId string (UA_NodeId_parse) into node_ids_, in the SAME order as
    // node_id_strings_ (index-paired: poll() looks up node_id_strings_[i] as the JSON key for
    // node_ids_[i]'s value). String/ByteString NodeIds heap-allocate their identifier (open62541's own
    // doc comment on UA_NodeId_parse) — cleaned up in close() via UA_NodeId_clear. On any parse failure,
    // everything parsed so far is cleaned up and this returns false (a config error, never a partial
    // node_ids_ left dangling for open() to accidentally treat as valid).
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

    // Lazily reconnect, gated by the backoff clock — see file banner. false == still backing off, or the
    // connect attempt itself failed (in which case the backoff for the NEXT attempt is scheduled here).
    bool ensure_connected() noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_attempt_at_) return false;

        connected_ = try_connect();
        if (!connected_) {
            next_attempt_at_ = now + std::chrono::milliseconds(backoff_ms_);
            backoff_ms_ = backoff_ms_ * 2 < kMaxBackoffMs ? backoff_ms_ * 2 : kMaxBackoffMs;
        }
        return connected_;
    }

    bool try_connect() noexcept {
        const UA_StatusCode rc = UA_Client_connect(client_, endpoint_.c_str());
        if (rc != UA_STATUSCODE_GOOD) return false;
        backoff_ms_ = kInitialBackoffMs;  // reset on success
        next_attempt_at_ = std::chrono::steady_clock::now();
        return true;
    }

    // A read failing is NOT automatically a connection loss (a well-formed BadNodeIdUnknown over a
    // perfectly healthy channel is common and expected) — the channel/session state is the honest signal
    // (006 §8's "reconnect on ConnectionLost", not "reconnect on any device-level error").
    bool connection_lost() noexcept {
        UA_SecureChannelState cs;
        UA_SessionState ss;
        UA_StatusCode connect_status;
        UA_Client_getState(client_, &cs, &ss, &connect_status);
        return !(cs == UA_SECURECHANNELSTATE_OPEN && ss == UA_SESSIONSTATE_ACTIVATED);
    }

    void teardown_session() noexcept {
        UA_Client_disconnect(client_);
        connected_ = false;
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
        return false;  // not a numeric-convertible scalar type -> caller skips this entry
    }

    std::string endpoint_;
    std::vector<std::string> node_id_strings_;  // config order; index-paired with node_ids_
    std::uint32_t rate_hz_ = 0;  // advisory only (see ModbusTcpDriver's own banner)

    bool opened_ = false;
    bool connected_ = false;
    UA_Client* client_ = nullptr;
    std::vector<UA_NodeId> node_ids_;  // parsed at open(), cleared at close()

    int backoff_ms_ = kInitialBackoffMs;
    std::chrono::steady_clock::time_point next_attempt_at_{};
};

}  // namespace aero::drivers

#else  // !AERO_OPCUA_ENABLED — identical call-site surface, honest "not compiled in" gate (mirrors
       // aero/pal/tls.hpp's stub pattern).

namespace aero::drivers {

class OpcUaDriver final : public IDriver {
public:
    OpcUaDriver(std::string /*endpoint*/, std::vector<std::string> /*node_ids*/) noexcept {}

    DriverStatus open(const DriverConfig& /*cfg*/) noexcept override { return DriverStatus::Error; }
    DriverStatus run(StreamSink<Frame> /*sink*/, StopToken /*stop*/) noexcept override {
        return DriverStatus::Unsupported;
    }
    DriverStatus poll(StreamSink<Frame> /*sink*/) noexcept override { return DriverStatus::Error; }
    void close() noexcept override {}
    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr DriverDescriptor kDesc{"aero.driver.opcua", /*writable*/ false};
};

}  // namespace aero::drivers

#endif  // AERO_OPCUA_ENABLED
