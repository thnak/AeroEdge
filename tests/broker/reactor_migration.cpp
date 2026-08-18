// 017 Phase 7 gate: `NativeBroker`'s IoContext reactor migration for plaintext connections
// (017-Native-Broker-Performance-Redesign.md, Phase 7). Plaintext connections are now reactor-managed
// (accept_loop() -> reactor_io_.post()); TLS connections stay on the pre-existing thread-per-connection
// session_loop() path unchanged this round.
//
// The bulk of "does the reactor path behave like the old path" correctness coverage comes for free: every
// existing tests/broker/*.cpp file that connects in plaintext (native_broker.cpp, mqtt5.cpp,
// buffered_read_framing.cpp, concurrency_stress.cpp, acl.cpp, bridge.cpp) now silently exercises the new
// reactor path instead of the old session_loop() thread, and all of them pass unmodified — including
// mqtt5.cpp's test_disconnect_reason_session_taken_over (an idle reactor session's takeover DISCONNECT
// delivered promptly — the Plan-agent critique's "idle reactor session" gap, closed) and
// concurrency_stress.cpp (repeated 20x clean — the teardown-during-fanout close_fd/io_mu race fix's
// direct regression coverage, mirroring Phase 2 Experiment C's own shape).
//
// This file covers what's genuinely NEW and not exercised by any existing test:
//   (1) A light explicit reactor round-trip anchor (QoS 0/1/2 over a plaintext, now reactor-managed,
//       connection) — mostly documentation-by-test, since the existing suite already proves this
//       transitively, but named explicitly here as this phase's own regression anchor.
//   (2) Mixed-mode fan-out: one plaintext (reactor) subscriber and one TLS (legacy) subscriber on the
//       SAME topic, published from each side — no existing test exercises both transport kinds on one
//       topic at once, which is exactly what the mixed-mode dispatch table (route_publish()) added.
//   (3) THE critical test: a reactor client that stops draining its own receive buffer right after
//       connecting must NOT block other, healthy reactor sessions' PINGREQ/PINGRESP round trips. This is
//       the direct regression test for Critical fix #1 (Session::send_packet() becoming reactor-aware) —
//       the exact class of bug (a "non-blocking" design with one unexamined blocking call path) that sank
//       Phase 6, proven here with a real run against a real stalled socket, not re-derived from the design.
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

#include "aero/broker/native_broker.hpp"
#include "aero/pal/poll.hpp"
#include "aero/pal/tls.hpp"
#include "pal/net.hpp"

#ifndef AERO_TLS_TEST_CERTS_DIR
#error "AERO_TLS_TEST_CERTS_DIR must be defined by tests/CMakeLists.txt to tests/pal/certs"
#endif

namespace mqtt = aero::transport::mqtt;
using aero::broker::Config;
using aero::broker::NativeBroker;

namespace {

std::string cert_path(const char* name) { return std::string(AERO_TLS_TEST_CERTS_DIR) + "/" + name; }

// ===== plaintext client — same shape/primitives as native_broker.cpp's TestClient (this tree's
//       established per-file-owns-its-own-client precedent, e.g. concurrency_stress.cpp) =================
class PlainClient {
public:
    ~PlainClient() { close(); }

    [[nodiscard]] bool connect(std::uint16_t port, const std::string& client_id) {
        auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
        if (!fd) return false;
        fd_ = *fd;
        const auto writable = aero::pal::wait_writable(fd_, 2000);
        if (!writable || !*writable || !quark::pal::connect_result(fd_)) return false;

        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(std::byte{0x04});
        vh.push_back(std::byte{0x02});  // clean session
        mqtt::put_u16_be(vh, /*keep_alive_s=*/60);
        mqtt::put_str(vh, client_id);
        if (!mqtt::write_packet(fd_, std::byte{0x10}, vh)) return false;
        auto ack = wait_for(0x20, 2000);
        return ack.has_value() && ack->body.size() >= 2 && std::to_integer<std::uint8_t>(ack->body[1]) == 0;
    }

    // Stops the background reader thread WITHOUT closing the fd — the connection stays open and fully
    // established, but nothing drains its receive buffer from this point on, so the SERVER's outbound
    // socket buffer for this connection fills and stays full: real TCP backpressure, not a mocked delay.
    void stop_reading() {
        running_.store(false, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
    }

    // Companion to stop_reading(): restarts draining on the SAME still-open connection. Used to prove the
    // eventual-delivery half of the head-of-line-blocking fix (Experiment B's own two-part shape) — a
    // backed-up recipient's queued backlog must not be silently dropped, only delayed.
    void resume_reading() {
        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });
    }

    [[nodiscard]] bool subscribe(const std::string& filter, std::uint8_t qos = 1) {
        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, next_id());
        mqtt::put_str(vh, filter);
        vh.push_back(static_cast<std::byte>(qos));
        if (!mqtt::write_packet(fd_, std::byte{0x82}, vh)) return false;
        return wait_for(0x90, 2000).has_value();
    }

    [[nodiscard]] bool publish(const std::string& topic, const std::string& payload, std::uint8_t qos,
                               bool retain = false) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        std::uint8_t flags = static_cast<std::uint8_t>(qos << 1);
        if (retain) flags |= 0x01;
        if (!mqtt::write_packet(fd_, static_cast<std::byte>(0x30 | flags), vh)) return false;
        return qos == 0 || wait_for(0x40, 2000).has_value();
    }

    [[nodiscard]] bool publish_qos2(const std::string& topic, const std::string& payload) {
        const std::uint16_t pid = next_id();
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        mqtt::put_u16_be(vh, pid);
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        if (!mqtt::write_packet(fd_, std::byte{0x30 | (2 << 1)}, vh)) return false;
        auto pubrec = wait_for(0x50, 2000);
        if (!pubrec) return false;
        std::vector<std::byte> rel;
        mqtt::put_u16_be(rel, pid);
        if (!mqtt::write_packet(fd_, std::byte{0x62}, rel)) return false;
        return wait_for(0x70, 2000).has_value();
    }

    [[nodiscard]] std::optional<std::pair<std::string, std::string>> wait_publish(int timeout_ms = 1000) {
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

    [[nodiscard]] bool ping() { return mqtt::write_packet(fd_, std::byte{0xC0}, {}) && wait_for(0xD0, 3000).has_value(); }

    // Sends a PINGREQ without waiting for (or ever reading) the PINGRESP reply — for a client that has
    // already stop_reading(). This is the send-side direction only (client -> server), unaffected by the
    // stalled RECEIVE direction, and it forces the broker to attempt Session::send_packet()'s direct-reply
    // path (PINGRESP) against this specific backed-up session — the exact call Critical fix #1 targets,
    // as opposed to the fan-out path (already non-blocking even before that fix).
    [[nodiscard]] bool ping_no_wait() { return mqtt::write_packet(fd_, std::byte{0xC0}, {}); }

    // Writes a DISCONNECT and closes the fd immediately after, with no pause in between — races the
    // broker's own recv() dispatch of this connection's already-written burst against the peer's FIN,
    // mirroring a real fire-and-forget device that disconnects right after publishing. Deliberately closes
    // the fd BEFORE stopping/joining the reader thread (unlike close()) — joining first would add a
    // poll-interval-sized delay between the last write() and the actual close_fd(), which is enough time
    // for the broker to already drain the burst and never race the FIN against pending data at all.
    void send_disconnect_and_close() {
        std::vector<std::byte> disc;
        (void)mqtt::write_packet(fd_, std::byte{0xE0}, disc);
        if (fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(fd_);
            fd_ = quark::pal::invalid_fd;
        }
        running_.store(false, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
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

// ===== minimal TLS client — same mbedTLS client-role pattern as native_broker_security.cpp's
//       RawTlsClient/RawTlsChannel/TlsTestClient (this file keeps its own copy, matching this tree's
//       existing per-file precedent rather than sharing a header across test binaries) ===================
struct RawTlsClient {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt trusted;

    RawTlsClient() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
        mbedtls_x509_crt_init(&trusted);
    }
    ~RawTlsClient() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&trusted);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }
    RawTlsClient(const RawTlsClient&) = delete;
    RawTlsClient& operator=(const RawTlsClient&) = delete;

    bool init(const std::string& trusted_cert_file) {
        static constexpr char kPers[] = "aero_reactor_migration_test";
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
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) return false;
        if (mbedtls_ssl_set_hostname(&ssl, "localhost") != 0) return false;
        return true;
    }

    static int bio_send(void* ctx, const unsigned char* buf, std::size_t len) {
        const auto fd = static_cast<quark::pal::fd_t>(reinterpret_cast<std::uintptr_t>(ctx));
        auto r = quark::pal::send_some(fd, reinterpret_cast<const std::byte*>(buf), len);
        if (r) return static_cast<int>(*r);
        return r.error() == quark::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : -1;
    }
    static int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
        const auto fd = static_cast<quark::pal::fd_t>(reinterpret_cast<std::uintptr_t>(ctx));
        auto r = quark::pal::recv_some(fd, reinterpret_cast<std::byte*>(buf), len);
        if (r) return static_cast<int>(*r);
        return r.error() == quark::pal::would_block() ? MBEDTLS_ERR_SSL_WANT_READ : -1;
    }

    bool handshake(quark::pal::fd_t fd, int timeout_ms = 5000) {
        mbedtls_ssl_set_bio(&ssl, reinterpret_cast<void*>(static_cast<std::uintptr_t>(fd)), bio_send,
                             bio_recv, nullptr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const int rc = mbedtls_ssl_handshake(&ssl);
            if (rc == 0) return true;
            if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                if (!aero::pal::wait_readable(fd, 200)) return false;
            } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (!aero::pal::wait_writable(fd, 200)) return false;
            } else {
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
        }
    }
};

struct RawTlsChannel {
    mbedtls_ssl_context* ssl = nullptr;
    quark::pal::fd_t fd_val = quark::pal::invalid_fd;
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

class TlsClient {
public:
    ~TlsClient() { close(); }

    [[nodiscard]] bool connect(std::uint16_t port, const std::string& client_id,
                                const std::string& trusted_cert_file) {
        auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
        if (!fd) return false;
        fd_ = *fd;
        const auto writable = aero::pal::wait_writable(fd_, 2000);
        if (!writable || !*writable || !quark::pal::connect_result(fd_)) return false;
        if (!tls_.init(trusted_cert_file)) return false;
        if (!tls_.handshake(fd_)) return false;
        channel_.ssl = &tls_.ssl;
        channel_.fd_val = fd_;

        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(std::byte{0x04});
        vh.push_back(std::byte{0x02});
        mqtt::put_u16_be(vh, 60);
        mqtt::put_str(vh, client_id);
        if (!mqtt::write_packet(channel_, std::byte{0x10}, vh)) return false;
        auto ack = wait_for(0x20, 3000);
        return ack.has_value() && ack->body.size() >= 2 && std::to_integer<std::uint8_t>(ack->body[1]) == 0;
    }

    [[nodiscard]] bool subscribe(const std::string& filter, std::uint8_t qos = 1) {
        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, next_id());
        mqtt::put_str(vh, filter);
        vh.push_back(static_cast<std::byte>(qos));
        if (!mqtt::write_packet(channel_, std::byte{0x82}, vh)) return false;
        return wait_for(0x90, 3000).has_value();
    }

    [[nodiscard]] bool publish(const std::string& topic, const std::string& payload, std::uint8_t qos) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        if (!mqtt::write_packet(channel_, static_cast<std::byte>(0x30 | (qos << 1)), vh)) return false;
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

// (1) Explicit reactor round-trip anchor: QoS 0/1/2 over a plaintext (now reactor-managed) connection.
bool test_reactor_roundtrip_qos012() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    PlainClient sub, pub;
    ok &= sub.connect(port, "reactor-sub");
    ok &= pub.connect(port, "reactor-pub");
    ok &= sub.subscribe("reactor/topic", /*qos=*/2);

    ok &= pub.publish("reactor/topic", "qos0-payload", 0);
    auto m0 = sub.wait_publish();
    ok &= m0.has_value() && m0->first == "reactor/topic" && m0->second == "qos0-payload";

    ok &= pub.publish("reactor/topic", "qos1-payload", 1);
    auto m1 = sub.wait_publish();
    ok &= m1.has_value() && m1->second == "qos1-payload";

    ok &= pub.publish_qos2("reactor/topic", "qos2-payload");
    auto m2 = sub.wait_publish();
    ok &= m2.has_value() && m2->second == "qos2-payload";

    ok &= sub.ping();

    sub.close();
    pub.close();
    broker.stop();
    if (!ok) std::fprintf(stderr, "test_reactor_roundtrip_qos012: FAILED\n");
    return ok;
}

// (2) Mixed-mode fan-out: one plaintext (reactor) subscriber + one TLS (legacy) subscriber on the SAME
// topic; a plaintext (reactor-thread) publisher and a TLS (legacy-thread) publisher each publish once —
// exercises 3 of the 4 dispatch-table cells in one test (reactor->reactor, reactor->legacy via the
// hand-off pool, legacy->reactor) since both publishes fan out to BOTH subscribers.
bool test_mixed_mode_reactor_and_tls() {
    bool ok = true;
    Config cfg{"127.0.0.1", /*listen_port=*/0};
    cfg.tls = aero::pal::tls::ServerConfig{cert_path("server_cert.pem"), cert_path("server_key.pem"), ""};
    cfg.tls_port = 0;
    NativeBroker broker(cfg);
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();
    const std::uint16_t tls_port = broker.listen_port_tls();

    PlainClient plain_sub, plain_pub;
    TlsClient tls_sub, tls_pub;
    ok &= plain_sub.connect(port, "mixed-plain-sub");
    ok &= plain_pub.connect(port, "mixed-plain-pub");
    ok &= tls_sub.connect(tls_port, "mixed-tls-sub", cert_path("server_cert.pem"));
    ok &= tls_pub.connect(tls_port, "mixed-tls-pub", cert_path("server_cert.pem"));

    ok &= plain_sub.subscribe("mixed/topic", 1);
    ok &= tls_sub.subscribe("mixed/topic", 1);

    // reactor-thread-originated publish -> fans out to a reactor recipient (enqueue_reactor_publish) AND
    // a legacy recipient (enqueue_legacy_handoff, Critical fix #2's bounded worker pool).
    ok &= plain_pub.publish("mixed/topic", "from-reactor", 1);
    auto r1 = plain_sub.wait_publish();
    auto r2 = tls_sub.wait_publish();
    ok &= r1.has_value() && r1->second == "from-reactor";
    ok &= r2.has_value() && r2->second == "from-reactor";

    // legacy-thread-originated publish -> fans out to a reactor recipient (enqueue_reactor_publish, called
    // from a non-reactor thread) AND another legacy recipient (unchanged inline blocking publish_to()).
    ok &= tls_pub.publish("mixed/topic", "from-legacy", 1);
    auto r3 = plain_sub.wait_publish();
    auto r4 = tls_sub.wait_publish();
    ok &= r3.has_value() && r3->second == "from-legacy";
    ok &= r4.has_value() && r4->second == "from-legacy";

    plain_sub.close();
    plain_pub.close();
    tls_sub.close();
    tls_pub.close();
    broker.stop();
    if (!ok) std::fprintf(stderr, "test_mixed_mode_reactor_and_tls: FAILED\n");
    return ok;
}

// (3) THE critical test: a reactor client that stops draining its own receive buffer must not block other
// reactor sessions. Direct regression test for Critical fix #1 (Session::send_packet() becoming
// reactor-aware) — before that fix, ANY outbound write to a backed-up reactor session (a direct reply
// like PINGRESP/SUBACK, or a fan-out PUBLISH) would block the shared reactor thread inside
// write_packet()'s no-deadline retry loop for as long as the recipient stayed backed up, freezing every
// OTHER reactor session sharing that one thread. Uses the same real-backpressure technique this doc's own
// Phase 6 investigation proved necessary (2048-byte payloads, large enough to genuinely exceed default OS
// socket buffers — a small payload lets the kernel buffer absorb everything and never actually blocks,
// silently passing even against the pre-fix code, which is exactly the trap Phase 6's own test design
// fell into on its first attempt before being corrected).
bool test_stalled_reactor_client_does_not_block_others() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    static constexpr const char* kTopic = "stall/topic";
    static constexpr int kBurstCount = 500;
    const std::string payload(2048, 'x');  // ~1MB total burst — exceeds typical default OS socket buffers

    // The stalled recipient: connects and subscribes normally (so it's a real, indexed fan-out target),
    // then stops draining — real TCP backpressure once its receive buffer fills, not a mocked delay.
    PlainClient stalled;
    ok &= stalled.connect(port, "stalled-client");
    ok &= stalled.subscribe(kTopic, /*qos=*/0);
    stalled.stop_reading();

    // Several HEALTHY reactor sessions, subscribed to the SAME topic, that keep draining normally
    // throughout — this is Experiment B's own validated shape: does a slow recipient's backlog delay
    // delivery to healthy recipients sharing the same reactor thread?
    static constexpr int kHealthyCount = 5;
    std::vector<PlainClient> healthy(kHealthyCount);
    for (int i = 0; i < kHealthyCount; ++i) {
        ok &= healthy[static_cast<std::size_t>(i)].connect(port, "healthy-" + std::to_string(i));
        ok &= healthy[static_cast<std::size_t>(i)].subscribe(kTopic, /*qos=*/0);
    }

    PlainClient pub;
    ok &= pub.connect(port, "stall-publisher");
    for (int i = 0; i < kBurstCount; ++i) ok &= pub.publish(kTopic, payload, /*qos=*/0);

    // By now the stalled session's outbound queue is genuinely backed up (real would-block from a real
    // full socket buffer, not simulated). Force several MORE Session::send_packet() direct-reply attempts
    // against this SAME backed-up session — PINGREQ -> PINGRESP is the simplest one — which is the exact
    // call path Critical fix #1 targets (as opposed to the fan-out path above, already non-blocking even
    // before that fix). The stalled client never reads the replies; what matters is whether ISSUING them
    // blocks the shared reactor thread.
    for (int i = 0; i < 5; ++i) ok &= stalled.ping_no_wait();

    // Every healthy subscriber must receive the FULL burst within a generous but bounded time — if the
    // reactor thread were blocked on the stalled recipient (the pre-fix behavior), these would time out
    // and this loop would never reach kBurstCount, failing the test deterministically rather than hanging
    // (wait_publish()'s own per-call timeout bounds worst case).
    for (int h = 0; h < kHealthyCount; ++h) {
        int received = 0;
        for (int i = 0; i < kBurstCount; ++i) {
            auto m = healthy[static_cast<std::size_t>(h)].wait_publish(3000);
            if (!m || m->first != kTopic) break;
            ++received;
        }
        if (received != kBurstCount) {
            std::fprintf(stderr, "test_stalled_reactor_client_does_not_block_others: healthy[%d] got %d/%d\n",
                         h, received, kBurstCount);
            ok = false;
        }
    }

    // A healthy PUBLISHER's own subsequent traffic must also stay unaffected (route_publish()'s fan-out
    // loop itself must not have stalled on the backed-up recipient either).
    ok &= pub.publish(kTopic, "tail-message", 0);

    // Experiment B's OTHER half (§2.4): a backed-up recipient must be slow, not starved — its queued
    // backlog must eventually arrive in full once it resumes draining, not be silently dropped. Proves
    // the non-blocking outbound queue (out_queue/out_current, try_drain_reactor_send()) actually delivers
    // everything it accepted, it just does so asynchronously across however many EPOLLOUT events it takes.
    stalled.resume_reading();
    int stalled_received = 0;
    for (int i = 0; i < kBurstCount + 1; ++i) {  // +1 for the tail-message above
        auto m = stalled.wait_publish(5000);
        if (!m || m->first != kTopic) break;
        ++stalled_received;
    }
    if (stalled_received != kBurstCount + 1) {
        std::fprintf(stderr, "test_stalled_reactor_client_does_not_block_others: stalled recipient got %d/%d (backlog was dropped, not just delayed)\n",
                     stalled_received, kBurstCount + 1);
        ok = false;
    }

    stalled.close();
    for (auto& h : healthy) h.close();
    pub.close();
    broker.stop();
    if (!ok) std::fprintf(stderr, "test_stalled_reactor_client_does_not_block_others: FAILED\n");
    return ok;
}

// (4) Phase 7c regression: a QoS-0 publisher that fires a burst of PUBLISHes back-to-back and then
// disconnects+closes immediately after (the ordinary shape of a fire-and-forget device) must not lose any
// of them. Direct regression test for a real bug found via bench/broker/broker_bench.cpp during 7c's own
// measurement work: on_reactor_ready() treated EPOLLERR/EPOLLHUP as an immediate short-circuit BEFORE
// draining EPOLLIN. The Windows WSAPoll backend (pal/windows_x86_64/net.hpp) reports POLLHUP alongside
// POLLRDNORM exactly when a peer writes a final burst and closes right after — the short-circuit discarded
// data that was already fully received into the kernel socket buffer, silently and permanently. A tiny
// 1-publisher/1-subscriber QoS-0 burst of 10-50 messages reproduced 20-70% loss well within any timeout
// before the fix; 100% delivery after. Repeated across several reconnect cycles (not just one shot) since
// whether a given close races the broker's recv() dispatch depends on OS thread scheduling.
bool test_burst_then_immediate_disconnect_no_loss() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    static constexpr const char* kTopic = "burst/topic";
    static constexpr int kMessages = 50;
    static constexpr int kIterations = 20;

    PlainClient sub;
    ok &= sub.connect(port, "burst-sub");
    ok &= sub.subscribe(kTopic, /*qos=*/0);

    for (int iter = 0; iter < kIterations && ok; ++iter) {
        PlainClient pub;
        ok &= pub.connect(port, "burst-pub-" + std::to_string(iter));
        const std::string payload(64, 'x');
        for (int i = 0; i < kMessages; ++i) ok &= pub.publish(kTopic, payload, /*qos=*/0);
        pub.send_disconnect_and_close();  // no pause between the last write and close, by design

        int received = 0;
        for (int i = 0; i < kMessages; ++i) {
            auto m = sub.wait_publish(2000);
            if (!m || m->first != kTopic) break;
            ++received;
        }
        if (received != kMessages) {
            std::fprintf(stderr,
                         "test_burst_then_immediate_disconnect_no_loss: iter %d got %d/%d (lost %d)\n",
                         iter, received, kMessages, kMessages - received);
            ok = false;
        }
    }

    sub.close();
    broker.stop();
    if (!ok) std::fprintf(stderr, "test_burst_then_immediate_disconnect_no_loss: FAILED\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_reactor_roundtrip_qos012();
    ok &= test_mixed_mode_reactor_and_tls();
    ok &= test_stalled_reactor_client_does_not_block_others();
    ok &= test_burst_then_immediate_disconnect_no_loss();
    std::printf("reactor_migration: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
