// AeroEdge M9.2 LOAD-BEARING gate (006 §6.1, 018) — proves Runtime::deploy() actually drives a PULL
// driver now, not just push (runtime_deploy.cpp's GeneratorDriver-only proof). Before this milestone,
// runtime.hpp's driver-ingestion path unconditionally spawned a producer thread calling `drv->run(...)`
// — the ENTIRE spec-018 driver family (ModbusTcpDriver, ModbusRtuDriver, OpcUaDriver) has `run()` as a
// hard Unsupported (006 §6.1: they are poll()-only), so despite being registered in the driver_reg
// factory, none of them could actually be deployed through a real Application before this fix. Only
// each driver's own test harness ever called poll() (via a throwaway per-call StreamActivation).
//
// This test deploys a REAL Application via `Runtime::deploy_json()` with `driver.type_id:
// "aero.driver.modbus_tcp"` against a hand-rolled fake Modbus-TCP server (same primitives every other
// socket test in this tree uses), then asserts `frames_processed` climbs over real (bounded, wall-clock)
// time — proving the new poll-timer lane (`Deployment::poller`, runtime.hpp) actually ticks
// `driver.poll(sink)` on the configured `rate_hz` cadence and delivers frames into the FlowActor, end to
// end, exactly the way the push lane already did for GeneratorDriver. Also proves clean teardown: undeploy
// joins the poller thread without hanging.
//
// UNLIKE tests/drivers/modbus_tcp_driver.cpp's own FakeModbusServer (which deliberately drops the
// connection after ONE request, to exercise that test's own reconnect scenario), this fake keeps each
// accepted connection alive and answers MULTIPLE sequential requests on it — the realistic shape for a
// driver that opens once and polls repeatedly, which is exactly what this test needs to observe.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "aero/pal/poll.hpp"
#include "aero/runtime/runtime.hpp"
#include "pal/net.hpp"

namespace {

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

// Keeps each connection alive, answering FC03 read-holding-register requests repeatedly — see file
// banner for why this differs from modbus_tcp_driver.cpp's own one-shot fake.
struct FakeModbusServer {
    quark::pal::fd_t listen_fd = quark::pal::invalid_fd;
    std::uint16_t port = 0;
    std::atomic<bool> running{false};
    std::thread thr;
    std::uint16_t canned_value = 42;

    bool start() {
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
            serve_conn(*a);  // stays here, answering requests, until the connection drops or we stop
            quark::pal::close_fd(*a);
        }
    }

    void serve_conn(quark::pal::fd_t conn) {
        while (running.load(std::memory_order_acquire)) {
            std::array<std::byte, 12> req{};
            if (!recv_exact(conn, req.data(), req.size(), running)) return;  // peer closed -> back to accept

            const std::uint16_t txn = get_u16_be(&req[0]);
            const std::uint8_t unit = std::to_integer<std::uint8_t>(req[6]);
            const std::uint8_t fc = std::to_integer<std::uint8_t>(req[7]);
            const std::uint16_t count = get_u16_be(&req[10]);
            if (fc != 0x03) continue;  // only FC03 needed for this test

            const auto byte_count = static_cast<std::uint8_t>(count * 2);
            std::vector<std::byte> resp(static_cast<std::size_t>(9) + byte_count);
            put_u16_be(&resp[0], txn);
            put_u16_be(&resp[2], 0x0000);
            put_u16_be(&resp[4], static_cast<std::uint16_t>(3 + byte_count));
            resp[6] = static_cast<std::byte>(unit);
            resp[7] = static_cast<std::byte>(fc);
            resp[8] = static_cast<std::byte>(byte_count);
            for (std::uint16_t i = 0; i < count; ++i) put_u16_be(&resp[9 + 2 * i], canned_value);
            if (!send_all(conn, resp.data(), resp.size())) return;
        }
    }
};

}  // namespace

int main() {
    FakeModbusServer server;
    if (!server.start()) {
        std::printf("server start failed\nFAIL\n");
        return 1;
    }

    // rate_hz=40 -> a 25ms poll interval (see runtime.hpp's driver-ingestion poll lane) — fast enough
    // to observe several ticks within a bounded few-second test window.
    const std::string app = R"({
      "name": "poll_driver_wiring",
      "version": "0.1.0",
      "actor": { "kind": "edge", "key": 11 },
      "flow": [
        { "type_id": "aero.source.decode" },
        { "type_id": "aero.transform.scale", "config": { "factor": 2 } },
        { "type_id": "aero.output.sum" }
      ],
      "driver": { "type_id": "aero.driver.modbus_tcp", "config": {
        "host": "127.0.0.1", "port": )" + std::to_string(server.port) + R"(,
        "unit_id": 1, "start_address": 0, "register_count": 1, "rate_hz": 40
      } }
    })";

    aero::runtime::Runtime rt;
    auto r = rt.deploy_json(app);
    if (!r) {
        std::printf("deploy failed: %s\nFAIL\n", r.error().c_str());
        server.stop();
        return 1;
    }

    // has_driver must be true immediately — proves the poll_driven branch (not the push branch, which
    // would spawn a producer that immediately returns Unsupported for this driver) actually engaged.
    bool ok = rt.status().value("has_driver", false);
    if (!ok) std::printf("has_driver: expected true\n");

    // Bounded wait: the poller should deliver several frames within a few seconds at 40 Hz.
    long frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && frames < 5) {
        frames = rt.status().value("frames_processed", 0L);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ok &= frames >= 5;
    std::printf("frames_processed  : %ld  (expected >= 5 within 5s at 40 Hz)\n", frames);

    auto u = rt.undeploy();
    const bool torn_down = u.has_value() && !rt.status().value("deployed", true);
    ok &= torn_down;
    std::printf("undeploy + teardown: %s\n", torn_down ? "clean" : "BAD");

    server.stop();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
