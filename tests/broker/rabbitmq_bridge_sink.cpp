// AeroEdge M8 gate (017 §6 Status) — RabbitMqBridgeSink (broker/rabbitmq_bridge_sink.hpp) over a
// hand-rolled fake AMQP 0-9-1 server (same quark::pal::net + aero::pal::poll primitives every other
// socket test in this tree already uses — tcp_transport.cpp/native_broker.cpp/bridge.cpp/
// modbus_tcp_driver.cpp). rabbitmq-c is CLIENT-ONLY (ships no test/mock broker), so this test speaks just
// enough of the AMQP 0-9-1 wire protocol by hand to drive a REAL RabbitMqBridgeSink (backed by the real
// rabbitmq-c library) through a full CONNECT/login/channel-open/publish/close round trip — the same
// "listen, accept, speak just enough of the wire protocol to validate a real client" shape
// tests/drivers/modbus_tcp_driver.cpp's FakeModbusServer uses.
//
// Covers:
//   (a) real connect+publish round-trip: the fake server completes the Connection.Start/StartOk/Tune/
//       TuneOk/Open/OpenOk + Channel.Open/OpenOk handshake, then captures a Basic.Publish (method +
//       content-header + body frames) and asserts the exchange, routing key (== topic), delivery-mode
//       (from qos), and payload bytes all match what RabbitMqBridgeSink sent. The fake server then
//       answers the client's own graceful Channel.Close/Connection.Close so close()/the destructor never
//       hangs waiting for a reply that never comes.
//   (b) connect() against a closed/refused port (a bound-then-immediately-closed ephemeral listener, so
//       the port is deterministically refused) returns false cleanly — no hang, no crash.
//   (c) after a successful connect, the fake server closes the accepted socket right after the handshake
//       (no publish served) — publish() against the now-dead connection eventually returns false
//       (rabbitmq-c's own write-path failure), and once it does, stays failing fast (no reconnect
//       attempt), never hangs. NOT asserted to fail on the very first post-kill call: a local socket
//       write can succeed (buffered by the kernel) before the peer's FIN/RST has actually been
//       observed — confirmed platform-dependent during dual-platform verification (Windows/clang++
//       failed the first post-kill publish(); WSL/g++-14 let exactly one through before failing) — so
//       this polls for the first failure within a bounded deadline instead of asserting immediacy.
// Deterministic-enough, exit-code-gated (0 = pass); bounded polling/reads; clean shutdown.
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "aero/broker/rabbitmq_bridge_sink.hpp"
#include "aero/pal/poll.hpp"
#include "pal/net.hpp"

using aero::broker::BridgeConfig;
using aero::broker::RabbitMqBridgeSink;

namespace {

// ===== byte helpers (big-endian, AMQP 0-9-1 wire encoding — this test's OWN independent codec, kept
// separate/duplicated on purpose: this is the fake SERVER's side, not code under test) ==================
void put_u8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(std::byte{v}); }
void put_u16_be(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::byte>(v & 0xFF));
}
void put_u32_be(std::vector<std::byte>& b, std::uint32_t v) {
    b.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
    b.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::byte>(v & 0xFF));
}
void put_shortstr(std::vector<std::byte>& b, std::string_view s) {
    put_u8(b, static_cast<std::uint8_t>(s.size()));
    for (char c : s) b.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
}
void put_longstr(std::vector<std::byte>& b, std::string_view s) {
    put_u32_be(b, static_cast<std::uint32_t>(s.size()));
    for (char c : s) b.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
}
void put_empty_table(std::vector<std::byte>& b) { put_u32_be(b, 0); }  // field-table length 0

std::uint16_t get_u16_be(const std::byte* p) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(p[0]) << 8) |
                                       std::to_integer<std::uint16_t>(p[1]));
}
std::uint32_t get_u32_be(const std::byte* p) {
    return (std::to_integer<std::uint32_t>(p[0]) << 24) | (std::to_integer<std::uint32_t>(p[1]) << 16) |
           (std::to_integer<std::uint32_t>(p[2]) << 8) | std::to_integer<std::uint32_t>(p[3]);
}
std::uint64_t get_u64_be(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(p[i]);
    return v;
}

// ===== bounded send/recv over quark::pal::net (mirrors modbus_tcp_driver.cpp's own helpers) ============
bool send_all(quark::pal::fd_t fd, const std::byte* buf, std::size_t n) {
    std::size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (sent < n) {
        auto w = quark::pal::send_some(fd, buf + sent, n - sent);
        if (w) { sent += *w; continue; }
        if (w.error() != quark::pal::would_block()) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (!aero::pal::wait_writable(fd, 200)) return false;
    }
    return true;
}
bool recv_exact(quark::pal::fd_t fd, std::byte* buf, std::size_t n, const std::atomic<bool>& running) {
    std::size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (got < n) {
        if (!running.load(std::memory_order_acquire)) return false;
        auto r = quark::pal::recv_some(fd, buf + got, n - got);
        if (r) {
            if (*r == 0) return false;  // peer closed
            got += *r;
            continue;
        }
        if (r.error() != quark::pal::would_block()) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (!aero::pal::wait_readable(fd, 200)) return false;
    }
    return true;
}

// ===== AMQP 0-9-1 frame envelope: octet type + short channel + long size + payload + octet 0xCE =======
bool write_frame(quark::pal::fd_t fd, std::uint8_t type, std::uint16_t channel,
                  const std::vector<std::byte>& payload) {
    std::vector<std::byte> hdr;
    put_u8(hdr, type);
    put_u16_be(hdr, channel);
    put_u32_be(hdr, static_cast<std::uint32_t>(payload.size()));
    if (!send_all(fd, hdr.data(), hdr.size())) return false;
    if (!payload.empty() && !send_all(fd, payload.data(), payload.size())) return false;
    const std::byte end{0xCE};
    return send_all(fd, &end, 1);
}

struct RecvFrame {
    std::uint8_t type = 0;
    std::uint16_t channel = 0;
    std::vector<std::byte> payload;
};
std::optional<RecvFrame> read_frame(quark::pal::fd_t fd, const std::atomic<bool>& running) {
    std::array<std::byte, 7> hdr{};
    if (!recv_exact(fd, hdr.data(), hdr.size(), running)) return std::nullopt;
    RecvFrame f;
    f.type = std::to_integer<std::uint8_t>(hdr[0]);
    f.channel = get_u16_be(&hdr[1]);
    const std::uint32_t size = get_u32_be(&hdr[3]);
    f.payload.resize(size);
    if (size > 0 && !recv_exact(fd, f.payload.data(), size, running)) return std::nullopt;
    std::byte end{};
    if (!recv_exact(fd, &end, 1, running)) return std::nullopt;
    if (std::to_integer<std::uint8_t>(end) != 0xCE) return std::nullopt;
    return f;
}

// ===== the fake AMQP 0-9-1 server's handshake state machine (server side of the CONNECT sequence the
// task's own banner spells out) ==========================================================================
bool do_handshake(quark::pal::fd_t conn, const std::atomic<bool>& running) {
    // 1. Protocol header: "AMQP" 0x00 0x00 0x09 0x01
    std::array<std::byte, 8> proto{};
    if (!recv_exact(conn, proto.data(), proto.size(), running)) return false;
    static constexpr std::array<char, 8> kExpected = {'A', 'M', 'Q', 'P', 0x00, 0x00, 0x09, 0x01};
    for (std::size_t i = 0; i < 8; ++i)
        if (std::to_integer<std::uint8_t>(proto[i]) != static_cast<std::uint8_t>(kExpected[i])) return false;

    // 2. Connection.Start (class 10, method 10)
    {
        std::vector<std::byte> p;
        put_u16_be(p, 10);
        put_u16_be(p, 10);
        put_u8(p, 0);  // version-major
        put_u8(p, 9);  // version-minor
        put_empty_table(p);
        put_longstr(p, "PLAIN");
        put_longstr(p, "en_US");
        if (!write_frame(conn, 1, 0, p)) return false;
    }

    // 3. Connection.StartOk — consume, don't need to parse.
    if (!read_frame(conn, running)) return false;

    // 4. Connection.Tune (class 10, method 30)
    {
        std::vector<std::byte> p;
        put_u16_be(p, 10);
        put_u16_be(p, 30);
        put_u16_be(p, 0);       // channel-max
        put_u32_be(p, 131072);  // frame-max
        put_u16_be(p, 0);       // heartbeat
        if (!write_frame(conn, 1, 0, p)) return false;
    }

    // 5. Connection.TuneOk — consume.
    if (!read_frame(conn, running)) return false;

    // 6. Connection.Open — consume.
    if (!read_frame(conn, running)) return false;

    // 7. Connection.OpenOk (class 10, method 41)
    {
        std::vector<std::byte> p;
        put_u16_be(p, 10);
        put_u16_be(p, 41);
        put_shortstr(p, "");  // reserved-1
        if (!write_frame(conn, 1, 0, p)) return false;
    }

    // 8. Channel.Open (class 20, method 10) on channel 1 — consume.
    const auto chopen = read_frame(conn, running);
    if (!chopen || chopen->channel != 1) return false;

    // 9. Channel.OpenOk (class 20, method 11) on channel 1
    {
        std::vector<std::byte> p;
        put_u16_be(p, 20);
        put_u16_be(p, 11);
        put_longstr(p, "");  // reserved-1
        if (!write_frame(conn, 1, 1, p)) return false;
    }
    return true;
}

struct CapturedPublish {
    std::string exchange;
    std::string routing_key;
    std::uint8_t delivery_mode = 0;
    std::vector<std::byte> body;
};

constexpr std::uint16_t kDeliveryModeFlag = 0x1000;  // AMQP_BASIC_DELIVERY_MODE_FLAG's bit position

// Reads Basic.Publish (method) + a content-header frame + one-or-more body frames, per the task banner's
// state machine. Returns nullopt on any framing error / unexpected method.
std::optional<CapturedPublish> read_publish(quark::pal::fd_t conn, const std::atomic<bool>& running) {
    const auto method = read_frame(conn, running);
    if (!method || method->type != 1 || method->payload.size() < 4) return std::nullopt;
    const auto& mb = method->payload;
    if (get_u16_be(&mb[0]) != 60 || get_u16_be(&mb[2]) != 40) return std::nullopt;  // Basic.Publish

    std::size_t pos = 4;
    if (pos + 2 > mb.size()) return std::nullopt;
    pos += 2;  // reserved-1 (ticket, short)
    if (pos >= mb.size()) return std::nullopt;
    const std::uint8_t exch_len = std::to_integer<std::uint8_t>(mb[pos]);
    pos += 1;
    if (pos + exch_len > mb.size()) return std::nullopt;
    CapturedPublish cap;
    cap.exchange.assign(reinterpret_cast<const char*>(&mb[pos]), exch_len);
    pos += exch_len;
    if (pos >= mb.size()) return std::nullopt;
    const std::uint8_t rk_len = std::to_integer<std::uint8_t>(mb[pos]);
    pos += 1;
    if (pos + rk_len > mb.size()) return std::nullopt;
    cap.routing_key.assign(reinterpret_cast<const char*>(&mb[pos]), rk_len);
    // remaining bits byte (mandatory/immediate) — not needed by this test.

    const auto header = read_frame(conn, running);
    if (!header || header->type != 2 || header->payload.size() < 14) return std::nullopt;
    const auto& hb = header->payload;
    const std::uint64_t body_size = get_u64_be(&hb[4]);
    std::size_t hpos = 12;
    const std::uint16_t flags = get_u16_be(&hb[hpos]);
    hpos += 2;
    if (flags & kDeliveryModeFlag) {
        if (hpos >= hb.size()) return std::nullopt;
        cap.delivery_mode = std::to_integer<std::uint8_t>(hb[hpos]);
        hpos += 1;
    }

    std::vector<std::byte> body;
    body.reserve(body_size);
    while (body.size() < body_size) {
        const auto bf = read_frame(conn, running);
        if (!bf || bf->type != 3) return std::nullopt;
        body.insert(body.end(), bf->payload.begin(), bf->payload.end());
    }
    cap.body = std::move(body);
    return cap;
}

// After a captured publish (or in the plain "just handshake, then wait" mode), answer the client's own
// graceful Channel.Close/Connection.Close so RabbitMqBridgeSink::close()/its destructor never hangs
// waiting for a CloseOk that would otherwise never arrive.
void serve_graceful_close(quark::pal::fd_t conn, const std::atomic<bool>& running) {
    while (running.load(std::memory_order_acquire)) {
        const auto frame = read_frame(conn, running);
        if (!frame) return;
        if (frame->type != 1 || frame->payload.size() < 4) continue;
        const std::uint16_t class_id = get_u16_be(&frame->payload[0]);
        const std::uint16_t method_id = get_u16_be(&frame->payload[2]);
        if (class_id == 20 && method_id == 40) {  // Channel.Close
            std::vector<std::byte> p;
            put_u16_be(p, 20);
            put_u16_be(p, 41);  // Channel.CloseOk
            if (!write_frame(conn, 1, frame->channel, p)) return;
        } else if (class_id == 10 && method_id == 50) {  // Connection.Close
            std::vector<std::byte> p;
            put_u16_be(p, 10);
            put_u16_be(p, 51);  // Connection.CloseOk
            (void)write_frame(conn, 1, 0, p);
            return;
        }
    }
}

// A single-connection fake AMQP server: accepts ONE client (RabbitMqBridgeSink's whole session lifetime
// is one TCP connection), runs the handshake, then either captures one publish + serves a graceful close
// (default), or kills the connection right after the handshake (kill_after_handshake — scenario (c)).
struct FakeAmqpServer {
    quark::pal::fd_t listen_fd = quark::pal::invalid_fd;
    std::uint16_t port = 0;
    std::atomic<bool> running{false};
    std::thread thr;
    bool kill_after_handshake = false;

    std::mutex mu;
    std::condition_variable cv;
    bool handshake_done = false;
    bool got_publish = false;
    CapturedPublish captured;

    bool start(bool kill_mode = false) {
        kill_after_handshake = kill_mode;
        auto l = quark::pal::tcp_listen(quark::pal::ipv4_loopback, /*port*/ 0);
        if (!l) return false;
        listen_fd = *l;
        auto p = quark::pal::local_port(listen_fd);
        if (!p) return false;
        port = *p;
        running.store(true, std::memory_order_release);
        thr = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop() {
        running.store(false, std::memory_order_release);
        if (thr.joinable()) thr.join();
        if (listen_fd != quark::pal::invalid_fd) {
            quark::pal::close_fd(listen_fd);
            listen_fd = quark::pal::invalid_fd;
        }
    }

    void accept_loop() {
        while (running.load(std::memory_order_acquire)) {
            const auto ready = aero::pal::wait_readable(listen_fd, 200);
            if (!ready || !*ready) continue;
            auto a = quark::pal::accept_one(listen_fd);
            if (!a) continue;
            serve(*a);
            return;  // single-connection server — see struct banner
        }
    }

    void serve(quark::pal::fd_t conn) {
        if (!do_handshake(conn, running)) {
            quark::pal::close_fd(conn);
            return;
        }
        {
            std::lock_guard<std::mutex> g(mu);
            handshake_done = true;
        }
        cv.notify_all();

        if (kill_after_handshake) {
            quark::pal::close_fd(conn);  // scenario (c): kill the session right after the handshake
            return;
        }

        auto pub = read_publish(conn, running);
        if (pub) {
            std::lock_guard<std::mutex> g(mu);
            captured = *pub;
            got_publish = true;
        }
        cv.notify_all();

        serve_graceful_close(conn, running);
        quark::pal::close_fd(conn);
    }

    bool wait_handshake(int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return handshake_done; });
    }
    bool wait_publish(int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return got_publish; });
    }
};

// ---- (a) real connect+publish round-trip --------------------------------------------------------------
bool test_connect_and_publish() {
    FakeAmqpServer server;
    if (!server.start()) { std::printf("connect_and_publish: server start failed\n"); return false; }

    RabbitMqBridgeSink sink;
    BridgeConfig cfg;
    cfg.endpoint = "127.0.0.1";
    cfg.port = server.port;
    cfg.username = "guest";
    cfg.password = "guest";
    cfg.vhost = "/";
    cfg.exchange = "aero.bridge";

    bool ok = sink.connect(cfg);
    if (!ok) std::printf("connect_and_publish: connect() failed\n");
    ok &= server.wait_handshake();

    const std::string payload_str = "hello-from-rabbitmq-bridge";
    std::vector<std::byte> payload(payload_str.size());
    for (std::size_t i = 0; i < payload_str.size(); ++i)
        payload[i] = static_cast<std::byte>(static_cast<std::uint8_t>(payload_str[i]));

    ok &= sink.publish("sensor/line1/temp", payload, /*qos=*/1);
    ok &= server.wait_publish();

    {
        std::lock_guard<std::mutex> g(server.mu);
        ok &= server.got_publish;
        if (server.got_publish) {
            ok &= server.captured.exchange == "aero.bridge";
            ok &= server.captured.routing_key == "sensor/line1/temp";
            ok &= server.captured.delivery_mode == 2;  // qos>=1 -> persistent
            ok &= server.captured.body.size() == payload.size();
            for (std::size_t i = 0; i < payload.size() && i < server.captured.body.size(); ++i)
                ok &= server.captured.body[i] == payload[i];
        }
    }

    // qos == 0 -> non-persistent(1).
    std::vector<std::byte> payload0{std::byte{'x'}};
    ok &= sink.publish("sensor/line1/other", payload0, /*qos=*/0);

    sink.close();
    server.stop();
    if (!ok) std::printf("connect_and_publish: assertion failed\n");
    return ok;
}

// ---- (b) connect() against a closed/refused port returns false cleanly --------------------------------
bool test_connect_refused() {
    // Bind an ephemeral listener, read back its port, then close it immediately — the port is now
    // deterministically refused (nothing listening), unlike guessing an arbitrary unused port number.
    auto l = quark::pal::tcp_listen(quark::pal::ipv4_loopback, /*port*/ 0);
    if (!l) { std::printf("connect_refused: could not bind a throwaway listener\n"); return false; }
    auto p = quark::pal::local_port(*l);
    quark::pal::close_fd(*l);
    if (!p) { std::printf("connect_refused: could not read back the throwaway port\n"); return false; }

    RabbitMqBridgeSink sink;
    BridgeConfig cfg;
    cfg.endpoint = "127.0.0.1";
    cfg.port = *p;

    const auto start = std::chrono::steady_clock::now();
    const bool connected = sink.connect(cfg);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    bool ok = !connected;
    // "No hang": well under the 5s connect timeout the sink uses internally.
    ok &= elapsed < std::chrono::seconds(4);
    if (!ok) std::printf("connect_refused: connect()=%d elapsed_ms=%lld\n", connected,
                          static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
    return ok;
}

// ---- (c) connection killed mid-session: publish() eventually fails, then stays fail-fast, never hangs -
bool test_publish_after_connection_killed() {
    FakeAmqpServer server;
    if (!server.start(/*kill_mode=*/true)) {
        std::printf("publish_after_killed: server start failed\n");
        return false;
    }

    RabbitMqBridgeSink sink;
    BridgeConfig cfg;
    cfg.endpoint = "127.0.0.1";
    cfg.port = server.port;

    bool ok = sink.connect(cfg);
    if (!ok) std::printf("publish_after_killed: connect() failed\n");
    ok &= server.wait_handshake();

    // Give the server's close() a moment to actually land on the loopback interface before publishing —
    // the same "let the RST/FIN propagate" allowance tests/drivers/modbus_tcp_driver.cpp's own
    // reconnect-after-loss scenario relies on (there, via the driver's own bounded retry loop instead).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<std::byte> payload{std::byte{'x'}, std::byte{'y'}};

    // Poll for the first failing publish() rather than asserting the very first post-kill call fails: a
    // local socket write can succeed (kernel-buffered) before the peer's FIN/RST is actually observed —
    // see the file banner's scenario (c) note. Bounded well under the sink's own 5s connect timeout, and
    // each individual call must still never hang.
    bool saw_failure = false;
    const auto poll_start = std::chrono::steady_clock::now();
    const auto poll_deadline = poll_start + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < poll_deadline) {
        const auto call_start = std::chrono::steady_clock::now();
        const bool published = sink.publish("sensor/line1/temp", payload, /*qos=*/1);
        const auto call_elapsed = std::chrono::steady_clock::now() - call_start;
        ok &= call_elapsed < std::chrono::seconds(4);  // each individual call, never hung
        if (!published) { saw_failure = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ok &= saw_failure;
    if (!saw_failure) std::printf("publish_after_killed: publish() never failed within the poll deadline\n");

    // Once failing, a further publish() must also fail fast (fail-fast posture, no reconnect loop)
    // without attempting any new I/O — still bounded/prompt.
    const auto start2 = std::chrono::steady_clock::now();
    const bool published2 = sink.publish("sensor/line1/temp", payload, /*qos=*/1);
    const auto elapsed2 = std::chrono::steady_clock::now() - start2;
    ok &= !published2;
    ok &= elapsed2 < std::chrono::milliseconds(500);  // fail-fast: no I/O attempted at all

    sink.close();
    server.stop();
    if (!ok) std::printf("publish_after_killed: assertion failed\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    const bool a_ok = test_connect_and_publish();
    ok &= a_ok;
    std::printf("[connect_and_publish] %s\n", a_ok ? "ok" : "FAIL");

    const bool b_ok = test_connect_refused();
    ok &= b_ok;
    std::printf("[connect_refused] %s\n", b_ok ? "ok" : "FAIL");

    const bool c_ok = test_publish_after_connection_killed();
    ok &= c_ok;
    std::printf("[publish_after_connection_killed] %s\n", c_ok ? "ok" : "FAIL");

    std::printf("rabbitmq_bridge_sink: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
