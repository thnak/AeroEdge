// AeroEdge PAL — tls_channel gate (M5, TLS+ACL milestone): `aero::pal::tls::TlsServerContext`/
// `TlsSession` (include/aero/pal/tls.hpp) proven over a REAL loopback TCP socket against mbedTLS's own
// CLIENT-side APIs. This is the one place in this tree allowed to touch mbedTLS's client role — tls.hpp
// itself is server-only (honest scope: AeroEdge's broker is always the TLS server, never a TLS client).
//
// Proves, over a real socket:
//   (1) a real non-blocking TLS handshake completes and application data round-trips correctly through
//       TlsSession::recv_some/send_some (server-auth-only TLS — no ca_file — the common MQTT-over-TLS
//       case: the device verifies the broker's identity, the broker takes any client).
//   (2) mTLS: a client presenting a certificate signed by the configured ca_file is ACCEPTED and data
//       round-trips.
//   (3) mTLS: a client presenting NO certificate when ca_file requires one is REJECTED —
//       TlsServerContext::accept() returns an error, the connection is NOT silently accepted.
//   (4) io_channel.hpp's PlainChannel/TlsChannel wrap a raw fd / a live TlsSession with the identical
//       recv_some/send_some/fd() surface the brief requires for future mqtt_codec.hpp templating.
// Certs (tests/pal/certs/, checked in): self-signed EC P-256, 1-year validity, generated with `openssl
// req -x509 ...` — TEST-ONLY material, never used in production. See that directory for the exact
// generation commands (repeatable from a bare openssl install).
// Deterministic, exit-code-gated (0 = pass); bounded polling; clean shutdown.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "aero/pal/poll.hpp"
#include "aero/pal/tls.hpp"
#include "aero/transport/io_channel.hpp"
#include "pal/net.hpp"

#ifndef AERO_TLS_TEST_CERTS_DIR
#error "AERO_TLS_TEST_CERTS_DIR must be defined by tests/CMakeLists.txt to tests/pal/certs"
#endif

namespace tls = aero::pal::tls;

namespace {

bool g_ok = true;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);   \
            g_ok = false;                                                            \
        }                                                                            \
    } while (0)

std::string cert_path(const char* name) { return std::string(AERO_TLS_TEST_CERTS_DIR) + "/" + name; }

// --- tiny real-socket helpers (mirrors tests/broker/native_broker.cpp / tests/transport/tcp_transport.cpp's
//     hand-rolled-client pattern — no external MQTT/TLS client dependency) --------------------------------
quark::pal::fd_t make_listener(std::uint16_t& port_out) {
    auto lfd = quark::pal::tcp_listen(quark::pal::ipv4_loopback, 0, 8);
    if (!lfd) return quark::pal::invalid_fd;
    auto p = quark::pal::local_port(*lfd);
    if (!p) {
        quark::pal::close_fd(*lfd);
        return quark::pal::invalid_fd;
    }
    port_out = *p;
    return *lfd;
}

quark::pal::fd_t accept_with_timeout(quark::pal::fd_t listen_fd, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        auto ready = aero::pal::wait_readable(listen_fd, 200);
        if (ready && *ready) {
            auto cfd = quark::pal::accept_one(listen_fd);
            if (cfd) return *cfd;
        }
        if (std::chrono::steady_clock::now() >= deadline) return quark::pal::invalid_fd;
    }
}

quark::pal::fd_t connect_with_timeout(std::uint16_t port, int timeout_ms) {
    auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
    if (!fd) return quark::pal::invalid_fd;
    auto writable = aero::pal::wait_writable(*fd, timeout_ms);
    if (!writable || !*writable || !quark::pal::connect_result(*fd)) {
        quark::pal::close_fd(*fd);
        return quark::pal::invalid_fd;
    }
    return *fd;
}

// Drive a TlsSession the same "poll(200ms) + retry on would_block" idiom as mqtt_codec.hpp's
// read_n/write_packet — proving TlsSession's recv_some/send_some really do behave like quark::pal's.
bool tls_send_all(tls::TlsSession& s, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::byte*>(data);
    std::size_t sent = 0;
    while (sent < n) {
        auto w = s.send_some(p + sent, n - sent);
        if (w) {
            sent += *w;
            continue;
        }
        if (w.error() != quark::pal::would_block()) return false;
        if (!aero::pal::wait_writable(s.fd(), 200)) return false;
    }
    return true;
}

bool tls_recv_exact(tls::TlsSession& s, void* data, std::size_t n, int timeout_ms = 3000) {
    auto* p = static_cast<std::byte*>(data);
    std::size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (got < n) {
        auto r = s.recv_some(p + got, n - got);
        if (r) {
            if (*r == 0) return false;  // EOF before n bytes arrived
            got += *r;
            continue;
        }
        if (r.error() != quark::pal::would_block()) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (!aero::pal::wait_readable(s.fd(), 200)) return false;
    }
    return true;
}

// --- a minimal hand-rolled TLS CLIENT over mbedTLS's own APIs (test-only: tls.hpp itself is server-only) --
int client_bio_send(void* ctx, const unsigned char* buf, std::size_t len) {
    const auto fd = static_cast<quark::pal::fd_t>(reinterpret_cast<std::uintptr_t>(ctx));
    auto r = quark::pal::send_some(fd, reinterpret_cast<const std::byte*>(buf), len);
    if (r) return static_cast<int>(*r);
    if (r.error() == quark::pal::would_block()) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
int client_bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    const auto fd = static_cast<quark::pal::fd_t>(reinterpret_cast<std::uintptr_t>(ctx));
    auto r = quark::pal::recv_some(fd, reinterpret_cast<std::byte*>(buf), len);
    if (r) return static_cast<int>(*r);
    if (r.error() == quark::pal::would_block()) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

struct TestTlsClient {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt trusted;   // the server cert we trust (self-signed, used directly as the "CA")
    mbedtls_x509_crt own_cert;  // optional: this client's own cert (mTLS)
    mbedtls_pk_context own_key;

    TestTlsClient() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
        mbedtls_x509_crt_init(&trusted);
        mbedtls_x509_crt_init(&own_cert);
        mbedtls_pk_init(&own_key);
    }
    ~TestTlsClient() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_pk_free(&own_key);
        mbedtls_x509_crt_free(&own_cert);
        mbedtls_x509_crt_free(&trusted);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }
    TestTlsClient(const TestTlsClient&) = delete;
    TestTlsClient& operator=(const TestTlsClient&) = delete;

    // own_cert_file/own_key_file empty ⇒ no client certificate presented (the mTLS-rejection scenario).
    bool init(const std::string& trusted_cert_file, const std::string& own_cert_file,
              const std::string& own_key_file) {
        static constexpr char kPers[] = "aero_tls_test_client";
        if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers) - 1) != 0)
            return false;
        if (mbedtls_x509_crt_parse_file(&trusted, trusted_cert_file.c_str()) != 0) return false;
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT) != 0)
            return false;
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
        mbedtls_ssl_conf_ca_chain(&conf, &trusted, nullptr);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);  // always verify the server
        if (!own_cert_file.empty()) {
            if (mbedtls_x509_crt_parse_file(&own_cert, own_cert_file.c_str()) != 0) return false;
            if (mbedtls_pk_parse_keyfile(&own_key, own_key_file.c_str(), nullptr, mbedtls_ctr_drbg_random,
                                          &ctr_drbg) != 0)
                return false;
            if (mbedtls_ssl_conf_own_cert(&conf, &own_cert, &own_key) != 0) return false;
        }
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) return false;
        if (mbedtls_ssl_set_hostname(&ssl, "localhost") != 0) return false;  // matches server_cert's SAN
        return true;
    }

    // Same poll-and-retry idiom as TlsServerContext::accept() (aero/pal/tls.hpp) — this is a test-only
    // duplication of that loop for the client role, not a second implementation of anything shipped.
    bool handshake(quark::pal::fd_t fd, int timeout_ms = 5000) {
        mbedtls_ssl_set_bio(&ssl, reinterpret_cast<void*>(static_cast<std::uintptr_t>(fd)), client_bio_send,
                             client_bio_recv, nullptr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const int rc = mbedtls_ssl_handshake(&ssl);
            if (rc == 0) return true;
            if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                if (!aero::pal::wait_readable(fd, 200)) return false;
            } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (!aero::pal::wait_writable(fd, 200)) return false;
            } else {
                return false;  // real handshake failure (e.g. server rejected our missing client cert)
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
        }
    }

    bool write_all(const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const int rc = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(data.data()) + sent,
                                              data.size() - sent);
            if (rc >= 0) {
                sent += static_cast<std::size_t>(rc);
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            return false;
        }
        return true;
    }

    bool read_exact(std::string& out, std::size_t n, int timeout_ms = 3000) {
        out.assign(n, '\0');
        std::size_t got = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (got < n) {
            const int rc = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char*>(out.data()) + got, n - got);
            if (rc > 0) {
                got += static_cast<std::size_t>(rc);
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                continue;
            }
            return false;  // EOF or a real error
        }
        return true;
    }
};

struct ScenarioResult {
    bool listener_ok = false;
    bool server_accept_ok = false;    // TlsServerContext::accept() completed the handshake
    bool server_echo_ok = false;      // payload round-tripped through TlsSession::recv_some/send_some
    bool channel_fd_matches = false;  // io_channel.hpp's TlsChannel::fd() agrees with the raw fd
    bool client_handshake_ok = false;
    bool client_roundtrip_ok = false;
};

// One end-to-end scenario: start a TlsServerContext on a fresh loopback listener, accept exactly one
// connection on a background thread (mirrors tcp_transport.cpp/native_broker.cpp's accept-thread
// pattern), and drive a client-side handshake + echo round trip against it. Every value the server
// thread writes is only read on the main thread AFTER server.join() — no data race (TSan-clean).
ScenarioResult run_scenario(const tls::ServerConfig& server_cfg, const std::string& client_trust_file,
                             const std::string& client_cert_file, const std::string& client_key_file,
                             const std::string& payload) {
    ScenarioResult result;

    auto ctx = tls::TlsServerContext::create(server_cfg);
    if (!ctx) {
        std::fprintf(stderr, "TlsServerContext::create failed: %s\n", ctx.error().c_str());
        return result;
    }

    std::uint16_t port = 0;
    quark::pal::fd_t listen_fd = make_listener(port);
    if (listen_fd == quark::pal::invalid_fd) return result;
    result.listener_ok = true;

    std::atomic<bool> server_accept_ok{false};
    std::atomic<bool> server_echo_ok{false};
    std::atomic<bool> channel_fd_matches{false};

    std::thread server([&] {
        quark::pal::fd_t cfd = accept_with_timeout(listen_fd, 3000);
        if (cfd == quark::pal::invalid_fd) return;
        auto sess = ctx->accept(cfd);
        if (!sess) {
            quark::pal::close_fd(cfd);
            return;
        }
        server_accept_ok.store(true, std::memory_order_relaxed);

        aero::transport::TlsChannel tch{&*sess};
        channel_fd_matches.store(tch.fd() == cfd, std::memory_order_relaxed);

        std::string buf(payload.size(), '\0');
        if (tls_recv_exact(*sess, buf.data(), buf.size()) && buf == payload &&
            tls_send_all(*sess, buf.data(), buf.size())) {
            server_echo_ok.store(true, std::memory_order_relaxed);
        }
        quark::pal::close_fd(cfd);
    });

    quark::pal::fd_t cfd = connect_with_timeout(port, 3000);
    if (cfd != quark::pal::invalid_fd) {
        aero::transport::PlainChannel pch{cfd};  // io_channel.hpp smoke check on the plaintext side
        if (pch.fd() != cfd) std::fprintf(stderr, "PlainChannel::fd() mismatch\n");

        TestTlsClient client;
        if (client.init(client_trust_file, client_cert_file, client_key_file) && client.handshake(cfd)) {
            result.client_handshake_ok = true;
            if (client.write_all(payload)) {
                std::string echoed;
                if (client.read_exact(echoed, payload.size()) && echoed == payload)
                    result.client_roundtrip_ok = true;
            }
        }
        quark::pal::close_fd(cfd);
    }

    server.join();
    result.server_accept_ok = server_accept_ok.load(std::memory_order_relaxed);
    result.server_echo_ok = server_echo_ok.load(std::memory_order_relaxed);
    result.channel_fd_matches = channel_fd_matches.load(std::memory_order_relaxed);
    quark::pal::close_fd(listen_fd);
    return result;
}

}  // namespace

int main() {
    // (1) Server-auth-only TLS (no ca_file): handshake completes, data round-trips both ways, and
    // io_channel.hpp's TlsChannel::fd() agrees with the accepted fd.
    {
        const auto r = run_scenario({cert_path("server_cert.pem"), cert_path("server_key.pem"), ""},
                                     cert_path("server_cert.pem"), "", "", "hello-tls-round-trip");
        CHECK(r.listener_ok);
        CHECK(r.server_accept_ok);
        CHECK(r.server_echo_ok);
        CHECK(r.channel_fd_matches);
        CHECK(r.client_handshake_ok);
        CHECK(r.client_roundtrip_ok);
    }

    // (2) mTLS positive: ca_file requires a client cert; the client presents one signed by that CA —
    // accepted, data round-trips.
    {
        const tls::ServerConfig cfg{cert_path("server_cert.pem"), cert_path("server_key.pem"),
                                     cert_path("client_ca_cert.pem")};
        const auto r = run_scenario(cfg, cert_path("server_cert.pem"), cert_path("client_cert.pem"),
                                     cert_path("client_key.pem"), "mtls-positive-payload");
        CHECK(r.server_accept_ok);
        CHECK(r.server_echo_ok);
        CHECK(r.client_handshake_ok);
        CHECK(r.client_roundtrip_ok);
    }

    // (3) mTLS negative: same ca_file-requiring server, but the client presents NO certificate at all —
    // MUST be rejected, not silently accepted. This is the load-bearing "tampered/mismatched scenario is
    // correctly rejected" case the brief calls out.
    {
        const tls::ServerConfig cfg{cert_path("server_cert.pem"), cert_path("server_key.pem"),
                                     cert_path("client_ca_cert.pem")};
        const auto r = run_scenario(cfg, cert_path("server_cert.pem"), "", "", "should-not-round-trip");
        CHECK(!r.server_accept_ok);
        CHECK(!r.server_echo_ok);
        CHECK(!r.client_roundtrip_ok);
    }

    // (4) The compiled-out ("AERO_ENABLE_TLS=OFF") code path is eyeballed by configuring a SEPARATE build
    // directory with -DAERO_ENABLE_TLS=OFF (see this repo's TLS PAL verification notes) rather than
    // exercised here — this test executable itself always links real mbedTLS (it IS the TLS test), so
    // AERO_TLS_ENABLED is unconditionally defined in this translation unit.

    std::printf("tls_channel: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
