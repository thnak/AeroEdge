// AeroEdge M9b driver — OpcUaDriver: a real OPC-UA client PULL driver (018 §Multi-protocol southbound,
// 006 §6.1), over open62541 (github.com/open62541/open62541, MPL-2.0), the standard open-source C99
// OPC-UA SDK. Same posture as ModbusTcpDriver (drivers/modbus_tcp_driver.hpp, this milestone's worked
// reference): PULL, not PUSH — every read happens inside one `poll()` call, `run()` is a hard
// Unsupported. v1 SCOPE (explicit, not an oversight): connect to ONE configured endpoint, read a fixed
// set of configured NodeIds via UA_Client_readValueAttribute, write a scalar to any NodeId via
// UA_Client_writeValueAttribute (M9.1 PR E, 018 §8 — the OPC-UA counterpart to ModbusTcpDriver's FC06),
// call an object/method NodeId pair with exactly one scalar input argument via UA_Client_call (M9.1 PR
// F — the FC16-ish "structured target string" counterpart, output arguments discarded unread), OR browse
// ONE configured NodeId's children via UA_Client_Service_browse (M9.1 PR G, 018 §8 — a SEPARATE poll
// mode from the scalar-NodeId reads, see poll()/browse_once()'s banner). NO security policies
// (Sign/SignAndEncrypt is backlog), NO Subscriptions/MonitoredItems — both still backlog.
//
// REUSE, NOT REBUILD (the whole point): this driver's job stops at serializing the polled NodeId->value
// results into a FLAT JSON OBJECT (e.g. {"ns=2;s=Temperature":23.5}) written into the Frame's byte
// payload — `aero::nodes::JsonParseNode` (nodes/compute_nodes.hpp, "aero.source.json", UNMODIFIED)
// already decodes exactly that shape into Tags downstream. No new decode node needed.
//
// SECURITY POLICIES (M9.4, 018 §8 — the last v1 backlog item): Sign/SignAndEncrypt over a client
// certificate, i.e. the SecureChannel itself is mutually authenticated by X.509 certs rather than left at
// MessageSecurityMode::None (the only mode every OTHER PR in this file uses). `OpcUaSecurityConfig`
// (default-constructed = disabled, `certificate_file` empty) is an OPT-IN, additive fourth constructor
// argument — every existing call site/deploy config keeps working unchanged. v1 scope, deliberately
// narrow like every other slice in this file:
//   - ONE trusted peer certificate (`trusted_server_certificate_file`), not a CA chain/multi-entry trust
//     store — matches this driver's existing "ONE configured endpoint" posture. A real CA-backed
//     deployment is backlog, not an oversight.
//   - Sign or SignAndEncrypt only (both certificate-backed) — MessageSecurityMode::None stays the
//     zero-config default via the disabled path below; there's no "encrypt but don't authenticate" mode
//     to opt into, matching how OPC-UA's own SecureChannel security actually works (its cert IS the
//     channel's identity, not a separate user-auth layer — this is what "cert-based client auth" in the
//     018 backlog item means: the CLIENT authenticates itself to the server via ITS OWN certificate as
//     part of establishing the channel, not a session-level X.509 UserIdentityToken, which stays backlog).
//   - Certificate/key material is loaded from DER files on disk (`certificate_file`/`private_key_file`/
//     `trusted_server_certificate_file`), mirroring aero/pal/tls.hpp's own `cert_file`/`key_file`
//     PEM-file convention (mbedTLS's parser auto-detects PEM vs DER) and open62541's own
//     examples/encryption/client_encryption.c reference client — NOT inline bytes/base64 in deploy JSON,
//     which would put private key material in a config file that might get logged or committed.
// REQUIRES the vendored open62541 to be built with UA_ENABLE_ENCRYPTION=MBEDTLS (root CMakeLists.txt) —
// sharing THIS project's own already-vendored mbedTLS build (017 M5), not a second copy (see
// cmake/patch_open62541.cmake's banner for why linking two independent mbedTLS static libs into one
// binary is a real duplicate-symbol hazard, not just wasted size).
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

#include "aero/drivers/opcua_security.hpp"
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
    // `browse_root` (default empty = disabled) switches this instance into BROWSE mode (M9.1 PR G):
    // poll() browses that ONE NodeId's children instead of reading node_ids's scalars. The two modes are
    // mutually exclusive in v1 (open() rejects a config that sets both) — see poll()/browse_once().
    // `security` (default-constructed = disabled, see opcua_security.hpp) opts into Sign/SignAndEncrypt
    // over a client certificate (M9.4, 018 §8) — additive, every existing 3-arg call site keeps working
    // unchanged at MessageSecurityMode::None.
    OpcUaDriver(std::string endpoint, std::vector<std::string> node_ids, std::string browse_root = {},
                OpcUaSecurityConfig security = {}) noexcept
        : endpoint_(std::move(endpoint)),
          node_id_strings_(std::move(node_ids)),
          browse_root_str_(std::move(browse_root)),
          security_(std::move(security)) {}

    ~OpcUaDriver() override { close(); }

    // Config-time rejection (never a silently truncated/corrupted frame, per Part 1's
    // Frame::payload_len/payload budget) — see file banner for the ~40-bytes/entry worst-case estimate.
    // Parses every configured NodeId string here too (a malformed NodeId string is also a config error,
    // independent of device reachability) — dial LAST, so a bad `node_ids` entry never even attempts a
    // socket. Mirrors ModbusTcpDriver: `opened_` is armed before the dial attempt, so a reachability
    // failure here still leaves the driver armed for a later poll() to retry (see ensure_connected()).
    DriverStatus open(const DriverConfig& cfg) noexcept override {
        browse_mode_ = !browse_root_str_.empty();
        if (browse_mode_) {
            // v1: a browse-mode instance browses exactly one root, not a mix of both poll shapes —
            // catches a config error here rather than silently ignoring node_ids in poll().
            if (!node_id_strings_.empty()) return DriverStatus::Error;
            UA_NodeId_init(&browse_root_id_);
            if (!parse_node_id(browse_root_str_, browse_root_id_)) return DriverStatus::Error;
        } else {
            if (!fits_payload_budget()) return DriverStatus::Error;
            if (!parse_node_ids()) return DriverStatus::Error;
        }

        rate_hz_ = cfg.rate_hz;  // advisory poll-interval hint only (see ModbusTcpDriver's own banner)
        opened_ = true;

        client_ = UA_Client_new();
        if (!client_) return DriverStatus::Error;
        if (!apply_security_config(UA_Client_getConfig(client_), security_)) {
            UA_Client_delete(client_);
            client_ = nullptr;
            return DriverStatus::Error;
        }

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
        if (browse_mode_) return browse_once(std::move(sink));

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

    // BROWSE mode's whole poll cycle (M9.1 PR G, 018 §8) — a SEPARATE JSON SHAPE from the scalar path
    // above: emits a JSON ARRAY of {"id":..., "name":...} objects (one per child reference), not a
    // {nodeId: value} object, because a browse result isn't a value at all. `aero::nodes::JsonParseNode`
    // (the scalar path's downstream decoder) does NOT understand this shape — v1 ships the raw array in
    // the Frame payload for a caller/flow to consume directly (a dedicated browse-decode node is
    // backlog, matching PR F's own "reading call outputs" deferral).
    //
    // ONE browse call, ONE level, NO continuation points (018 §8 open questions): requestedMaxReferencesPerNode
    // is capped at kMaxBrowseResults so the SERVER already truncates server-side; if the true child count
    // is larger, the response's own (unread) continuationPoint would let a v2 fetch the rest via
    // UA_Client_Service_browseNext — out of scope here, same "smallest independent slice" posture as
    // every other M9.1 PR. The final body-size check is a bug guard (should be unreachable given the
    // requestedMaxReferencesPerNode cap) — mirrors poll()'s own guard on the scalar path.
    DriverStatus browse_once(StreamSink<Frame> sink) noexcept {
        UA_BrowseDescription bd;
        UA_BrowseDescription_init(&bd);
        bd.nodeId = browse_root_id_;  // borrowed (see close(): browse_root_id_ is cleared exactly once)
        bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
        // HierarchicalReferences only (Organizes/HasComponent/HasProperty/...), NOT every reference type:
        // an unrestricted browse (referenceTypeId left NULL) also returns e.g. HasTypeDefinition ->
        // BaseObjectType, which isn't a "child" in the sense a caller means by browsing — this is the
        // conventional "list an object's children" restriction most OPC-UA browsers apply by default.
        bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
        bd.includeSubtypes = true;
        bd.nodeClassMask = 0;  // every NodeClass
        bd.resultMask = UA_BROWSERESULTMASK_BROWSENAME;

        UA_BrowseRequest request;
        UA_BrowseRequest_init(&request);
        request.requestedMaxReferencesPerNode = kMaxBrowseResults;
        request.nodesToBrowseSize = 1;
        request.nodesToBrowse = &bd;  // stack-local; never UA_BrowseRequest_clear'd (would double-free bd.nodeId)

        UA_BrowseResponse response = UA_Client_Service_browse(client_, request);

        const bool service_ok = response.responseHeader.serviceResult == UA_STATUSCODE_GOOD;
        const bool result_ok =
            service_ok && response.resultsSize == 1 && response.results[0].statusCode == UA_STATUSCODE_GOOD;

        nlohmann::json j = nlohmann::json::array();
        if (result_ok) {
            const auto& result = response.results[0];
            for (std::size_t i = 0; i < result.referencesSize && j.size() < kMaxBrowseResults; ++i) {
                const auto& ref = result.references[i];
                UA_String printed = UA_STRING_NULL;
                if (UA_ExpandedNodeId_print(&ref.nodeId, &printed) != UA_STATUSCODE_GOOD) {
                    UA_String_clear(&printed);
                    continue;  // one malformed reference doesn't fail the whole browse
                }
                const std::string id_str(reinterpret_cast<const char*>(printed.data), printed.length);
                UA_String_clear(&printed);
                const std::string name_str(reinterpret_cast<const char*>(ref.browseName.name.data),
                                            ref.browseName.name.length);
                j.push_back({{"id", id_str}, {"name", name_str}});
            }
        }
        UA_BrowseResponse_clear(&response);

        if (!service_ok) {
            if (connection_lost()) {
                teardown_session();  // (006 §8) -> reconnect w/ backoff on a later poll()
                return DriverStatus::Error;
            }
            return DriverStatus::Error;  // well-formed device-level error, connection stays healthy
        }
        if (!result_ok) return DriverStatus::Error;  // e.g. bad root NodeId, connection stays healthy

        const std::string body = j.dump();
        // Bug guard, see this method's banner — the requestedMaxReferencesPerNode cap should already
        // keep this under budget.
        if (body.size() > aero::kMaxFramePayload) return DriverStatus::Error;

        Frame frame{};
        frame.payload_len = static_cast<std::uint16_t>(body.size());
        std::memcpy(frame.payload.data(), body.data(), body.size());

        while (!sink.try_push(frame)) {
            std::this_thread::yield();  // lossless backpressure (006 §3): stall, never drop
        }
        return DriverStatus::Ok;
    }

    // A device-directed write (006 §7), M9.1's write slice (018 §8). `DeviceCommand` has no dedicated
    // object/method field (006 §7, a shared SDK type also used by OTA — not widened for this), so both
    // forms live entirely in `cmd.target`, mirroring ModbusTcpDriver::write()'s comma-vs-bare split:
    //   - a bare NodeId string ("ns=1;s=Setpoint") -> UA_Client_writeValueAttribute (PR E), `cmd.value`
    //     written as a UA_Double, matching this driver's read-side convention of normalizing every value
    //     to double (variant_to_double()).
    //   - "objectNodeId|methodNodeId" (PR F) -> UA_Client_call with EXACTLY ONE scalar UA_Double input
    //     argument (`cmd.value`); output arguments are freed, unread (v1 — 0-arg/multi-arg calls and
    //     reading outputs are backlog, §8). OPC-UA NodeId identifier strings don't use "|" in this
    //     codebase's own encoding, so the two forms don't collide.
    // Both forms parse their NodeId(s) BEFORE touching the connection (a malformed target is a config
    // error, not a device/connection error, so it short-circuits without a reconnect attempt even when
    // disconnected), and share the same connection-loss detection/teardown posture as poll() (006 §8): a
    // genuine channel/session loss tears the session down for the next poll()/write() to reconnect; a
    // well-formed OPC-UA error over a healthy connection (bad NodeId, type mismatch, access denied,
    // wrong argument count) is a clean DriverStatus::Error, connection stays up.
    DriverStatus write(const DeviceCommand& cmd) noexcept override {
        if (!opened_) return DriverStatus::Error;

        const auto sep = cmd.target.find('|');
        if (sep != std::string_view::npos) {
            return call_method(cmd.target.substr(0, sep), cmd.target.substr(sep + 1), cmd.value);
        }

        UA_NodeId id;
        UA_NodeId_init(&id);
        if (!parse_node_id(cmd.target, id)) {
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
        if (browse_mode_) UA_NodeId_clear(&browse_root_id_);
        if (client_) {
            if (connected_) UA_Client_disconnect(client_);
            UA_Client_delete(client_);
            client_ = nullptr;
        }
        connected_ = false;
        opened_ = false;
    }

    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr DriverDescriptor kDesc{"aero.driver.opcua", /*writable*/ true, /*poll_driven*/ true};

private:
    // Connect-attempt timing is entirely open62541's own (UA_Client_connect blocks internally per its own
    // ClientConfig timeout, no separate dial_tcp()-style timeout knob needed here — unlike ModbusTcpDriver
    // over a raw socket).
    static constexpr int kInitialBackoffMs = 200;
    static constexpr int kMaxBackoffMs = 5000;
    // Conservative worst-case per-entry serialized size (quoted NodeId string + colon + a long-ish
    // double + comma), see file banner. 2 accounts for the object's own "{" "}".
    static constexpr std::size_t kBytesPerEntryEstimate = 40;
    // BROWSE mode (M9.1 PR G): capped small enough that even long BrowseNames stay under
    // kMaxFramePayload (see browse_once()'s banner) — also passed to the server as
    // requestedMaxReferencesPerNode, so this is enforced server-side too, not just client-side truncation.
    static constexpr std::uint32_t kMaxBrowseResults = 3;

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

    // Shared NodeId-string parse (UA_NodeId_parse) — write()'s bare-NodeId form and call_method()'s
    // object/method pair both go through this. `out` must already be UA_NodeId_init'd by the caller;
    // on failure `out` is left as its init'd (empty) value, never partially populated.
    static bool parse_node_id(std::string_view s, UA_NodeId& out) noexcept {
        UA_String ua_s = UA_String_fromChars(std::string(s).c_str());
        const UA_StatusCode rc = UA_NodeId_parse(&out, ua_s);
        UA_String_clear(&ua_s);
        return rc == UA_STATUSCODE_GOOD;
    }

    // "objectNodeId|methodNodeId" method call (M9.1 PR F, 018 §8) — see write()'s banner for the v1
    // scope (exactly one scalar UA_Double input argument, output arguments discarded unread).
    DriverStatus call_method(std::string_view object_str, std::string_view method_str,
                              std::int64_t value) noexcept {
        UA_NodeId object_id, method_id;
        UA_NodeId_init(&object_id);
        UA_NodeId_init(&method_id);
        if (!parse_node_id(object_str, object_id) || !parse_node_id(method_str, method_id)) {
            UA_NodeId_clear(&object_id);
            UA_NodeId_clear(&method_id);
            return DriverStatus::Error;
        }

        if (!connected_ && !ensure_connected()) {
            UA_NodeId_clear(&object_id);
            UA_NodeId_clear(&method_id);
            return DriverStatus::Error;
        }

        UA_Variant input;
        UA_Variant_init(&input);
        UA_Double v = static_cast<UA_Double>(value);
        const UA_StatusCode variant_rc = UA_Variant_setScalarCopy(&input, &v, &UA_TYPES[UA_TYPES_DOUBLE]);
        if (variant_rc != UA_STATUSCODE_GOOD) {
            UA_NodeId_clear(&object_id);
            UA_NodeId_clear(&method_id);
            return DriverStatus::Error;
        }

        std::size_t output_size = 0;
        UA_Variant* output = nullptr;
        const UA_StatusCode rc =
            UA_Client_call(client_, object_id, method_id, 1, &input, &output_size, &output);

        UA_Variant_clear(&input);
        UA_NodeId_clear(&object_id);
        UA_NodeId_clear(&method_id);
        if (output != nullptr) UA_Array_delete(output, output_size, &UA_TYPES[UA_TYPES_VARIANT]);

        if (rc != UA_STATUSCODE_GOOD) {
            if (connection_lost()) {
                teardown_session();  // (006 §8) -> reconnect w/ backoff on a later poll()/write()
                return DriverStatus::Error;
            }
            return DriverStatus::Error;  // well-formed device-level error, connection stays healthy
        }
        return DriverStatus::Ok;
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

    std::string browse_root_str_;   // BROWSE mode (M9.1 PR G): empty == disabled (scalar poll instead)
    bool browse_mode_ = false;      // set from browse_root_str_ in open(), not the constructor
    UA_NodeId browse_root_id_{};    // parsed at open(), cleared at close() — see close()'s guard
    OpcUaSecurityConfig security_;  // M9.4: empty == disabled (MessageSecurityMode::None)

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
    OpcUaDriver(std::string /*endpoint*/, std::vector<std::string> /*node_ids*/,
                std::string /*browse_root*/ = {}, OpcUaSecurityConfig /*security*/ = {}) noexcept {}

    DriverStatus open(const DriverConfig& /*cfg*/) noexcept override { return DriverStatus::Error; }
    DriverStatus run(StreamSink<Frame> /*sink*/, StopToken /*stop*/) noexcept override {
        return DriverStatus::Unsupported;
    }
    DriverStatus poll(StreamSink<Frame> /*sink*/) noexcept override { return DriverStatus::Error; }
    void close() noexcept override {}
    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr DriverDescriptor kDesc{"aero.driver.opcua", /*writable*/ false, /*poll_driven*/ true};
};

}  // namespace aero::drivers

#endif  // AERO_OPCUA_ENABLED
