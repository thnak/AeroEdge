// AeroEdge M9a gate (018 §Multi-protocol southbound) — ModbusTcpDriver (drivers/modbus_tcp_driver.hpp)
// over a hand-rolled fake Modbus-TCP server (same quark::pal::net + aero::pal::poll primitives every
// other socket test in this tree already uses — tcp_transport.cpp/native_broker.cpp/bridge.cpp). Kept a
// focused unit/integration test of the driver + ModbusDecodeNode pair (NOT the full Runtime): frames are
// drained straight off a StreamActivation<Frame> ring (the stream_ingest.cpp pattern) into a bare
// ProcessingContext, never through a FlowActor/Runtime deploy.
//
// ONE StreamActivation PER poll() CALL (deliberate, not an oversight): a Quark 024 StreamActivation binds
// exactly ONE producer for its whole lifetime (StreamActivation::bind_producer — a second bind is a typed
// 007 error, and nothing un-binds it). `IDriver::poll()` takes its `StreamSink<Frame>` BY VALUE with no
// way to hand it back, so a single StreamSink is inherently good for exactly one poll() call. This test's
// `poll_once()` helper therefore stands up a small, cheap, throwaway StreamActivation per call — the
// production wiring for a poll-driven (as opposed to run()-driven) driver is explicitly out of this
// task's scope (left to "whatever caller drives poll() externally"), so this is a faithful, minimal way
// to exercise the driver's actual contract without inventing that wiring here.
//
// Covers:
//   (1) happy path: driver.poll() gets back known register values over a real socket; ModbusDecodeNode
//       (nodes/compute_nodes.hpp, reused verbatim — decode is explicitly NOT this driver's job) decodes
//       ctx.payload into tags that match the canned values.
//   (2) an oversized register_count (count*2 > kMaxFramePayload) is rejected at open(), never a silently
//       truncated frame.
//   (3) connection loss mid-session: the fake server closes each connection right after one response (a
//       natural way to force a drop without extra synchronization) — the driver's NEXT poll() against
//       the now-dead fd fails cleanly (Error, no crash), and a LATER poll() (after the driver's bounded
//       backoff) reconnects against a freshly accepted connection and succeeds.
// Deterministic-enough, exit-code-gated (0 = pass); bounded polling/retries; clean shutdown.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "aero/drivers/modbus_tcp_driver.hpp"
#include "aero/nodes/compute_nodes.hpp"
#include "aero/pal/poll.hpp"
#include "aero/sdk/processing_context.hpp"
#include "pal/net.hpp"
#include "quark/core/stream_activation.hpp"

using aero::Frame;
using aero::drivers::ModbusTcpDriver;

namespace {

// ===== byte helpers (big-endian, mirrors the driver's own private ones — kept separate/duplicated on
// purpose: this is the TEST's independent fake server, not code under test) ===========================
void put_u16_be(std::byte* p, std::uint16_t v) {
    p[0] = static_cast<std::byte>((v >> 8) & 0xFF);
    p[1] = static_cast<std::byte>(v & 0xFF);
}
std::uint16_t get_u16_be(const std::byte* p) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(p[0]) << 8) |
                                       std::to_integer<std::uint16_t>(p[1]));
}

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

// A fake Modbus-TCP server: accepts one connection at a time, serves EXACTLY ONE FC03 request on it,
// then closes the connection and goes back to accepting — a natural, synchronization-free way to force
// the "connection lost mid-session" scenario test (3) needs, while also being the server tests (1)/(2)
// dial against (a single request per connection is all either of those needs too).
struct FakeModbusServer {
    quark::pal::fd_t listen_fd = quark::pal::invalid_fd;
    std::uint16_t port = 0;
    std::atomic<bool> running{false};
    std::thread thr;
    std::vector<std::uint16_t> registers;  // canned holding-register bank

    bool start() {
        registers.resize(200);
        for (std::size_t i = 0; i < registers.size(); ++i)
            registers[i] = static_cast<std::uint16_t>(0xA000 + i);

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
            if (!ready || !*ready) continue;  // timeout (gives `running` a chance to flip) or poll error
            auto a = quark::pal::accept_one(listen_fd);
            if (!a) continue;
            serve_one(*a);
            quark::pal::close_fd(*a);  // drop right after — see struct banner
        }
    }

    // Read exactly one FC03 request (12 bytes: MBAP7 + fc1 + addr2 + count2), reply with the canned
    // register slice, then return (caller closes the connection).
    void serve_one(quark::pal::fd_t conn) {
        std::array<std::byte, 12> req{};
        if (!recv_exact(conn, req.data(), req.size(), running)) return;

        const std::uint16_t txn = get_u16_be(&req[0]);
        const std::uint8_t unit = std::to_integer<std::uint8_t>(req[6]);
        const std::uint8_t fc = std::to_integer<std::uint8_t>(req[7]);
        const std::uint16_t start = get_u16_be(&req[8]);
        const std::uint16_t count = get_u16_be(&req[10]);
        if (fc != 0x03) return;

        const std::uint8_t byte_count = static_cast<std::uint8_t>(count * 2);
        std::vector<std::byte> resp(static_cast<std::size_t>(9) + byte_count);
        put_u16_be(&resp[0], txn);
        put_u16_be(&resp[2], 0x0000);
        put_u16_be(&resp[4], static_cast<std::uint16_t>(3 + byte_count));  // unit(1)+fc(1)+bytecount(1)+data
        resp[6] = static_cast<std::byte>(unit);
        resp[7] = static_cast<std::byte>(0x03);
        resp[8] = static_cast<std::byte>(byte_count);
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::uint16_t v = (start + i) < registers.size() ? registers[start + i] : 0;
            put_u16_be(&resp[9 + 2 * i], v);
        }
        (void)send_all(conn, resp.data(), resp.size());
    }
};

// Drain exactly one Frame off a stream ring (bounded wait) — the stream_ingest.cpp pattern, minus the
// flow/actor: this test decodes straight into a bare ProcessingContext.
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

// One poll() call against a FRESH, throwaway StreamActivation — see file banner for why one-per-call
// (a StreamActivation's single-producer bind is a lifetime commitment; poll()'s StreamSink-by-value
// contract has no way to hand a token back for reuse across calls).
PollOutcome poll_once(ModbusTcpDriver& driver) {
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

// ---- (1) happy path: poll() -> ModbusDecodeNode -> decoded tags match the canned values -------------
bool test_happy_path() {
    FakeModbusServer server;
    if (!server.start()) { std::printf("happy_path: server start failed\n"); return false; }

    ModbusTcpDriver driver("127.0.0.1", server.port, /*unit*/ 1, /*start*/ 2, /*count*/ 4);
    aero::DriverConfig cfg{};
    bool ok = driver.open(cfg) == aero::DriverStatus::Ok;

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Ok;
    ok &= outcome.frame.has_value();
    if (outcome.frame) {
        ok &= outcome.frame->payload_len == 8;  // 4 registers * 2 bytes

        aero::ProcessingContext ctx;
        ctx.reset(&*outcome.frame);
        aero::nodes::ModbusDecodeNode decode;
        ok &= decode.process(ctx) == aero::NodeResult::Continue;
        ok &= ctx.tags.size() == 4;
        const std::uint16_t expected[4] = {0xA002, 0xA003, 0xA004, 0xA005};
        for (std::size_t i = 0; i < 4 && i < ctx.tags.size(); ++i) {
            ok &= ctx.tags[i].value == static_cast<double>(expected[i]);
        }
    }

    driver.close();
    server.stop();
    if (!ok) std::printf("happy_path: assertion failed\n");
    return ok;
}

// ---- (2) an oversized register_count is rejected cleanly at open() -----------------------------------
bool test_oversized_rejected() {
    // count*2 must exceed kMaxFramePayload; the server doesn't even need to be reachable — open() must
    // reject on the config check alone, before ever dialing.
    const std::uint16_t count = static_cast<std::uint16_t>(aero::kMaxFramePayload / 2 + 1);
    ModbusTcpDriver driver("127.0.0.1", /*port*/ 1, /*unit*/ 1, /*start*/ 0, count);
    aero::DriverConfig cfg{};
    const bool ok = driver.open(cfg) == aero::DriverStatus::Error;
    if (!ok) std::printf("oversized_rejected: open() did not reject count=%u\n", count);
    return ok;
}

// ---- (3) connection loss mid-session: the next poll() reconnects, never wedges/crashes ---------------
bool test_reconnect_after_loss() {
    FakeModbusServer server;
    if (!server.start()) { std::printf("reconnect: server start failed\n"); return false; }

    ModbusTcpDriver driver("127.0.0.1", server.port, /*unit*/ 1, /*start*/ 0, /*count*/ 2);
    aero::DriverConfig cfg{};
    bool ok = driver.open(cfg) == aero::DriverStatus::Ok;

    // First poll: served by the server's first accepted connection -> Ok. The fake server closes that
    // connection right after responding (struct banner), so the driver's fd is now stale server-side.
    const auto first = poll_once(driver);
    ok &= first.status == aero::DriverStatus::Ok && first.frame.has_value();

    // Second poll: reuses the now-server-closed fd -> must fail CLEANLY (Error), never crash/hang. This
    // is the "connection lost" detection; the driver closes its own fd and schedules a backoff retry.
    const auto lost = poll_once(driver);
    ok &= lost.status != aero::DriverStatus::Ok;

    // Bounded retry loop: eventually (after the driver's backoff elapses) poll() reconnects against a
    // freshly accepted connection on the SAME server/port and succeeds.
    bool reconnected = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !reconnected) {
        const auto r = poll_once(driver);
        if (r.status == aero::DriverStatus::Ok && r.frame.has_value()) {
            reconnected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

    std::printf("modbus_tcp_driver: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
