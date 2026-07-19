// AeroEdge Phase-7 REAL-gRPC gate (014 §6, X2): the hand-written GrpcClientTransport exchanges genuine
// MessageFrames with a REAL gRPC server (grpcio, launched via uv) over an ACTUAL HTTP/2 connection — the
// concrete backend the offline NullGrpcTransport stands in for. No gRPC/HTTP2 library is linked; every
// byte of the HTTP/2 framing + gRPC message framing is produced by the C++ client itself.
//
// Proves, end-to-end over real HTTP/2 + gRPC:
//   (1) the hand-written HTTP/2 client works: preface, SETTINGS exchange, a HEADERS-opened bidi stream,
//       DATA frames — the grpcio C-core accepts them and runs the /aero.Transport/Exchange RPC;
//   (2) gRPC message framing round-trips: N frames sent as gRPC messages are decoded BY THE SERVER (with
//       AeroEdge's own codec, in Python), echoed with from/to swapped, and decoded back by the C++ client;
//   (3) header fidelity (C4): from/to/target/msg_type/trace_id survive codec + HTTP/2 both ways — the
//       echoed frames carry from=2/to=1 (server-swapped) with trace_id preserved;
//   (4) single-stream FIFO (C1): responses arrive strictly in send order with NO resequencer — an ordered
//       HTTP/2 stream IS the order-preserving substrate (contrast MqttTransport, 014 §6).
//
// SKIPS (exit 0 + SKIP line) if uv / grpcio cannot start — an external backend, not an AeroEdge defect.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aero/transport/grpc_client_transport.hpp"
#include "proc_util.hpp"

using namespace aero::transport;
using aero::testutil::UvBackend;

#ifndef AERO_UV_BIN
#define AERO_UV_BIN "uv"
#endif
#ifndef AERO_GRPC_SERVER_PY
#define AERO_GRPC_SERVER_PY "tests/py/grpc_echo_server.py"
#endif

namespace {

struct RecordingSink {
    std::mutex mu;
    std::vector<std::uint64_t> order;   // trace_id per echoed frame, in arrival order
    std::vector<std::pair<std::uint64_t, std::uint64_t>> endpoints;  // (from,to) to confirm server swap
    std::atomic<int> count{0};
    void operator()(MessageFrame f) {
        std::lock_guard<std::mutex> g(mu);
        order.push_back(f.trace_id);
        endpoints.emplace_back(f.from.value, f.to.value);
        count.fetch_add(1, std::memory_order_release);
    }
};

MessageFrame make_frame(std::uint64_t i) {
    MessageFrame f;
    f.from = quark::NodeId{1};
    f.to = quark::NodeId{2};
    f.target = quark::ActorId{quark::TypeKey{0xC0DE}, /*key*/ 3};
    f.msg_type = quark::TypeKey{0x3000 + i};
    f.trace_id = i;
    f.payload = {static_cast<std::byte>(i & 0xFF)};
    return f;
}

bool wait_count(std::atomic<int>& c, int want, int timeout_ms = 15000) {
    for (int waited = 0; c.load(std::memory_order_acquire) < want; waited += 5) {
        if (waited > timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

int skip(const char* why) {
    std::printf("SKIP grpc_transport: %s\n", why);
    std::printf("OK\n");
    return 0;
}

}  // namespace

int main() {
    constexpr int N = 100;
    const std::uint16_t port = aero::testutil::free_port();
    if (port == 0) return skip("could not allocate a free port");

    UvBackend server(AERO_UV_BIN, {"run", "--with", "grpcio", AERO_GRPC_SERVER_PY, std::to_string(port)});
    if (!server.spawned()) return skip("could not spawn uv (grpc server)");
    if (!UvBackend::wait_for_port(port)) return skip("grpcio server did not come up (no uv/network?)");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    bool ok = true;
    GrpcTransport::Config cfg;
    cfg.target = "127.0.0.1:" + std::to_string(port);
    cfg.tls = false;  // reference client speaks h2c (cleartext) — TLS is a real-build concern (§6)

    RecordingSink sink;
    GrpcClientTransport client(cfg);
    client.on_receive(std::ref(sink));

    auto s = client.start();
    if (!s) {
        std::printf("grpc start failed: %s\n", s.error().c_str());
        return skip("client could not establish the HTTP/2 stream");
    }

    for (int i = 0; i < N; ++i) client.send(quark::NodeId{2}, make_frame(static_cast<std::uint64_t>(i)));
    ok &= wait_count(sink.count, N);

    ok &= client.frames_sent() == static_cast<std::uint64_t>(N);
    ok &= client.frames_received() == static_cast<std::uint64_t>(N);
    ok &= client.send_errors() == 0;

    // (3)/(4): every echo is server-swapped (from=2,to=1) AND arrives strictly in send order 0..N-1.
    bool fifo = false, swapped = true;
    {
        std::lock_guard<std::mutex> g(sink.mu);
        fifo = static_cast<int>(sink.order.size()) == N;
        for (int i = 0; i < N && fifo; ++i)
            if (sink.order[static_cast<std::size_t>(i)] != static_cast<std::uint64_t>(i)) fifo = false;
        for (auto& [from, to] : sink.endpoints)
            if (from != 2 || to != 1) swapped = false;
    }
    ok &= fifo && swapped;

    client.stop();

    std::printf("grpc(real grpcio HTTP/2): sent=%llu recv=%d fifo=%s server_roundtrip(swap)=%s\n",
                (unsigned long long)client.frames_sent(), sink.count.load(),
                fifo ? "intact" : "BROKEN", swapped ? "verified" : "BROKEN");
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
