// AeroEdge 014 §5 gate — MqttClientTransport's QoS-1 sender-side retransmit-on-missing-PUBACK
// (mqtt_client_transport.hpp, closing that file's former TODO). Deliberately NOT built on the real amqtt
// broker tests/transport/mqtt_transport.cpp uses — a conforming broker always PUBACKs a valid QoS-1
// PUBLISH immediately, so there is no way to make it withhold an ack on demand. Instead, a small
// hand-rolled fake broker (real loopback TCP, background thread — mirrors this tree's other "fake server,
// real socket, deterministic fault injection" fixtures, e.g. tests/drivers/*'s FakeOpcUaServer/
// FakeModbusServer) speaks just enough MQTT 3.1.1 server-side protocol (CONNACK, SUBACK, and a
// caller-supplied policy for whether/when to PUBACK an inbound PUBLISH) to control exactly when an ack
// does or doesn't arrive.
//
// Config's new puback_timeout_ms/max_publish_retries/retry_check_interval_ms (mqtt_transport.hpp) are set
// small in both tests below — trading the real ~15s default give-up window for a fast, deterministic one,
// the same way tests elsewhere in this tree tune down other production timeouts/backoffs for CI speed.
//
// Proves:
//   (1) a PUBLISH whose FIRST PUBACK is withheld gets retransmitted with the DUP flag set (MQTT 3.1.1
//       §3.3.1.1) — proven by the broker only acking the SECOND arrival, which it also asserts carried
//       DUP — and the retransmit succeeds: publish_retransmits() >= 1, send_errors() == 0.
//   (2) a PUBLISH that NEVER gets acked is retried EXACTLY max_publish_retries times (1 initial send + N
//       retries, no more, no fewer) and then abandoned: publish_gaveup() == 1, send_errors() == 1, and the
//       broker observed exactly 1 + N total arrivals of it.
// Deterministic, exit-code-gated (0 = pass); bounded polling; clean shutdown.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

#include "aero/transport/mqtt_client_transport.hpp"
#include "aero/transport/mqtt_codec.hpp"

using namespace aero::transport;
namespace mqtt = aero::transport::mqtt;

namespace {

// A single client connection's worth of MQTT 3.1.1 SERVER-side protocol — just enough to satisfy
// MqttClientTransport::start() (CONNECT->CONNACK, SUBSCRIBE->SUBACK) and then observe/selectively-ack
// inbound QoS-1 PUBLISHes. `should_ack(total_seen, dup)` decides whether THIS arrival gets a PUBACK;
// `total_seen` counts every PUBLISH arrival (1-based) regardless of pid — both tests below only ever have
// ONE logical in-flight PUBLISH at a time, so arrival count alone unambiguously identifies "first attempt"
// vs. "a retry".
class FakeQos1Broker {
public:
    ~FakeQos1Broker() { stop(); }

    using AckPolicy = std::function<bool(int total_seen, bool dup)>;

    [[nodiscard]] bool start(AckPolicy policy) {
        policy_ = std::move(policy);
        auto lfd = quark::pal::tcp_listen(/*addr=*/0, /*port=*/0, /*backlog=*/1);
        if (!lfd) return false;
        listen_fd_ = *lfd;
        auto p = quark::pal::local_port(listen_fd_);
        if (!p) return false;
        port_ = *p;

        running_.store(true, std::memory_order_release);
        thr_ = std::thread([this] { run(); });
        return true;
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] int publishes_seen() const noexcept { return publishes_seen_.load(std::memory_order_acquire); }
    [[nodiscard]] bool dup_seen_on_any_ack() const noexcept {
        return dup_seen_on_any_ack_.load(std::memory_order_acquire);
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (client_fd_ != quark::pal::invalid_fd) quark::pal::close_fd(client_fd_);
        if (listen_fd_ != quark::pal::invalid_fd) quark::pal::close_fd(listen_fd_);
        if (thr_.joinable()) thr_.join();
    }

private:
    void run() {
        quark::pal::fd_t cfd = quark::pal::invalid_fd;
        const auto accept_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < accept_deadline) {
            const auto ready = aero::pal::wait_readable(listen_fd_, 100);
            if (!ready || !*ready) continue;
            auto c = quark::pal::accept_one(listen_fd_);
            if (c) { cfd = *c; break; }
        }
        if (cfd == quark::pal::invalid_fd) return;
        client_fd_ = cfd;

        // CONNECT -> CONNACK(0,0).
        auto connect_pkt = mqtt::read_packet(cfd, running_);
        if (!connect_pkt) return;
        (void)mqtt::write_packet(cfd, std::byte{0x20}, std::vector<std::byte>{std::byte{0}, std::byte{0}});

        // SUBSCRIBE -> SUBACK (granted QoS 1 for whatever single filter was requested).
        auto sub_pkt = mqtt::read_packet(cfd, running_);
        if (!sub_pkt) return;
        std::vector<std::byte> suback_vh;
        if (sub_pkt->body.size() >= 2) {
            suback_vh.push_back(sub_pkt->body[0]);
            suback_vh.push_back(sub_pkt->body[1]);  // echo the packet id
        }
        suback_vh.push_back(std::byte{0x01});
        (void)mqtt::write_packet(cfd, std::byte{0x90}, suback_vh);

        // PUBLISH loop: parse just enough to find the packet id, apply the ack policy.
        while (running_.load(std::memory_order_acquire)) {
            auto pkt = mqtt::read_packet(cfd, running_);
            if (!pkt) break;
            const std::uint8_t type = pkt->type_flags & 0xF0;
            if (type != 0x30) continue;  // this fixture only ever expects PUBLISH from the client
            const bool dup = (pkt->type_flags & 0x08) != 0;
            const std::vector<std::byte>& b = pkt->body;
            if (b.size() < 2) continue;
            const std::uint16_t topic_len =
                (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
            std::size_t pos = 2 + topic_len;
            if (pos + 2 > b.size()) continue;
            const std::uint16_t pid = (std::to_integer<std::uint8_t>(b[pos]) << 8) |
                                       std::to_integer<std::uint8_t>(b[pos + 1]);

            const int seen = publishes_seen_.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (policy_(seen, dup)) {
                if (dup) dup_seen_on_any_ack_.store(true, std::memory_order_release);
                std::vector<std::byte> ack;
                mqtt::put_u16_be(ack, pid);
                (void)mqtt::write_packet(cfd, std::byte{0x40}, ack);
            }
        }
    }

    AckPolicy policy_;
    quark::pal::fd_t listen_fd_ = quark::pal::invalid_fd;
    quark::pal::fd_t client_fd_ = quark::pal::invalid_fd;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thr_;
    std::atomic<int> publishes_seen_{0};
    std::atomic<bool> dup_seen_on_any_ack_{false};
};

MessageFrame make_frame(std::uint64_t from, std::uint64_t to) {
    MessageFrame f;
    f.from = quark::NodeId{from};
    f.to = quark::NodeId{to};
    f.target = quark::ActorId{quark::TypeKey{0x5150}, /*key*/ 1};
    f.msg_type = quark::TypeKey{0x2001};
    f.trace_id = 1;
    f.payload = {std::byte{0xAB}};
    return f;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// ---- (1) withheld first PUBACK -> retransmit (with DUP) -> succeeds -------------------------------------
bool test_retransmit_then_succeed() {
    FakeQos1Broker broker;
    // Withhold the ack on the FIRST arrival only; ack every later one (the retry).
    const bool started = broker.start([](int seen, bool /*dup*/) { return seen != 1; });
    if (!started) {
        std::printf("retransmit_then_succeed: broker start failed\n");
        return false;
    }

    MqttTransport::Config cfg;
    cfg.broker_uri = "tcp://127.0.0.1:" + std::to_string(broker.port());
    cfg.puback_timeout_ms = 150;
    cfg.retry_check_interval_ms = 20;
    cfg.max_publish_retries = 3;

    MqttClientTransport client(quark::NodeId{1}, cfg);
    auto s = client.start();
    if (!s) {
        std::printf("retransmit_then_succeed: client start failed: %s\n", s.error().c_str());
        return false;
    }

    client.send(quark::NodeId{2}, make_frame(1, 2));

    bool ok = wait_until([&] { return client.publish_retransmits() >= 1; }, 2000);
    // Give the PUBACK a moment to actually land back at the client after the broker's retry-triggered ack.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ok &= client.publish_retransmits() >= 1;
    ok &= client.publish_gaveup() == 0;
    ok &= client.send_errors() == 0;
    ok &= client.frames_sent() == 1;
    ok &= broker.dup_seen_on_any_ack();  // the acked (2nd) arrival really did carry DUP

    client.stop();
    broker.stop();
    if (!ok) {
        std::printf("retransmit_then_succeed: retransmits=%llu gaveup=%llu errors=%llu dup_seen=%s\n",
                    (unsigned long long)client.publish_retransmits(), (unsigned long long)client.publish_gaveup(),
                    (unsigned long long)client.send_errors(), broker.dup_seen_on_any_ack() ? "yes" : "no");
    }
    return ok;
}

// ---- (2) never acked -> retried exactly max_publish_retries times, then abandoned -----------------------
bool test_gives_up_after_max_retries() {
    FakeQos1Broker broker;
    const bool started = broker.start([](int /*seen*/, bool /*dup*/) { return false; });  // never ack
    if (!started) {
        std::printf("gives_up_after_max_retries: broker start failed\n");
        return false;
    }

    MqttTransport::Config cfg;
    cfg.broker_uri = "tcp://127.0.0.1:" + std::to_string(broker.port());
    cfg.puback_timeout_ms = 100;
    cfg.retry_check_interval_ms = 20;
    cfg.max_publish_retries = 2;

    MqttClientTransport client(quark::NodeId{1}, cfg);
    auto s = client.start();
    if (!s) {
        std::printf("gives_up_after_max_retries: client start failed: %s\n", s.error().c_str());
        return false;
    }

    client.send(quark::NodeId{2}, make_frame(1, 2));

    bool ok = wait_until([&] { return client.publish_gaveup() >= 1; }, 3000);
    // Nothing should retry further once abandoned — settle briefly and re-check the counts are stable.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ok &= client.publish_gaveup() == 1;
    ok &= client.send_errors() == 1;
    // Exactly 1 initial send + max_publish_retries retries reached the broker — no more, no fewer.
    ok &= broker.publishes_seen() == 1 + cfg.max_publish_retries;

    client.stop();
    broker.stop();
    if (!ok) {
        std::printf("gives_up_after_max_retries: gaveup=%llu errors=%llu broker_seen=%d (want %d)\n",
                    (unsigned long long)client.publish_gaveup(), (unsigned long long)client.send_errors(),
                    broker.publishes_seen(), 1 + cfg.max_publish_retries);
    }
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    const bool r1 = test_retransmit_then_succeed();
    ok &= r1;
    std::printf("[retransmit_then_succeed] %s\n", r1 ? "ok" : "FAIL");

    const bool r2 = test_gives_up_after_max_retries();
    ok &= r2;
    std::printf("[gives_up_after_max_retries] %s\n", r2 ? "ok" : "FAIL");

    std::printf("mqtt_transport_retry: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
