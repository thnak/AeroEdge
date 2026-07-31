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
// Deterministic-enough, exit-code-gated (0 = pass); bounded polling/retries; clean shutdown.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include "aero/drivers/opcua_driver.hpp"
#include "aero/nodes/compute_nodes.hpp"
#include "aero/sdk/processing_context.hpp"
#include "quark/core/stream_activation.hpp"

using aero::Frame;
using aero::drivers::OpcUaDriver;

namespace {

constexpr std::uint16_t kTestPort = 48711;  // see file banner: fixed, not ephemeral
constexpr const char* kEndpoint = "opc.tcp://127.0.0.1:48711";

// ===== a real open62541 UA_Server, hosted on a background thread ======================================
// UA_Server_run(server, &running) is open62541's own standard start/stop idiom (its examples and its own
// test suite both drive a server exactly this way from a worker thread, flipping `running` to stop).
struct FakeOpcUaServer {
    UA_Server* server = nullptr;
    volatile UA_Boolean running = false;
    std::thread thr;
    std::atomic<bool> run_failed{false};

    // Retries the bind (bounded) since a just-stopped server on the same port may still hold the socket
    // in TIME_WAIT for a moment — mirrors this test's own bounded-retry philosophy elsewhere.
    bool start() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        UA_ServerConfig config;
        std::memset(&config, 0, sizeof(config));
        while (std::chrono::steady_clock::now() < deadline) {
            if (UA_ServerConfig_setMinimal(&config, kTestPort, nullptr) == UA_STATUSCODE_GOOD) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        server = UA_Server_newWithConfig(&config);
        if (!server) return false;

        add_variable(server, "Temperature", 23.5);
        add_variable(server, "Pressure", 101.3);

        run_failed.store(false, std::memory_order_release);
        running = true;
        thr = std::thread([this] {
            const UA_StatusCode rc = UA_Server_run(server, &running);
            if (rc != UA_STATUSCODE_GOOD) run_failed.store(true, std::memory_order_release);
        });

        // No public "server is ready" callback — give run_startup (inside UA_Server_run, on the thread
        // just spawned) a bounded moment to actually bind the listening socket before a client dials.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return !run_failed.load(std::memory_order_acquire);
    }

    void stop() {
        if (!server) return;
        running = false;
        if (thr.joinable()) thr.join();
        UA_Server_delete(server);
        server = nullptr;
    }

    static void add_variable(UA_Server* srv, const char* name, UA_Double value) {
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
        attr.description = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(name));
        attr.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(name));
        attr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ;

        const UA_NodeId node_id = UA_NODEID_STRING(1, const_cast<char*>(name));
        const UA_QualifiedName browse_name = UA_QUALIFIEDNAME(1, const_cast<char*>(name));
        const UA_NodeId parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        const UA_NodeId parent_ref = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
        const UA_NodeId type_def = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
        UA_Server_addVariableNode(srv, node_id, parent, parent_ref, browse_name, type_def, attr, nullptr,
                                   nullptr);
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
    bool ok = true;

    const bool happy_ok = test_happy_path();
    ok &= happy_ok;
    std::printf("[happy_path] %s\n", happy_ok ? "ok" : "FAIL");

    const bool oversized_ok = test_oversized_rejected();
    ok &= oversized_ok;
    std::printf("[oversized_rejected] %s\n", oversized_ok ? "ok" : "FAIL");

    const bool reconnect_ok = test_reconnect_after_loss();
    ok &= reconnect_ok;
    std::printf("[reconnect_after_loss] %s\n", reconnect_ok ? "ok" : "FAIL");

    std::printf("opcua_driver: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
