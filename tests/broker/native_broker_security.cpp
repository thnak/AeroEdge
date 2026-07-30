// AeroEdge M5 TLS+ACL milestone gate: `NativeBroker`'s integration of the TLS listener
// (aero/pal/tls.hpp) and the per-topic/CONNECT-auth seam (aero/broker/acl.hpp) into the actual broker
// session lifecycle — as opposed to tests/pal/tls_channel.cpp (TLS PAL in isolation, no MQTT) and
// tests/broker/acl.cpp (ACL policy logic in isolation, no sockets), this file proves the WIRING: a real
// NativeBroker with cfg.tls/cfg.authenticate/cfg.authorizer set, driven by real MQTT clients over real
// sockets (one TLS-capable, hand-rolled over mbedTLS's client-role APIs — tls.hpp itself is server-only,
// same posture as tls_channel.cpp's own TestTlsClient; one plaintext, mirroring
// tests/broker/native_broker.cpp's TestClient pattern).
//
// Proves, over real sockets:
//   (1) TLS handshake + round-trip: a client completes a real TLS handshake against cfg.tls's listener,
//       CONNECT/SUBSCRIBEs, another TLS client PUBLISHes, the payload round-trips through the encrypted
//       channel correctly.
//   (2) mTLS: a client presenting no certificate is REJECTED when cfg.tls->ca_file requires one (not
//       silently accepted as plaintext-equivalent); a client presenting a valid client cert IS accepted.
//   (3) ACL matrix: a TopicAclAuthorizer (Default::Closed) — an allowed PUBLISH/SUBSCRIBE succeeds; a
//       denied PUBLISH is silently dropped (no ack, subscriber never sees it, on_publish() never fires);
//       a denied SUBSCRIBE gets SUBACK 0x80 and never receives anything even when the topic is
//       separately, validly PUBLISHed by someone else.
//   (4) CONNECT auth: cfg.authenticate rejects bad credentials with CONNACK rc=0x04 BEFORE any session
//       state is touched — proven by reconnecting under the SAME client_id with valid credentials and
//       clean_session=0 and asserting session_present is false (nothing survived the rejected attempt).
//   (5) Backward-compat smoke check: a Config with tls/authenticate/authorizer all unset behaves like an
//       ordinary plaintext no-auth broker (the exhaustive version of this lives in
//       tests/broker/native_broker.cpp/bridge.cpp — this is just confirming M5's Config changes didn't
//       shift a default).
// Deterministic, exit-code-gated (0 = pass); bounded polling; clean shutdown.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "aero/broker/acl.hpp"
#include "aero/broker/native_broker.hpp"
#include "aero/pal/poll.hpp"
#include "aero/pal/tls.hpp"
#include "pal/net.hpp"

#ifndef AERO_TLS_TEST_CERTS_DIR
#error "AERO_TLS_TEST_CERTS_DIR must be defined by tests/CMakeLists.txt to tests/pal/certs"
#endif

namespace mqtt = aero::transport::mqtt;
using aero::broker::AclAction;
using aero::broker::Config;
using aero::broker::NativeBroker;
using aero::broker::TopicAclAuthorizer;

namespace {

std::string cert_path(const char* name) { return std::string(AERO_TLS_TEST_CERTS_DIR) + "/" + name; }

// CONNACK, materialized (not just the pass/fail bool the plaintext TestClient in native_broker.cpp
// returns) — the auth-rejection test (4) needs the actual return-code byte and session_present bit.
struct ConnAck {
    bool ok = false;  // a CONNACK actually arrived — NOT the same as rc == 0
    bool session_present = false;
    std::uint8_t rc = 0xFF;
};

// ===== plaintext client (ACL + CONNECT-auth tests, mirrors tests/broker/native_broker.cpp's TestClient,
//       extended with username/password on CONNECT and a granted/denied SUBACK byte on subscribe) =======
class PlainTestClient {
public:
    ~PlainTestClient() { close(); }

    [[nodiscard]] ConnAck connect(std::uint16_t port, const std::string& client_id,
                                   const std::string& username = "", const std::string& password = "",
                                   bool clean_session = true) {
        ConnAck result;
        auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
        if (!fd) return result;
        fd_ = *fd;
        const auto writable = aero::pal::wait_writable(fd_, 2000);
        if (!writable || !*writable || !quark::pal::connect_result(fd_)) return result;

        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        std::uint8_t flags = 0;
        if (clean_session) flags |= 0x02;
        if (!username.empty()) flags |= 0x80;
        if (!password.empty()) flags |= 0x40;
        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(std::byte{0x04});
        vh.push_back(static_cast<std::byte>(flags));
        mqtt::put_u16_be(vh, 60);
        mqtt::put_str(vh, client_id);
        if (!username.empty()) mqtt::put_str(vh, username);
        if (!password.empty()) mqtt::put_str(vh, password);
        if (!mqtt::write_packet(fd_, std::byte{0x10}, vh)) return result;

        auto ack = wait_for(0x20, 2000);
        if (!ack || ack->body.size() < 2) return result;
        result.ok = true;
        result.session_present = (std::to_integer<std::uint8_t>(ack->body[0]) & 0x01) != 0;
        result.rc = std::to_integer<std::uint8_t>(ack->body[1]);
        return result;
    }

    // Returns the SUBACK return-code byte for this (single-filter) SUBSCRIBE — 0x80 means denied
    // (3.9.3) — or nullopt if no SUBACK arrived at all.
    [[nodiscard]] std::optional<std::uint8_t> subscribe(const std::string& filter, std::uint8_t qos = 1) {
        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, next_id());
        mqtt::put_str(vh, filter);
        vh.push_back(static_cast<std::byte>(qos));
        if (!mqtt::write_packet(fd_, std::byte{0x82}, vh)) return std::nullopt;
        auto ack = wait_for(0x90, 2000);
        if (!ack || ack->body.size() < 3) return std::nullopt;
        return std::to_integer<std::uint8_t>(ack->body[2]);
    }

    // qos>0: returns whether a PUBACK arrived — false is the expected, correct outcome for a PUBLISH the
    // broker's ACL silently drops (no ack at all), not just a wire error.
    [[nodiscard]] bool publish(const std::string& topic, const std::string& payload, std::uint8_t qos,
                               bool retain, int puback_timeout_ms = 2000) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        std::uint8_t pflags = static_cast<std::uint8_t>(qos << 1);
        if (retain) pflags |= 0x01;
        if (!mqtt::write_packet(fd_, static_cast<std::byte>(0x30 | pflags), vh)) return false;
        return qos == 0 || wait_for(0x40, puback_timeout_ms).has_value();
    }

    [[nodiscard]] std::optional<std::pair<std::string, std::string>> wait_publish(int timeout_ms = 1500) {
        auto pkt = wait_for(0x30, timeout_ms);
        if (!pkt) return std::nullopt;
        const std::vector<std::byte>& b = pkt->body;
        if (b.size() < 2) return std::nullopt;
        const std::uint16_t tlen =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        std::size_t pos = 2 + tlen;
        if (pos > b.size()) return std::nullopt;
        std::string topic(reinterpret_cast<const char*>(b.data() + 2), tlen);
        const std::uint8_t qos = (pkt->type_flags >> 1) & 0x03;
        if (qos > 0) {
            if (pos + 2 > b.size()) return std::nullopt;
            const std::uint16_t pid =
                (std::to_integer<std::uint8_t>(b[pos]) << 8) | std::to_integer<std::uint8_t>(b[pos + 1]);
            pos += 2;
            std::vector<std::byte> ack;
            mqtt::put_u16_be(ack, pid);
            (void)mqtt::write_packet(fd_, std::byte{0x40}, ack);
        }
        std::string payload(reinterpret_cast<const char*>(b.data() + pos), b.size() - pos);
        return std::make_pair(std::move(topic), std::move(payload));
    }

    void close() {
        running_.store(false, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
        if (fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(fd_);
            fd_ = quark::pal::invalid_fd;
        }
    }

private:
    void reader_loop() {
        while (running_.load(std::memory_order_acquire)) {
            auto pkt = mqtt::read_packet(fd_, running_);
            if (!pkt) break;
            std::lock_guard<std::mutex> g(mu_);
            inbox_.push_back(std::move(*pkt));
        }
    }
    std::optional<mqtt::Packet> wait_for(std::uint8_t type_high_nibble, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> g(mu_);
                for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
                    if ((it->type_flags & 0xF0) == type_high_nibble) {
                        mqtt::Packet p = std::move(*it);
                        inbox_.erase(it);
                        return p;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::uint16_t next_id() {
        const std::uint16_t id = ++packet_id_;
        return id == 0 ? ++packet_id_ : id;
    }

    quark::pal::fd_t fd_ = quark::pal::invalid_fd;
    std::atomic<bool> running_{false};
    std::thread reader_;
    std::mutex mu_;
    std::vector<mqtt::Packet> inbox_;
    std::uint16_t packet_id_ = 0;
};

// ===== TLS client (tests 1/2): a hand-rolled mbedTLS CLIENT (tls.hpp itself is server-only — same
//       posture tests/pal/tls_channel.cpp's own TestTlsClient documents) + a small Channel-shaped struct
//       (recv_some/send_some/fd()) so this test can drive the REAL mqtt_codec.hpp read_packet/
//       write_packet templates over it (Task 2's whole point: any Channel-shaped type works, not just
//       PlainChannel/TlsChannel) instead of hand-rolling MQTT framing a second time. ==================
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

// Test-only mbedTLS client role (mirrors tests/pal/tls_channel.cpp's TestTlsClient). own_cert/key files
// empty ⇒ no client certificate presented — the mTLS-rejection scenario (test 2).
struct RawTlsClient {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt trusted;
    mbedtls_x509_crt own_cert;
    mbedtls_pk_context own_key;

    RawTlsClient() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
        mbedtls_x509_crt_init(&trusted);
        mbedtls_x509_crt_init(&own_cert);
        mbedtls_pk_init(&own_key);
    }
    ~RawTlsClient() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_pk_free(&own_key);
        mbedtls_x509_crt_free(&own_cert);
        mbedtls_x509_crt_free(&trusted);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }
    RawTlsClient(const RawTlsClient&) = delete;
    RawTlsClient& operator=(const RawTlsClient&) = delete;

    bool init(const std::string& trusted_cert_file, const std::string& own_cert_file,
              const std::string& own_key_file) {
        static constexpr char kPers[] = "aero_native_broker_security_test";
        if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers) - 1) != 0)
            return false;
        if (mbedtls_x509_crt_parse_file(&trusted, trusted_cert_file.c_str()) != 0) return false;
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT) != 0)
            return false;
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
        mbedtls_ssl_conf_ca_chain(&conf, &trusted, nullptr);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
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
};

// A Channel-shaped (recv_some/send_some/fd()) wrapper over RawTlsClient's mbedtls_ssl_context — proves
// mqtt_codec.hpp's templated read_packet/write_packet really do work over ANY conforming type, not just
// aero::transport::PlainChannel/TlsChannel.
struct RawTlsChannel {
    mbedtls_ssl_context* ssl = nullptr;
    quark::pal::fd_t fd_val = quark::pal::invalid_fd;
    // mbedTLS's mbedtls_ssl_context is NOT safe to call concurrently from two threads without
    // MBEDTLS_THREADING_C compiled in (which this tree's mbedTLS build doesn't enable) — this test client
    // has a reader thread calling recv_some (mbedtls_ssl_read) while the test's own thread calls
    // send_some (mbedtls_ssl_write) for CONNECT/SUBSCRIBE/PUBLISH, so the two must be mutually excluded.
    // Cheap to hold: both calls are single, non-blocking attempts (the caller already polled fd()
    // readable/writable first), never a call that blocks waiting on the network.
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();

    [[nodiscard]] std::expected<std::size_t, std::error_code> recv_some(std::byte* buf, std::size_t n) {
        std::lock_guard<std::mutex> g(*mu);
        const int rc = mbedtls_ssl_read(ssl, reinterpret_cast<unsigned char*>(buf), n);
        if (rc >= 0) return static_cast<std::size_t>(rc);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return std::unexpected(quark::pal::would_block());
        return std::unexpected(std::make_error_code(std::errc::connection_aborted));
    }
    [[nodiscard]] std::expected<std::size_t, std::error_code> send_some(const std::byte* buf, std::size_t n) {
        std::lock_guard<std::mutex> g(*mu);
        const int rc = mbedtls_ssl_write(ssl, reinterpret_cast<const unsigned char*>(buf), n);
        if (rc >= 0) return static_cast<std::size_t>(rc);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return std::unexpected(quark::pal::would_block());
        return std::unexpected(std::make_error_code(std::errc::connection_aborted));
    }
    [[nodiscard]] quark::pal::fd_t fd() const noexcept { return fd_val; }
};

class TlsTestClient {
public:
    ~TlsTestClient() { close(); }

    // own_cert_file/own_key_file empty ⇒ no client certificate presented (test 2's rejection scenario).
    // Returns false on ANY failure — TCP connect, TLS handshake (including a server-side mTLS rejection),
    // or setup — the caller doesn't need to distinguish which for this test's purposes.
    [[nodiscard]] bool connect_tls(std::uint16_t port, const std::string& trusted_cert_file,
                                    const std::string& own_cert_file = "",
                                    const std::string& own_key_file = "") {
        auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
        if (!fd) return false;
        fd_ = *fd;
        const auto writable = aero::pal::wait_writable(fd_, 2000);
        if (!writable || !*writable || !quark::pal::connect_result(fd_)) return false;
        if (!tls_.init(trusted_cert_file, own_cert_file, own_key_file)) return false;
        if (!tls_.handshake(fd_)) return false;
        channel_.ssl = &tls_.ssl;
        channel_.fd_val = fd_;
        return true;
    }

    [[nodiscard]] ConnAck connect_mqtt(const std::string& client_id) {
        ConnAck result;
        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(std::byte{0x04});
        vh.push_back(std::byte{0x02});  // clean session
        mqtt::put_u16_be(vh, 60);
        mqtt::put_str(vh, client_id);
        if (!mqtt::write_packet(channel_, std::byte{0x10}, vh)) return result;

        auto ack = wait_for(0x20, 3000);
        if (!ack || ack->body.size() < 2) return result;
        result.ok = true;
        result.session_present = (std::to_integer<std::uint8_t>(ack->body[0]) & 0x01) != 0;
        result.rc = std::to_integer<std::uint8_t>(ack->body[1]);
        return result;
    }

    [[nodiscard]] bool subscribe(const std::string& filter, std::uint8_t qos = 1) {
        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, next_id());
        mqtt::put_str(vh, filter);
        vh.push_back(static_cast<std::byte>(qos));
        if (!mqtt::write_packet(channel_, std::byte{0x82}, vh)) return false;
        return wait_for(0x90, 3000).has_value();
    }

    [[nodiscard]] bool publish(const std::string& topic, const std::string& payload, std::uint8_t qos,
                               bool retain) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        std::uint8_t pflags = static_cast<std::uint8_t>(qos << 1);
        if (retain) pflags |= 0x01;
        if (!mqtt::write_packet(channel_, static_cast<std::byte>(0x30 | pflags), vh)) return false;
        return qos == 0 || wait_for(0x40, 3000).has_value();
    }

    [[nodiscard]] std::optional<std::pair<std::string, std::string>> wait_publish(int timeout_ms = 2000) {
        auto pkt = wait_for(0x30, timeout_ms);
        if (!pkt) return std::nullopt;
        const std::vector<std::byte>& b = pkt->body;
        if (b.size() < 2) return std::nullopt;
        const std::uint16_t tlen =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        std::size_t pos = 2 + tlen;
        if (pos > b.size()) return std::nullopt;
        std::string topic(reinterpret_cast<const char*>(b.data() + 2), tlen);
        const std::uint8_t qos = (pkt->type_flags >> 1) & 0x03;
        if (qos > 0) {
            if (pos + 2 > b.size()) return std::nullopt;
            const std::uint16_t pid =
                (std::to_integer<std::uint8_t>(b[pos]) << 8) | std::to_integer<std::uint8_t>(b[pos + 1]);
            pos += 2;
            std::vector<std::byte> ack;
            mqtt::put_u16_be(ack, pid);
            (void)mqtt::write_packet(channel_, std::byte{0x40}, ack);
        }
        std::string payload(reinterpret_cast<const char*>(b.data() + pos), b.size() - pos);
        return std::make_pair(std::move(topic), std::move(payload));
    }

    void close() {
        running_.store(false, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
        if (fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(fd_);
            fd_ = quark::pal::invalid_fd;
        }
    }

private:
    void reader_loop() {
        while (running_.load(std::memory_order_acquire)) {
            auto pkt = mqtt::read_packet(channel_, running_);
            if (!pkt) break;
            std::lock_guard<std::mutex> g(mu_);
            inbox_.push_back(std::move(*pkt));
        }
    }
    std::optional<mqtt::Packet> wait_for(std::uint8_t type_high_nibble, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> g(mu_);
                for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
                    if ((it->type_flags & 0xF0) == type_high_nibble) {
                        mqtt::Packet p = std::move(*it);
                        inbox_.erase(it);
                        return p;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::uint16_t next_id() {
        const std::uint16_t id = ++packet_id_;
        return id == 0 ? ++packet_id_ : id;
    }

    quark::pal::fd_t fd_ = quark::pal::invalid_fd;
    RawTlsClient tls_;
    RawTlsChannel channel_;
    std::atomic<bool> running_{false};
    std::thread reader_;
    std::mutex mu_;
    std::vector<mqtt::Packet> inbox_;
    std::uint16_t packet_id_ = 0;
};

// (1) TLS handshake + round-trip: server-auth-only TLS (no ca_file — the common MQTT-over-TLS case), a
// real handshake, CONNECT/SUBSCRIBE, a second client PUBLISHes, the payload round-trips correctly.
bool test_tls_roundtrip() {
    bool ok = true;
    Config cfg{"127.0.0.1", /*listen_port=*/0};
    cfg.tls = aero::pal::tls::ServerConfig{cert_path("server_cert.pem"), cert_path("server_key.pem"), ""};
    cfg.tls_port = 0;  // ephemeral — keeps this test isolated from any other listening on 8883
    NativeBroker broker(cfg);
    ok &= broker.start().has_value();
    const std::uint16_t tls_port = broker.listen_port_tls();

    TlsTestClient sub, pub;
    ok &= sub.connect_tls(tls_port, cert_path("server_cert.pem"));  // self-signed cert used as its own CA
    ok &= sub.connect_mqtt("tls-sub").ok;
    ok &= sub.subscribe("sensor/#", /*qos=*/1);

    ok &= pub.connect_tls(tls_port, cert_path("server_cert.pem"));
    ok &= pub.connect_mqtt("tls-pub").ok;
    ok &= pub.publish("sensor/temp", "23.5", /*qos=*/1, /*retain=*/false);

    auto got = sub.wait_publish();
    ok &= got.has_value() && got->first == "sensor/temp" && got->second == "23.5";

    sub.close();
    pub.close();
    broker.stop();
    return ok;
}

// (2) mTLS: cfg.tls->ca_file requires a client certificate. A client presenting NO certificate at all is
// REJECTED (handshake fails outright, not silently accepted as plaintext-equivalent) — the load-bearing
// "tamper/rejection" case. A client presenting a cert signed by ca_file IS accepted, as a positive
// control proving the server is actually enforcing mTLS rather than just refusing everyone.
bool test_mtls_rejection() {
    bool ok = true;
    Config cfg{"127.0.0.1", /*listen_port=*/0};
    cfg.tls = aero::pal::tls::ServerConfig{cert_path("server_cert.pem"), cert_path("server_key.pem"),
                                            cert_path("client_ca_cert.pem")};
    cfg.tls_port = 0;
    NativeBroker broker(cfg);
    ok &= broker.start().has_value();
    const std::uint16_t tls_port = broker.listen_port_tls();

    {
        TlsTestClient no_cert;
        // Deliberately no own_cert_file/own_key_file — the server MUST refuse this connection. NOTE: in
        // TLS 1.3, the CLIENT's own mbedtls_ssl_handshake() can return success once it has sent its
        // Finished message — the server's rejection (missing required client cert) is a separate alert
        // the client only observes on its NEXT read/write, not synchronously inside handshake() itself
        // (this is exactly why tls_channel.cpp's own equivalent scenario asserts on the SERVER-side
        // accept() result, not the client-side handshake() return — same reasoning applies here). So the
        // authoritative check is: does a subsequent MQTT exchange actually work end-to-end? It must not —
        // connect_mqtt() must fail (no CONNACK ever arrives, because the server already tore the
        // connection down) regardless of what connect_tls() itself returned.
        const bool handshook = no_cert.connect_tls(tls_port, cert_path("server_cert.pem"));
        const bool mqtt_ok = handshook && no_cert.connect_mqtt("no-cert-client").ok;
        ok &= !mqtt_ok;  // must NOT have gotten a working MQTT session — the connection was refused
        no_cert.close();
    }
    {
        TlsTestClient with_cert;
        // Positive control: a client presenting a cert signed by ca_file IS accepted end-to-end — proves
        // the server is actually enforcing mTLS, not just refusing every connection unconditionally.
        const bool handshook = with_cert.connect_tls(tls_port, cert_path("server_cert.pem"),
                                                      cert_path("client_cert.pem"), cert_path("client_key.pem"));
        ok &= handshook;
        if (handshook) ok &= with_cert.connect_mqtt("mtls-client").ok;
        with_cert.close();
    }

    broker.stop();
    return ok;
}

// (3) ACL matrix: TopicAclAuthorizer(Default::Closed) with principal-scoped rules — an allowed PUBLISH/
// SUBSCRIBE succeeds; a denied PUBLISH is silently dropped (no PUBACK, subscriber never sees it, even
// though it's validly subscribed); a denied SUBSCRIBE gets SUBACK 0x80 and never receives anything even
// when the SAME topic is separately, validly PUBLISHed by someone else.
bool test_acl_matrix() {
    bool ok = true;
    auto authorizer = std::make_shared<TopicAclAuthorizer>(TopicAclAuthorizer::Default::Closed);
    // "allowed/topic": both principals may act on it — the straightforward positive case.
    authorizer->add_rule("sub-principal", "allowed/topic", AclAction::Subscribe, true);
    authorizer->add_rule("pub-principal", "allowed/topic", AclAction::Publish, true);
    // "restricted/topic": pub-principal MAY publish, but NO ONE may subscribe (no matching rule, Closed
    // default) — isolates "denied SUBSCRIBE" from "denied PUBLISH".
    authorizer->add_rule("pub-principal", "restricted/topic", AclAction::Publish, true);
    // "secret/topic": sub-principal MAY subscribe, but pub-principal has no Publish rule for it — isolates
    // "denied PUBLISH" from "denied SUBSCRIBE".
    authorizer->add_rule("sub-principal", "secret/topic", AclAction::Subscribe, true);

    Config cfg{"127.0.0.1", /*listen_port=*/0};
    cfg.authorizer = authorizer;
    // Principal = the CONNECT username verbatim, unconditionally accepted — this test is about the ACL
    // seam, not authentication, so every credential is "valid" and just becomes the principal.
    cfg.authenticate = [](std::string_view user, std::string_view) -> std::optional<std::string> {
        return std::string(user);
    };
    NativeBroker broker(cfg);
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    PlainTestClient sub, pub;
    ok &= sub.connect(port, "acl-sub", "sub-principal").ok;
    ok &= pub.connect(port, "acl-pub", "pub-principal").ok;

    // Allowed SUBSCRIBE: granted (not 0x80).
    auto sub_allowed_rc = sub.subscribe("allowed/topic", /*qos=*/1);
    ok &= sub_allowed_rc.has_value() && *sub_allowed_rc != 0x80;
    // Denied SUBSCRIBE (no rule grants sub-principal Subscribe on restricted/topic): SUBACK 0x80.
    auto sub_denied_rc = sub.subscribe("restricted/topic", /*qos=*/1);
    ok &= sub_denied_rc.has_value() && *sub_denied_rc == 0x80;
    // Allowed SUBSCRIBE on secret/topic (isolates the denied-PUBLISH case below).
    auto sub_secret_rc = sub.subscribe("secret/topic", /*qos=*/1);
    ok &= sub_secret_rc.has_value() && *sub_secret_rc != 0x80;

    // Allowed PUBLISH: PUBACK arrives, subscriber receives it.
    ok &= pub.publish("allowed/topic", "hi-allowed", /*qos=*/1, /*retain=*/false);
    auto got_allowed = sub.wait_publish();
    ok &= got_allowed.has_value() && got_allowed->first == "allowed/topic" && got_allowed->second == "hi-allowed";

    // Allowed PUBLISH on a topic the subscriber was DENIED for: publisher still gets its PUBACK (the
    // publish itself is authorized), but the subscriber never receives it — the denial happened at
    // SUBSCRIBE time, so the filter was never added to its subscription list at all.
    ok &= pub.publish("restricted/topic", "hi-restricted", /*qos=*/1, /*retain=*/false);
    auto got_restricted = sub.wait_publish(500);
    ok &= !got_restricted.has_value();

    // Denied PUBLISH (pub-principal has no rule for secret/topic, Default::Closed): silently dropped —
    // NO PUBACK at all (not even an error ack) — and the validly-subscribed subscriber never sees it.
    ok &= !pub.publish("secret/topic", "hi-secret", /*qos=*/1, /*retain=*/false, /*puback_timeout_ms=*/800);
    auto got_secret = sub.wait_publish(500);
    ok &= !got_secret.has_value();

    sub.close();
    pub.close();
    broker.stop();
    return ok;
}

// (4) CONNECT auth rejection: cfg.authenticate only accepts one specific username/password. A CONNECT
// with wrong credentials gets CONNACK rc=0x04 and the connection closes; critically, this happens BEFORE
// any session-state mutation — proven by reconnecting under the SAME client_id afterward with VALID
// credentials and clean_session=0: session_present must be false, because the earlier rejected attempt
// (which also used clean_session=0) never created anything for it to restore.
bool test_connect_auth_rejection() {
    bool ok = true;
    Config cfg{"127.0.0.1", /*listen_port=*/0};
    cfg.authenticate = [](std::string_view user, std::string_view pass) -> std::optional<std::string> {
        if (user == "valid_user" && pass == "valid_pass") return std::string(user);
        return std::nullopt;
    };
    NativeBroker broker(cfg);
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    {
        PlainTestClient bad;
        auto ack = bad.connect(port, "auth-cid", "wrong_user", "wrong_pass", /*clean_session=*/false);
        ok &= ack.ok;                    // a CONNACK did arrive...
        ok &= ack.rc == 0x04;            // ...with the "bad username or password" return code (3.2.2.3)
        ok &= !ack.session_present;      // nothing was ever restored for a first-ever CONNECT under this id
        bad.close();
    }
    // Give the broker's session thread a moment to finish tearing down the rejected connection before
    // reconnecting under the same client_id — mirrors native_broker.cpp's test_persistent_session's own
    // synchronization-aid comment (no cross-thread "teardown complete" signal to wait on instead).
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    {
        PlainTestClient good;
        auto ack = good.connect(port, "auth-cid", "valid_user", "valid_pass", /*clean_session=*/false);
        ok &= ack.ok;
        ok &= ack.rc == 0x00;
        // The load-bearing assertion: the rejected attempt left ZERO trace. If it had touched
        // client_sessions_/stored_sessions_, this reconnect could observe stale state; the clean answer
        // is session_present == false, because broker-side nothing was EVER stored under "auth-cid".
        ok &= !ack.session_present;
        good.close();
    }

    broker.stop();
    return ok;
}

// (5) Backward-compat smoke check: Config with tls/authenticate/authorizer all unset behaves exactly like
// an ordinary plaintext no-auth broker — brief, since the exhaustive version of this already lives in
// tests/broker/native_broker.cpp/bridge.cpp; this just confirms M5's Config additions didn't shift a
// default.
bool test_backward_compat_smoke() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();
    ok &= broker.listen_port_tls() == 0;  // no TLS listener ever bound

    PlainTestClient sub, pub;
    ok &= sub.connect(port, "compat-sub").ok;
    ok &= pub.connect(port, "compat-pub").ok;
    ok &= sub.subscribe("compat/topic", /*qos=*/1).value_or(0x80) != 0x80;
    ok &= pub.publish("compat/topic", "hello", /*qos=*/1, /*retain=*/false);
    auto got = sub.wait_publish();
    ok &= got.has_value() && got->first == "compat/topic" && got->second == "hello";

    sub.close();
    pub.close();
    broker.stop();
    return ok;
}

}  // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"test_tls_roundtrip", test_tls_roundtrip},
        {"test_mtls_rejection", test_mtls_rejection},
        {"test_acl_matrix", test_acl_matrix},
        {"test_connect_auth_rejection", test_connect_auth_rejection},
        {"test_backward_compat_smoke", test_backward_compat_smoke},
    };
    bool ok = true;
    for (const auto& t : tests) {
        const bool r = t.fn();
        std::printf("%s: %s\n", t.name, r ? "OK" : "FAIL");
        ok &= r;
    }

    std::printf("native_broker_security: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
