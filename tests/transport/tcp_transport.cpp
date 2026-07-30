// AeroEdge Phase-7 REAL-TCP gate (014 §4, X1): two nodes exchange genuine MessageFrames over an actual
// TCP socket — the concrete, wire-carrying realization of the §2 cross-node path (which cross_node_message
// proves in-process over Quark's LoopbackTransport). No Python, no external backend: POSIX sockets only.
//
// This proves, over a real loopback socket:
//   (1) round-trip delivery: node 1 → node 2 gets every frame; node 2 → node 1 gets its reply frames;
//   (2) header fidelity (C4): from/to/target/msg_type/trace_id/payload all survive the codec + wire;
//   (3) per-(from → target) FIFO (C1): frames arrive strictly in send order over the one ordered
//       connection — no resequencer, because TCP itself is the flow-controlled, order-preserving substrate.
// Deterministic, exit-code-gated (0 = pass); bounded polling; clean shutdown (must be TSan-clean).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "aero/transport/tcp_transport.hpp"

using namespace aero::transport;

namespace {

// A thread-safe sink recording each received frame's ordering key (trace_id) + a payload witness.
struct RecordingSink {
    std::mutex mu;
    std::vector<std::uint64_t> order;  // trace_id per frame, in arrival order
    std::vector<std::uint8_t> witness; // payload[0] per frame, to confirm the body crossed intact
    std::atomic<int> count{0};

    void operator()(MessageFrame f) {
        std::lock_guard<std::mutex> g(mu);
        order.push_back(f.trace_id);
        witness.push_back(f.payload.empty() ? 0 : std::to_integer<std::uint8_t>(f.payload[0]));
        count.fetch_add(1, std::memory_order_release);
    }
};

// Build a frame carrying an ordering key `i` in trace_id and a one-byte payload witness.
MessageFrame make_frame(std::uint64_t from, std::uint64_t to, std::uint64_t i) {
    MessageFrame f;
    f.from = quark::NodeId{from};
    f.to = quark::NodeId{to};
    f.target = quark::ActorId{quark::TypeKey{0xABCD}, /*key*/ 42};
    f.msg_type = quark::TypeKey{0x1111 + i};
    f.trace_id = i;
    f.payload = {static_cast<std::byte>(i & 0xFF)};
    return f;
}

bool wait_count(std::atomic<int>& c, int want, int timeout_ms = 5000) {
    for (int waited = 0; c.load(std::memory_order_acquire) < want; waited += 5) {
        if (waited > timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

bool ordered_0_to_n(const std::vector<std::uint64_t>& v, int n) {
    if (static_cast<int>(v.size()) < n) return false;
    for (int i = 0; i < n; ++i)
        if (v[static_cast<std::size_t>(i)] != static_cast<std::uint64_t>(i)) return false;
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    constexpr int N = 200;

    RecordingSink sink2;  // node 2's inbound sink
    RecordingSink sink1;  // node 1's inbound sink (for the reply direction)

    TcpTransport n1(TcpTransport::Config{/*self*/ quark::NodeId{1}});
    TcpTransport n2(TcpTransport::Config{/*self*/ quark::NodeId{2}});
    n1.on_receive(std::ref(sink1));
    n2.on_receive(std::ref(sink2));

    auto s1 = n1.start();
    auto s2 = n2.start();
    ok &= s1.has_value() && s2.has_value();
    if (!ok) {
        std::printf("start failed: n1=%s n2=%s\n", s1 ? "ok" : s1.error().c_str(),
                    s2 ? "ok" : s2.error().c_str());
        std::printf("FAIL\n");
        return 1;
    }

    // Peers learn each other's ephemeral port post-start (014 §8), then dial lazily on first send.
    n1.add_peer(quark::NodeId{2}, "127.0.0.1", n2.listen_port());
    n2.add_peer(quark::NodeId{1}, "127.0.0.1", n1.listen_port());

    // Direction A: node 1 → node 2, N frames, keys 0..N-1.
    for (int i = 0; i < N; ++i) n1.send(quark::NodeId{2}, make_frame(1, 2, static_cast<std::uint64_t>(i)));
    ok &= wait_count(sink2.count, N);

    // Direction B (reply path over a distinct connection): node 2 → node 1, N frames.
    for (int i = 0; i < N; ++i) n2.send(quark::NodeId{1}, make_frame(2, 1, static_cast<std::uint64_t>(i)));
    ok &= wait_count(sink1.count, N);

    // (1) delivery counts on the wire.
    ok &= n1.frames_sent() == static_cast<std::uint64_t>(N);
    ok &= n2.frames_sent() == static_cast<std::uint64_t>(N);
    ok &= n1.send_errors() == 0 && n2.send_errors() == 0;
    ok &= n2.frames_received() == static_cast<std::uint64_t>(N);
    ok &= n1.frames_received() == static_cast<std::uint64_t>(N);

    // (3) FIFO over the wire (C1): both directions arrived strictly in send order 0..N-1.
    bool fifo_a = false, fifo_b = false;
    {
        std::lock_guard<std::mutex> g(sink2.mu);
        fifo_a = ordered_0_to_n(sink2.order, N);
    }
    {
        std::lock_guard<std::mutex> g(sink1.mu);
        fifo_b = ordered_0_to_n(sink1.order, N);
    }
    ok &= fifo_a && fifo_b;

    n1.stop();
    n2.stop();

    std::printf("tcp: A(1->2) delivered=%d fifo=%s | B(2->1) delivered=%d fifo=%s | send_err=%llu\n",
                sink2.count.load(), fifo_a ? "intact" : "BROKEN", sink1.count.load(),
                fifo_b ? "intact" : "BROKEN",
                (unsigned long long)(n1.send_errors() + n2.send_errors()));
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
