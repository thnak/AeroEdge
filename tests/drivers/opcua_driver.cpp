// AeroEdge M9b gate (018 §Multi-protocol southbound) — OpcUaDriver (drivers/opcua_driver.hpp) over a
// REAL open62541 UA_Server hosted in-process on a background thread (the standard way open62541's own
// test suite works — no external test infra needed). Kept a focused unit/integration test of the driver
// + JsonParseNode pair (NOT the full Runtime), mirroring tests/drivers/modbus_tcp_driver.cpp exactly:
// frames are drained straight off a StreamActivation<Frame> ring into a bare ProcessingContext, never
// through a FlowActor/Runtime deploy.
//
// FIXED TEST PORT (not ephemeral): unlike the Modbus test's fake server (a hand-rolled TCP listener that
// can bind port 0 and read back the OS-assigned port via quark::pal::local_port), open62541's server
// config's "port 0 -> dynamically assigned" path resolves the real port down inside its EventLoop/POSIX
// network-layer plugin at run_startup time, with no straightforward public-API way to read it back before
// the driver needs the endpoint URL. A fixed, uncommon port is used instead — acceptable for a single,
// serially-run ctest binary.
//
// Covers:
//   (1) happy path: driver.poll() gets back known variable-node values over a real OPC-UA session;
//       JsonParseNode (nodes/compute_nodes.hpp, reused verbatim — decode is explicitly NOT this driver's
//       job) decodes ctx.payload into tags that match the canned values.
//   (2) a node_ids list large enough to blow the 128-byte payload budget is rejected at open() — the
//       server doesn't even need to be reachable (config-time-only rejection, mirrors the Modbus test).
//   (3) connection loss mid-session: the fake server is stopped entirely — the driver's NEXT poll()
//       against the now-dead endpoint fails cleanly (Error, no crash/hang), and a LATER poll() (after the
//       driver's bounded backoff, once the server is brought back up on the same port) reconnects and
//       succeeds.
//   (4) M9.1 PR G: a BROWSE-mode driver (constructed with a `browse_root`) lists a known node's children
//       as a JSON array, and rejects a malformed root / a config that sets both node_ids and browse_root.
// Deterministic-enough, exit-code-gated (0 = pass); bounded polling/retries; clean shutdown.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory_resource>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <open62541/plugin/log_stdout.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include "aero/drivers/opcua_driver.hpp"
#include "aero/nodes/compute_nodes.hpp"
#include "aero/sdk/processing_context.hpp"
#include "nlohmann/json.hpp"
#include "quark/core/stream_activation.hpp"

using aero::Frame;
using aero::drivers::OpcUaDriver;

namespace {

constexpr std::uint16_t kTestPort = 48711;  // see file banner: fixed, not ephemeral
constexpr const char* kEndpoint = "opc.tcp://127.0.0.1:48711";

// ===== a real open62541 UA_Server, hosted on a background thread ======================================
// NOT UA_Server_run(server, &running) anymore — see the TSan root-cause notes below. Manually sequenced
// via open62541's own decomposed start/iterate/shutdown triplet instead (server.h: "The prologue part of
// UA_Server_run" / "single iteration of the main loop" / "The epilogue part of UA_Server_run" — this is
// the SAME public API UA_Server_run itself is built on, not a workaround hack).
//
// TSAN ROOT CAUSE (CI's g++-14 -fsanitize=thread leg, test_reconnect_after_loss): this fixture runs a
// real UA_Server (background thread) concurrently with a real UA_Client (main thread, OpcUaDriver) over
// an actual TCP loopback connection — a genuinely multi-threaded open62541 workload. Reproduced locally
// (g++-14, Debug, -fsanitize=thread, matching CI's exact matrix leg) and root-caused to TWO independent,
// real races, neither of them in OUR driver code:
//
//  1) open62541's own arch/common/ua_timer.c has FILE-SCOPE globals `earliest`/`latest`/`adjustedNextTime`
//     used as scratch inside `addCallback()`, commented there as "only used behind the mutex" — but that
//     mutex is PER-UA_Timer-INSTANCE (each UA_Client and each UA_Server owns its own UA_Timer/mutex), so
//     the comment's invariant silently breaks the moment a Client's timer and a Server's timer both call
//     addCallback() concurrently: e.g. the client's one-time housekeeping-callback registration (inside
//     UA_Client_connect, __UA_Client_startup) racing the server's own startup (UA_Server_run_startup
//     registering its protocol managers' cyclic callbacks) — exactly what used to happen here, since the
//     old code let a freshly spawned background thread run UA_Server_run_startup() WHILE the main thread
//     was free to race ahead into UA_Client_connect() with nothing but a 200ms sleep between them.
//     FIX: this fixture now calls UA_Server_run_startup() SYNCHRONOUSLY on the calling (main) thread,
//     inside start(), before the background thread is even spawned — the same thread that later calls
//     OpcUaDriver::open()/poll() (which drives the client's own one-time startup registration). That
//     makes the two addCallback() call sites strictly SEQUENCED in program order on one thread, a real
//     happens-before edge, not a probabilistic one. Bonus: UA_Server_run_startup() is also where the
//     listening socket actually gets bound (see TCP_registerListenSockets in the TSan stack), so start()
//     returning now means "the socket is bound", a real guarantee — the old 200ms "hope it's bound by
//     now" sleep is gone, not just lengthened.
//  2) open62541's default stdout logger (plugins/ua_log_stdout.c, UA_Log_Stdout_log — installed by both
//     UA_ServerConfig_setMinimal and the driver's UA_ClientConfig_setDefault when no logger is configured)
//     calls UA_DateTime_localTimeUtcOffset() (arch/posix/ua_clock.c) to timestamp EVERY log line, which
//     calls libc's mktime(). Confirmed by installing libc6-dbg and re-running under TSan: the race is
//     inside glibc's own timezone-abbreviation interning (a malloc'd string looked up in/inserted into a
//     process-global cache, unguarded on this call path — tzset() itself takes a lock, but mktime()'s
//     internal, implicit call into the same machinery does not) — this is why the original CI report's
//     SUMMARY line names a bare, unsymbolized "libc.so.6+0x..." offset instead of a function. NOTE: an
//     earlier attempt at this fix called tzset() once up front, on the theory that it was a one-time lazy
//     cache population race — verified (10x repeated TSan runs) that this did NOT eliminate the race, so
//     it is not that; the racy interning happens on every call, not just the first. open62541 logs from
//     BOTH the server thread and the main thread throughout the fixture's whole lifetime (not just at
//     startup), so this can fire whenever their log lines happen to land concurrently — a genuine
//     inherent hazard of running two independent open62541 EventLoops (each with its own default logger)
//     concurrently in one process, not something a synchronization point at one moment in time can bound.
//     FIX: UA_Log_Stdout_log itself checks its configured minLevel and returns BEFORE computing the
//     timestamp (`if(minLevel > level) return;`, see plugins/ua_log_stdout.c) — i.e. the timestamp/mktime
//     call is conditional on the message actually being emitted, not unconditional. This fixture's own
//     server is given a logger built via UA_Log_Stdout_withLevel/UA_Log_Stdout_new(UA_LOGLEVEL_FATAL)
//     (below, in start()) instead of the setMinimal-installed default (UA_LOGLEVEL_INFO) — the server
//     thread then never reaches the racy mktime() call at all (this test doesn't assert on open62541's
//     own log output; only driver.poll()'s return status and the frames it produces matter). With only
//     the single-threaded main thread left calling mktime() (via the client's own default INFO logger,
//     inside include/aero/drivers/opcua_driver.hpp, untouched — a lone thread can't race itself), the
//     race has no second concurrent caller left and cannot occur. Verified: 10x repeated direct TSan runs
//     of this binary, zero races (down from 10/10 raced before this fix — see report for numbers).
//
// Neither is a bug in aero::drivers::OpcUaDriver (include/aero/drivers/opcua_driver.hpp is untouched) —
// both are pre-existing thread-safety gaps in vendored open62541's own plugins, only reachable because
// this fixture is (deliberately, per the file banner above) a genuine two-thread open62541 workload.
// Fixed at the fixture/harness level, matching this project's precedent for third-party thread-safety
// gaps (M5's mbedTLS PSA race: a real MBEDTLS_THREADING_ALT shim, not a sleep or a suppression) — #1 is a
// real happens-before edge (no sleep involved at all), #2 configures the ACTUAL open62541 knob
// (UA_Logger/minLevel) that gates the racy call, not a workaround bolted on from outside it.
struct FakeOpcUaServer {
    UA_Server* server = nullptr;
    std::atomic<bool> running{false};
    std::thread thr;
    std::atomic<bool> run_failed{false};

    // Retries the bind (bounded) since a just-stopped server on the same port may still hold the socket
    // in TIME_WAIT for a moment — mirrors this test's own bounded-retry philosophy elsewhere.
    bool start() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        UA_ServerConfig config;
        std::memset(&config, 0, sizeof(config));
        // See the struct banner above (root cause #2): pre-install a FATAL-only logger BEFORE
        // UA_ServerConfig_setMinimal runs, so its "if(conf->logging == NULL) conf->logging = ..." default
        // -installation check (plugins/ua_config_default.c) sees this already set and leaves it alone,
        // instead of installing its own UA_LOGLEVEL_INFO stdout logger. This server thread then never
        // reaches UA_Log_Stdout_log's racy mktime() call — the level check happens first and returns.
        config.logging = UA_Log_Stdout_new(UA_LOGLEVEL_FATAL);
        while (std::chrono::steady_clock::now() < deadline) {
            if (UA_ServerConfig_setMinimal(&config, kTestPort, nullptr) == UA_STATUSCODE_GOOD) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        server = UA_Server_newWithConfig(&config);
        if (!server) return false;

        add_variable(server, "Temperature", 23.5, UA_ACCESSLEVELMASK_READ);
        add_variable(server, "Pressure", 101.3, UA_ACCESSLEVELMASK_READ);
        add_variable(server, "Setpoint", 0.0,
                      UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE);  // M9.1 PR E write test target
        add_increment_method(server);  // M9.1 PR F call_method() test target
        add_browse_root(server);       // M9.1 PR G browse() test target: "ns=1;s=BrowseRoot" -> 2 children

        // Synchronous on THIS (the caller's) thread — see the struct banner above for why: this is the
        // real happens-before edge against the driver's own UA_Client startup (also called from the main
        // thread, always AFTER start() returns), and it's also where the listening socket actually gets
        // bound — no heuristic sleep needed afterward to "probably" have a bound socket.
        if (UA_Server_run_startup(server) != UA_STATUSCODE_GOOD) {
            UA_Server_delete(server);
            server = nullptr;
            return false;
        }

        run_failed.store(false, std::memory_order_release);
        running.store(true, std::memory_order_release);
        thr = std::thread([this] {
            while (running.load(std::memory_order_acquire)) {
                UA_Server_run_iterate(server, true);
            }
            if (UA_Server_run_shutdown(server) != UA_STATUSCODE_GOOD) {
                run_failed.store(true, std::memory_order_release);
            }
        });
        return true;
    }

    void stop() {
        if (!server) return;
        running.store(false, std::memory_order_release);
        if (thr.joinable()) thr.join();
        UA_Server_delete(server);
        server = nullptr;
    }

    static void add_variable(UA_Server* srv, const char* name, UA_Double value, UA_Byte access_level,
                              UA_NodeId parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER)) {
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
        attr.description = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(name));
        attr.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(name));
        attr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
        attr.accessLevel = access_level;

        const UA_NodeId node_id = UA_NODEID_STRING(1, const_cast<char*>(name));
        const UA_QualifiedName browse_name = UA_QUALIFIEDNAME(1, const_cast<char*>(name));
        const UA_NodeId parent_ref = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
        const UA_NodeId type_def = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
        UA_Server_addVariableNode(srv, node_id, parent, parent_ref, browse_name, type_def, attr, nullptr,
                                   nullptr);
    }

    // "BrowseRoot": an Object node (ns=1;s=BrowseRoot) with exactly two known children ("ChildA",
    // "ChildB") — test_browse()'s deterministic target (kMaxBrowseResults=3 comfortably covers 2).
    static void add_browse_root(UA_Server* srv) {
        UA_ObjectAttributes attr = UA_ObjectAttributes_default;
        attr.description = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>("BrowseRoot"));
        attr.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>("BrowseRoot"));

        const UA_NodeId node_id = UA_NODEID_STRING(1, const_cast<char*>("BrowseRoot"));
        const UA_QualifiedName browse_name = UA_QUALIFIEDNAME(1, const_cast<char*>("BrowseRoot"));
        const UA_NodeId parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        const UA_NodeId parent_ref = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
        const UA_NodeId type_def = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE);
        UA_Server_addObjectNode(srv, node_id, parent, parent_ref, browse_name, type_def, attr, nullptr,
                                 nullptr);

        add_variable(srv, "ChildA", 1.0, UA_ACCESSLEVELMASK_READ,
                     UA_NODEID_STRING(1, const_cast<char*>("BrowseRoot")));
        add_variable(srv, "ChildB", 2.0, UA_ACCESSLEVELMASK_READ,
                     UA_NODEID_STRING(1, const_cast<char*>("BrowseRoot")));
    }

    // "Increment": one scalar double in, one scalar double out (out = in + 1) — a minimal callable
    // target for test_call_method()'s M9.1 PR F coverage. Attached directly to the ObjectsFolder
    // (i=85, ns=0) via HasComponent, the standard shape UA_Client_call expects (objectId must have the
    // method as a HasComponent child, not just any arbitrary NodeId pair).
    static UA_StatusCode increment_callback(UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void*,
                                             const UA_NodeId*, void*, std::size_t input_size,
                                             const UA_Variant* input, std::size_t output_size,
                                             UA_Variant* output) {
        if (input_size != 1 || output_size != 1) return UA_STATUSCODE_BADARGUMENTSMISSING;
        if (!UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_DOUBLE])) {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        const UA_Double in = *static_cast<const UA_Double*>(input[0].data);
        UA_Double out = in + 1.0;
        return UA_Variant_setScalarCopy(&output[0], &out, &UA_TYPES[UA_TYPES_DOUBLE]);
    }

    static void add_increment_method(UA_Server* srv) {
        UA_Argument input_arg;
        UA_Argument_init(&input_arg);
        input_arg.description = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>("value"));
        input_arg.name = UA_STRING(const_cast<char*>("value"));
        input_arg.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
        input_arg.valueRank = UA_VALUERANK_SCALAR;

        UA_Argument output_arg;
        UA_Argument_init(&output_arg);
        output_arg.description = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>("result"));
        output_arg.name = UA_STRING(const_cast<char*>("result"));
        output_arg.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
        output_arg.valueRank = UA_VALUERANK_SCALAR;

        UA_MethodAttributes attr = UA_MethodAttributes_default;
        attr.description = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>("Increment"));
        attr.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>("Increment"));
        attr.executable = true;
        attr.userExecutable = true;

        const UA_NodeId method_id = UA_NODEID_STRING(1, const_cast<char*>("Increment"));
        const UA_QualifiedName browse_name = UA_QUALIFIEDNAME(1, const_cast<char*>("Increment"));
        const UA_NodeId parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        const UA_NodeId parent_ref = UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT);
        UA_Server_addMethodNode(srv, method_id, parent, parent_ref, browse_name, attr, increment_callback,
                                 1, &input_arg, 1, &output_arg, nullptr, nullptr);
    }
};

// Drain exactly one Frame off a stream ring (bounded wait) — same helper as modbus_tcp_driver.cpp.
std::optional<Frame> drain_one(quark::StreamChannel<Frame>& ch, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ch.occupancy() > 0) {
            quark::StreamBatch<Frame> batch(ch, /*budget*/ 1);
            if (const Frame* f = batch.next()) {
                Frame copy = *f;
                batch.retire();
                return copy;
            }
        }
        std::this_thread::yield();
    }
    return std::nullopt;
}

struct PollOutcome {
    aero::DriverStatus status = aero::DriverStatus::Error;
    std::optional<Frame> frame;
};

// One poll() call against a FRESH, throwaway StreamActivation — see modbus_tcp_driver.cpp's file banner
// for why one-per-call (a StreamActivation's single-producer bind is a lifetime commitment; poll()'s
// StreamSink-by-value contract has no way to hand a token back for reuse across calls).
PollOutcome poll_once(OpcUaDriver& driver) {
    quark::StreamActivation<Frame>::Config scfg;
    scfg.capacity = 4;
    std::pmr::monotonic_buffer_resource mr;
    quark::StreamActivation<Frame> act(scfg, &mr);
    auto tok = quark::open_stream(act);
    if (!tok) return {};
    aero::StreamSink<Frame> sink(std::move(tok.value()));

    PollOutcome out;
    out.status = driver.poll(std::move(sink));
    if (out.status == aero::DriverStatus::Ok) out.frame = drain_one(act.channel());
    return out;
}

// open() can race the server's background startup thread (no public "ready" signal) — bounded retry.
bool open_with_retry(OpcUaDriver& driver, const aero::DriverConfig& cfg, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (driver.open(cfg) == aero::DriverStatus::Ok) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ---- (1) happy path: poll() -> JsonParseNode -> decoded tags match the canned values -----------------
bool test_happy_path() {
    FakeOpcUaServer server;
    if (!server.start()) { std::printf("happy_path: server start failed\n"); return false; }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{"ns=1;s=Temperature", "ns=1;s=Pressure"});
    aero::DriverConfig cfg{};
    bool ok = open_with_retry(driver, cfg);
    if (!ok) std::printf("happy_path: open() never succeeded\n");

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Ok;
    ok &= outcome.frame.has_value();
    if (outcome.frame) {
        aero::ProcessingContext ctx;
        ctx.reset(&*outcome.frame);
        aero::nodes::JsonParseNode decode;
        ok &= decode.process(ctx) == aero::NodeResult::Continue;
        ok &= ctx.tags.size() == 2;

        double temperature = 0.0, pressure = 0.0;
        bool saw_temp = false, saw_pressure = false;
        for (const auto& t : ctx.tags) {
            if (t.name == "ns=1;s=Temperature") { temperature = t.value; saw_temp = true; }
            if (t.name == "ns=1;s=Pressure") { pressure = t.value; saw_pressure = true; }
        }
        ok &= saw_temp && saw_pressure;
        ok &= temperature == 23.5;
        ok &= pressure == 101.3;
    }

    driver.close();
    server.stop();
    if (!ok) std::printf("happy_path: assertion failed\n");
    return ok;
}

// ---- (1b) M9.1 PR E: write() a scalar to the server's writable "Setpoint" node, read it back -----------
bool test_write_scalar() {
    FakeOpcUaServer server;
    if (!server.start()) { std::printf("write_scalar: server start failed\n"); return false; }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{"ns=1;s=Setpoint"});
    aero::DriverConfig cfg{};
    bool ok = open_with_retry(driver, cfg);
    if (!ok) std::printf("write_scalar: open() never succeeded\n");

    ok &= driver.write(aero::DeviceCommand{"ns=1;s=Setpoint", 42}) == aero::DriverStatus::Ok;

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Ok;
    ok &= outcome.frame.has_value();
    if (outcome.frame) {
        aero::ProcessingContext ctx;
        ctx.reset(&*outcome.frame);
        aero::nodes::JsonParseNode decode;
        ok &= decode.process(ctx) == aero::NodeResult::Continue;
        ok &= ctx.tags.size() == 1 && ctx.tags[0].name == "ns=1;s=Setpoint" && ctx.tags[0].value == 42.0;
    }

    driver.close();
    server.stop();
    if (!ok) std::printf("write_scalar: assertion failed\n");
    return ok;
}

// ---- (1c) M9.1 PR E: writing to a read-only node is a clean Error, connection stays healthy ------------
bool test_write_read_only_rejected() {
    FakeOpcUaServer server;
    if (!server.start()) { std::printf("write_read_only: server start failed\n"); return false; }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{"ns=1;s=Temperature"});
    aero::DriverConfig cfg{};
    bool ok = open_with_retry(driver, cfg);
    if (!ok) std::printf("write_read_only: open() never succeeded\n");

    ok &= driver.write(aero::DeviceCommand{"ns=1;s=Temperature", 99}) == aero::DriverStatus::Error;

    // Connection should have stayed healthy — a normal poll() right after still succeeds.
    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Ok;

    driver.close();
    server.stop();
    if (!ok) std::printf("write_read_only: assertion failed\n");
    return ok;
}

// ---- (1d) M9.1 PR E: a malformed NodeId target is rejected without any I/O -----------------------------
bool test_write_invalid_target() {
    // Port 1 is expected to refuse the connect, so open() itself returns Error here — irrelevant to this
    // test, same posture as ModbusTcpDriver's write_invalid_target test: open() sets `opened_` true
    // before it ever dials, and write()'s target parse runs before it touches the connection at all.
    OpcUaDriver driver("opc.tcp://127.0.0.1:1", std::vector<std::string>{});
    aero::DriverConfig cfg{};
    (void)driver.open(cfg);

    const bool ok = driver.write(aero::DeviceCommand{"not-a-valid-nodeid", 1}) == aero::DriverStatus::Error;
    driver.close();
    if (!ok) std::printf("write_invalid_target: assertion failed\n");
    return ok;
}

// ---- (1e) M9.1 PR F: "object|method" calls the fake server's Increment method (out = in + 1, v1
// discards the output — only DriverStatus::Ok proves the call round-tripped with the right arg count
// and type) --------------------------------------------------------------------------------------------
bool test_call_method() {
    FakeOpcUaServer server;
    if (!server.start()) { std::printf("call_method: server start failed\n"); return false; }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{});
    aero::DriverConfig cfg{};
    bool ok = open_with_retry(driver, cfg);
    if (!ok) std::printf("call_method: open() never succeeded\n");

    // object = ObjectsFolder (i=85, ns=0), method = ns=1;s=Increment.
    ok &= driver.write(aero::DeviceCommand{"i=85|ns=1;s=Increment", 41}) == aero::DriverStatus::Ok;

    driver.close();
    server.stop();
    if (!ok) std::printf("call_method: assertion failed\n");
    return ok;
}

// ---- (1f) M9.1 PR F: an unknown method NodeId is a clean Error, connection stays healthy ---------------
bool test_call_method_unknown_rejected() {
    FakeOpcUaServer server;
    if (!server.start()) { std::printf("call_method_unknown: server start failed\n"); return false; }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{});
    aero::DriverConfig cfg{};
    bool ok = open_with_retry(driver, cfg);

    ok &= driver.write(aero::DeviceCommand{"i=85|ns=1;s=Nonexistent", 1}) == aero::DriverStatus::Error;

    // Connection should have stayed healthy — a normal poll() right after still succeeds.
    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Ok;

    driver.close();
    server.stop();
    if (!ok) std::printf("call_method_unknown: assertion failed\n");
    return ok;
}

// ---- (1g) M9.1 PR F: a malformed method half of "object|method" is rejected without any I/O -----------
bool test_call_method_invalid_target() {
    // Same posture as test_write_invalid_target: open() sets `opened_` true before it ever dials against
    // the refused port 1, and both NodeId halves must parse cleanly before either is touched.
    OpcUaDriver driver("opc.tcp://127.0.0.1:1", std::vector<std::string>{});
    aero::DriverConfig cfg{};
    (void)driver.open(cfg);

    const bool ok =
        driver.write(aero::DeviceCommand{"i=85|not-a-valid-nodeid", 1}) == aero::DriverStatus::Error;
    driver.close();
    if (!ok) std::printf("call_method_invalid_target: assertion failed\n");
    return ok;
}

// ---- (1h) M9.1 PR G: browse-mode driver lists a known node's children as a JSON array -------------------
bool test_browse() {
    FakeOpcUaServer server;
    if (!server.start()) { std::printf("browse: server start failed\n"); return false; }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{}, "ns=1;s=BrowseRoot");
    aero::DriverConfig cfg{};
    bool ok = open_with_retry(driver, cfg);
    if (!ok) std::printf("browse: open() never succeeded\n");

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Ok;
    ok &= outcome.frame.has_value();
    if (outcome.frame) {
        const std::string body(reinterpret_cast<const char*>(outcome.frame->payload.data()),
                                outcome.frame->payload_len);
        const auto arr = nlohmann::json::parse(body, nullptr, /*allow_exceptions*/ false);
        ok &= !arr.is_discarded() && arr.is_array() && arr.size() == 2;
        bool saw_a = false, saw_b = false;
        if (arr.is_array()) {
            for (const auto& e : arr) {
                const auto name = e.value("name", std::string{});
                const auto id = e.value("id", std::string{});
                if (name == "ChildA") saw_a = !id.empty();
                if (name == "ChildB") saw_b = !id.empty();
            }
        }
        ok &= saw_a && saw_b;
    }

    driver.close();
    server.stop();
    if (!ok) std::printf("browse: assertion failed\n");
    return ok;
}

// ---- (1i) M9.1 PR G: a malformed browse_root NodeId is rejected at open(), no I/O -----------------------
bool test_browse_invalid_root() {
    OpcUaDriver driver("opc.tcp://127.0.0.1:1", std::vector<std::string>{}, "not-a-valid-nodeid");
    aero::DriverConfig cfg{};
    const bool ok = driver.open(cfg) == aero::DriverStatus::Error;
    driver.close();
    if (!ok) std::printf("browse_invalid_root: assertion failed\n");
    return ok;
}

// ---- (1j) M9.1 PR G: configuring both node_ids and browse_root together is rejected at open() -----------
bool test_browse_and_node_ids_rejected() {
    OpcUaDriver driver("opc.tcp://127.0.0.1:1", std::vector<std::string>{"ns=1;s=Temperature"},
                        "ns=1;s=BrowseRoot");
    aero::DriverConfig cfg{};
    const bool ok = driver.open(cfg) == aero::DriverStatus::Error;
    driver.close();
    if (!ok) std::printf("browse_and_node_ids_rejected: assertion failed\n");
    return ok;
}

// ---- (2) a node_ids list too large for the 128-byte payload budget is rejected at open() --------------
bool test_oversized_rejected() {
    // ~40 bytes/entry worst-case estimate (opcua_driver.hpp) -> 4 entries already exceeds 128 bytes.
    // The server doesn't even need to be reachable: open() must reject on the config check alone.
    std::vector<std::string> node_ids = {"ns=1;s=A", "ns=1;s=B", "ns=1;s=C", "ns=1;s=D"};
    OpcUaDriver driver("opc.tcp://127.0.0.1:1", std::move(node_ids));
    aero::DriverConfig cfg{};
    const bool ok = driver.open(cfg) == aero::DriverStatus::Error;
    if (!ok) std::printf("oversized_rejected: open() did not reject an oversized node_ids list\n");
    return ok;
}

// ---- (3) connection loss mid-session: the next poll() reconnects, never wedges/crashes ----------------
bool test_reconnect_after_loss() {
    FakeOpcUaServer server;
    if (!server.start()) { std::printf("reconnect: server start failed\n"); return false; }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{"ns=1;s=Temperature"});
    aero::DriverConfig cfg{};
    bool ok = open_with_retry(driver, cfg);
    if (!ok) std::printf("reconnect: open() never succeeded\n");

    // First poll: server is up -> Ok.
    const auto first = poll_once(driver);
    ok &= first.status == aero::DriverStatus::Ok && first.frame.has_value();

    // Stop the server entirely (closes the listening socket + any live session/channel).
    server.stop();

    // Next poll: the now-dead endpoint -> must fail CLEANLY (Error), never crash/hang. This is the
    // "connection lost" detection; the driver tears its own session down and schedules a backoff retry.
    const auto lost = poll_once(driver);
    ok &= lost.status != aero::DriverStatus::Ok;

    // Bring the server back up on the SAME port/endpoint.
    ok &= server.start();

    // Bounded retry loop: eventually (after the driver's backoff elapses) poll() reconnects and succeeds.
    bool reconnected = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline && !reconnected) {
        const auto r = poll_once(driver);
        if (r.status == aero::DriverStatus::Ok && r.frame.has_value()) {
            reconnected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ok &= reconnected;
    if (!reconnected) std::printf("reconnect: driver never reconnected within the bound\n");

    driver.close();
    server.stop();
    if (!ok) std::printf("reconnect: assertion failed\n");
    return ok;
}

}  // namespace

int main() {
    // See FakeOpcUaServer's banner (root cause #2) for the full story: open62541's own logging calls
    // libc's mktime() on every log line from BOTH the client and server threads, and glibc's lazy,
    // one-time timezone-database population on first use is not safe to race between threads. Force that
    // one-time population to happen here, single-threaded, before any server/client thread exists —
    // every later concurrent mktime() call is then just a read of an already-populated cache.
    tzset();

    bool ok = true;

    const bool happy_ok = test_happy_path();
    ok &= happy_ok;
    std::printf("[happy_path] %s\n", happy_ok ? "ok" : "FAIL");

    const bool write_ok = test_write_scalar();
    ok &= write_ok;
    std::printf("[write_scalar] %s\n", write_ok ? "ok" : "FAIL");

    const bool write_ro_ok = test_write_read_only_rejected();
    ok &= write_ro_ok;
    std::printf("[write_read_only_rejected] %s\n", write_ro_ok ? "ok" : "FAIL");

    const bool write_bad_target_ok = test_write_invalid_target();
    ok &= write_bad_target_ok;
    std::printf("[write_invalid_target] %s\n", write_bad_target_ok ? "ok" : "FAIL");

    const bool call_method_ok = test_call_method();
    ok &= call_method_ok;
    std::printf("[call_method] %s\n", call_method_ok ? "ok" : "FAIL");

    const bool call_method_unknown_ok = test_call_method_unknown_rejected();
    ok &= call_method_unknown_ok;
    std::printf("[call_method_unknown_rejected] %s\n", call_method_unknown_ok ? "ok" : "FAIL");

    const bool call_method_bad_target_ok = test_call_method_invalid_target();
    ok &= call_method_bad_target_ok;
    std::printf("[call_method_invalid_target] %s\n", call_method_bad_target_ok ? "ok" : "FAIL");

    const bool browse_ok = test_browse();
    ok &= browse_ok;
    std::printf("[browse] %s\n", browse_ok ? "ok" : "FAIL");

    const bool browse_invalid_root_ok = test_browse_invalid_root();
    ok &= browse_invalid_root_ok;
    std::printf("[browse_invalid_root] %s\n", browse_invalid_root_ok ? "ok" : "FAIL");

    const bool browse_and_node_ids_ok = test_browse_and_node_ids_rejected();
    ok &= browse_and_node_ids_ok;
    std::printf("[browse_and_node_ids_rejected] %s\n", browse_and_node_ids_ok ? "ok" : "FAIL");

    const bool oversized_ok = test_oversized_rejected();
    ok &= oversized_ok;
    std::printf("[oversized_rejected] %s\n", oversized_ok ? "ok" : "FAIL");

    const bool reconnect_ok = test_reconnect_after_loss();
    ok &= reconnect_ok;
    std::printf("[reconnect_after_loss] %s\n", reconnect_ok ? "ok" : "FAIL");

    std::printf("opcua_driver: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
