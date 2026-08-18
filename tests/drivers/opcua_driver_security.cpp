// AeroEdge M9.4 gate (018 §8, the last v1 backlog item) — OpcUaDriver's Sign/SignAndEncrypt + cert-based
// client-auth path (opcua_security.hpp), over a REAL open62541 UA_Server configured with real security
// policies (UA_ServerConfig_setDefaultWithSecurityPolicies), mirroring tests/drivers/opcua_driver.cpp's
// own "real UA_Server on a background thread" fixture shape (open62541's own standard test idiom).
//
// FIXED TEST PORT 48715 — distinct from every other OPC-UA test's own fixed port (48711-48714, see those
// files' own banners for why fixed-not-ephemeral).
//
// TSan note: this fixture is, like opcua_driver.cpp's FakeOpcUaServer, a genuine two-thread open62541
// workload (a real UA_Server on a background thread + a real UA_Client on the main thread over an actual
// TCP loopback connection) — it reuses the exact same root-caused fixes opcua_driver.cpp's FakeOpcUaServer
// banner documents in detail (synchronous UA_Server_run_startup() on the caller's thread before the
// background thread is spawned; a FATAL-only logger installed before config setup so neither thread's
// logging reaches the racy libc mktime() call). See that file's banner for the full root-cause writeup —
// not repeated here.
//
// Covers:
//   (1) happy path: a client configured with its own cert/key + the server's cert as its ONE trusted peer
//       (opcua_security.hpp's v1 single-entry trust list) connects at SignAndEncrypt and reads a known
//       variable-node value — proving Sign/SignAndEncrypt + cert-based client auth work end to end, not
//       just "the config call didn't error".
//   (2) Sign-only (no encryption) also works — UA_ServerConfig_setDefaultWithSecurityPolicies adds
//       endpoints for every policy IN EACH MODE (open62541's own UA_ServerConfig_addAllEndpoints), so one
//       fixture server proves both modes without needing two servers.
//   (3) a client whose cert is NOT in the server's trust list is rejected — proves the server actually
//       enforces cert-based auth, not merely that encryption is turned on.
//   (4) a missing/unreadable certificate_file is a clean config-time Error at open(), no network I/O.
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
#include <open62541/plugin/pki_default.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include "aero/drivers/opcua_driver.hpp"
#include "aero/drivers/opcua_security.hpp"
#include "aero/pal/tls.hpp"
#include "aero/nodes/compute_nodes.hpp"
#include "aero/sdk/processing_context.hpp"
#include "quark/core/stream_activation.hpp"

using aero::Frame;
using aero::drivers::OpcUaDriver;
using aero::drivers::OpcUaSecurityConfig;

namespace {

constexpr std::uint16_t kTestPort = 48715;  // see file banner
constexpr const char* kEndpoint = "opc.tcp://127.0.0.1:48715";

const std::string kCertsDir = AERO_OPCUA_TEST_CERTS_DIR;  // set by tests/CMakeLists.txt

bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    if (size <= 0) { std::fclose(fp); return false; }
    out.resize(static_cast<std::size_t>(size));
    std::fseek(fp, 0, SEEK_SET);
    const std::size_t n = std::fread(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    return n == out.size();
}

UA_ByteString to_ua_bytestring(const std::vector<std::uint8_t>& bytes) noexcept {
    UA_ByteString s;
    s.length = bytes.size();
    s.data = const_cast<UA_Byte*>(bytes.data());
    return s;
}

// A real open62541 UA_Server, security policies enabled, background thread — see file banner for the
// TSan root-cause notes this fixture shares verbatim with opcua_driver.cpp's own FakeOpcUaServer.
// `trusted_client_cert_file` (empty = trust nothing) controls which client cert(s) the server accepts —
// test (3) below deliberately passes a DIFFERENT cert than the one its client actually presents.
struct SecureFakeOpcUaServer {
    UA_Server* server = nullptr;
    std::atomic<bool> running{false};
    std::thread thr;

    bool start(const std::string& trusted_client_cert_file) {
        std::vector<std::uint8_t> cert_bytes, key_bytes, trust_bytes;
        if (!read_file(kCertsDir + "/server_cert.der", cert_bytes)) {
            std::printf("SecureFakeOpcUaServer::start: failed to read %s\n",
                        (kCertsDir + "/server_cert.der").c_str());
            return false;
        }
        if (!read_file(kCertsDir + "/server_key.der", key_bytes)) {
            std::printf("SecureFakeOpcUaServer::start: failed to read %s\n",
                        (kCertsDir + "/server_key.der").c_str());
            return false;
        }
        const bool have_trust = !trusted_client_cert_file.empty();
        if (have_trust && !read_file(trusted_client_cert_file, trust_bytes)) {
            std::printf("SecureFakeOpcUaServer::start: failed to read %s\n", trusted_client_cert_file.c_str());
            return false;
        }

        const UA_ByteString cert = to_ua_bytestring(cert_bytes);
        const UA_ByteString key = to_ua_bytestring(key_bytes);
        const UA_ByteString trust = to_ua_bytestring(trust_bytes);

        UA_ServerConfig config;
        std::memset(&config, 0, sizeof(config));
        // See file banner: a FATAL-only logger, installed before config setup, so this thread never
        // reaches the racy libc mktime() call inside open62541's default stdout logger.
        config.logging = UA_Log_Stdout_new(UA_LOGLEVEL_FATAL);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        UA_StatusCode rc = UA_STATUSCODE_BADINTERNALERROR;
        while (std::chrono::steady_clock::now() < deadline) {
            rc = UA_ServerConfig_setDefaultWithSecurityPolicies(
                &config, kTestPort, &cert, &key, have_trust ? &trust : nullptr, have_trust ? 1 : 0,
                nullptr, 0, nullptr, 0);
            if (rc == UA_STATUSCODE_GOOD) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (rc != UA_STATUSCODE_GOOD) {
            std::printf("SecureFakeOpcUaServer::start: setDefaultWithSecurityPolicies failed: %s\n",
                        UA_StatusCode_name(rc));
            return false;
        }

        // The server's own cert's URI SAN ("urn:aeroedge:test:server", baked in at generation time —
        // see tests/drivers/certs/) must match config.applicationDescription.applicationUri verbatim:
        // UA_Server_run_startup verifies the server's own cert against this field (mbedTLS's plugin does
        // a raw substring search of the configured URI inside the cert's v3 extension bytes,
        // plugins/crypto/mbedtls/ua_pki_mbedtls.c) and fails BadCertificateUriInvalid on a mismatch — the
        // default applicationUri UA_ServerConfig_setDefaultWithSecurityPolicies assigns doesn't match a
        // cert generated outside open62541's own tooling.
        UA_String_clear(&config.applicationDescription.applicationUri);
        config.applicationDescription.applicationUri = UA_STRING_ALLOC("urn:aeroedge:test:server");

        server = UA_Server_newWithConfig(&config);
        if (!server) {
            std::printf("SecureFakeOpcUaServer::start: UA_Server_newWithConfig returned null\n");
            return false;
        }

        add_variable(server, "Temperature", 23.5);

        // Synchronous on the caller's thread — see file banner (same happens-before edge
        // opcua_driver.cpp's FakeOpcUaServer relies on).
        const UA_StatusCode startup_rc = UA_Server_run_startup(server);
        if (startup_rc != UA_STATUSCODE_GOOD) {
            std::printf("SecureFakeOpcUaServer::start: run_startup failed: %s\n",
                        UA_StatusCode_name(startup_rc));
            UA_Server_delete(server);
            server = nullptr;
            return false;
        }

        running.store(true, std::memory_order_release);
        thr = std::thread([this] {
            while (running.load(std::memory_order_acquire)) {
                UA_Server_run_iterate(server, true);
            }
            UA_Server_run_shutdown(server);
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

bool open_with_retry(OpcUaDriver& driver, const aero::DriverConfig& cfg, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (driver.open(cfg) == aero::DriverStatus::Ok) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

OpcUaSecurityConfig client_security(bool sign_and_encrypt) {
    OpcUaSecurityConfig sec;
    sec.certificate_file = kCertsDir + "/client_cert.der";
    sec.private_key_file = kCertsDir + "/client_key.der";
    sec.trusted_server_certificate_file = kCertsDir + "/server_cert.der";
    sec.sign_and_encrypt = sign_and_encrypt;
    sec.application_uri = "urn:aeroedge:test:client";  // must match client_cert.der's URI SAN
    return sec;
}

// ---- (1) happy path: SignAndEncrypt, cert-based client auth, real value read --------------------------
bool test_secure_connect_and_read() {
    SecureFakeOpcUaServer server;
    if (!server.start(kCertsDir + "/client_cert.der")) {
        std::printf("secure_connect_and_read: server start failed\n");
        return false;
    }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{"ns=1;s=Temperature"}, {},
                        client_security(/*sign_and_encrypt*/ true));
    aero::DriverConfig cfg{};
    bool ok = open_with_retry(driver, cfg);
    if (!ok) std::printf("secure_connect_and_read: open() never succeeded\n");

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Ok;
    ok &= outcome.frame.has_value();
    if (outcome.frame) {
        aero::ProcessingContext ctx;
        ctx.reset(&*outcome.frame);
        aero::nodes::JsonParseNode decode;
        ok &= decode.process(ctx) == aero::NodeResult::Continue;
        ok &= ctx.tags.size() == 1 && ctx.tags[0].name == "ns=1;s=Temperature" &&
              ctx.tags[0].value == 23.5;
    }

    driver.close();
    server.stop();
    if (!ok) std::printf("secure_connect_and_read: assertion failed\n");
    return ok;
}

// ---- (2) Sign-only (no encryption) also connects and reads --------------------------------------------
bool test_sign_only_connect_and_read() {
    SecureFakeOpcUaServer server;
    if (!server.start(kCertsDir + "/client_cert.der")) {
        std::printf("sign_only: server start failed\n");
        return false;
    }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{"ns=1;s=Temperature"}, {},
                        client_security(/*sign_and_encrypt*/ false));
    aero::DriverConfig cfg{};
    bool ok = open_with_retry(driver, cfg);
    if (!ok) std::printf("sign_only: open() never succeeded\n");

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Ok;
    ok &= outcome.frame.has_value();

    driver.close();
    server.stop();
    if (!ok) std::printf("sign_only: assertion failed\n");
    return ok;
}

// ---- (3) a client cert NOT in the server's trust list is rejected -------------------------------------
bool test_untrusted_client_rejected() {
    // Server trusts ONLY the server's own cert (a stand-in "someone else's cert") — never the client's.
    SecureFakeOpcUaServer server;
    if (!server.start(kCertsDir + "/server_cert.der")) {
        std::printf("untrusted_client: server start failed\n");
        return false;
    }

    OpcUaDriver driver(kEndpoint, std::vector<std::string>{"ns=1;s=Temperature"}, {},
                        client_security(/*sign_and_encrypt*/ true));
    aero::DriverConfig cfg{};
    // No retry loop here: this must never succeed, so a single bounded attempt window is enough.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool ever_connected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (driver.open(cfg) == aero::DriverStatus::Ok) { ever_connected = true; break; }
        driver.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    driver.close();
    server.stop();
    if (ever_connected) std::printf("untrusted_client: driver connected despite an untrusted cert\n");
    return !ever_connected;
}

// ---- (4) a missing certificate_file is a clean config-time Error, no network I/O ----------------------
bool test_missing_cert_file_rejected() {
    OpcUaSecurityConfig sec;
    sec.certificate_file = kCertsDir + "/does_not_exist.der";
    sec.private_key_file = kCertsDir + "/client_key.der";
    sec.trusted_server_certificate_file = kCertsDir + "/server_cert.der";

    // Port 1 is expected to refuse the connect too, but the missing cert file should fail BEFORE any
    // dial attempt (config-time rejection, same posture as this driver's other open()-time checks).
    OpcUaDriver driver("opc.tcp://127.0.0.1:1", std::vector<std::string>{}, {}, sec);
    aero::DriverConfig cfg{};
    const bool ok = driver.open(cfg) == aero::DriverStatus::Error;
    driver.close();
    if (!ok) std::printf("missing_cert_file_rejected: assertion failed\n");
    return ok;
}

}  // namespace

int main() {
    // This fixture's server (below) uses real mbedTLS-backed security policies directly, bypassing
    // apply_security_config()'s own registration call (opcua_security.hpp) — register here too, for the
    // same reason: MBEDTLS_THREADING_ALT requires mbedtls_threading_set_alt() called before ANY other
    // mbedTLS call, or CTR_DRBG seeding fails outright (MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED) — see
    // opcua_security.hpp's own banner on apply_security_config() for the full root-cause writeup.
    aero::pal::tls::detail::ensure_threading_registered();
    tzset();  // see file banner: one-time libc timezone population, single-threaded, before any
              // server/client thread exists (matches opcua_driver.cpp's own main()).

    bool ok = true;

    const bool secure_ok = test_secure_connect_and_read();
    ok &= secure_ok;
    std::printf("[secure_connect_and_read] %s\n", secure_ok ? "ok" : "FAIL");

    const bool sign_only_ok = test_sign_only_connect_and_read();
    ok &= sign_only_ok;
    std::printf("[sign_only_connect_and_read] %s\n", sign_only_ok ? "ok" : "FAIL");

    const bool untrusted_ok = test_untrusted_client_rejected();
    ok &= untrusted_ok;
    std::printf("[untrusted_client_rejected] %s\n", untrusted_ok ? "ok" : "FAIL");

    const bool missing_cert_ok = test_missing_cert_file_rejected();
    ok &= missing_cert_ok;
    std::printf("[missing_cert_file_rejected] %s\n", missing_cert_ok ? "ok" : "FAIL");

    std::printf("opcua_driver_security: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
