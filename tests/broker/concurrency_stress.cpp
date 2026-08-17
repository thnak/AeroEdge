// 017 Phase 1 gap #1 / Phase 2 Experiment C (redesign doc 017-Native-Broker-Performance-Redesign.md
// §1.5 gap #1, §2.3 Experiment C): does a concurrent-publish-vs-teardown race already exist in the
// CURRENT SHIPPED `NativeBroker` (include/aero/broker/native_broker.hpp), independent of any redesign?
//
// Every existing tests/broker/*.cpp test *avoids* this race deliberately (see native_broker.cpp's
// test_persistent_session, which sleeps 150ms specifically so a publish never overlaps a session mid
// teardown). This file does the opposite on purpose: it never sleeps to dodge the race, it runs
// straight at it, continuously, for the whole test.
//
// Shape: ONE NativeBroker instance. Three kinds of concurrent activity share it for a sustained,
// time-bounded window (no artificial "just wait until it's safe" sleeps anywhere below):
//   1. Several publisher threads tight-loop QoS-1 PUBLISH to a shared topic.
//   2. A separate teardown thread repeatedly connects a FRESH client, SUBSCRIBEs to that same topic,
//      then abruptly closes the socket with NO clean DISCONNECT — forcing `teardown_session`'s
//      non-clean-disconnect path (Last-Will-style abrupt end, exactly TestClient::close()'s contract,
//      already used this way by native_broker.cpp's test_will) over and over, racing live PUBLISHes.
//   3. Several STEADY subscriber clients — connected once up front and never torn down — sit
//      subscribed to the shared topic the entire time and must keep receiving messages throughout.
//
// What's under test is exactly what the redesign doc's §2.3 Experiment C asks for: the broker's own
// liveness (never hangs, never crashes) and the steady, still-connected clients' correctness (messages
// keep arriving all the way to the end of the stress window) — NOT the torn-down clients' own
// experience, which is allowed to be whatever a fresh short-lived connection experiences.
//
// Deterministic, exit-code-gated (0 = pass). This is meant to be PERMANENT coverage regardless of what
// Phase 3 of 017 decides — it closes a real, previously-untested gap either way.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aero/broker/native_broker.hpp"

namespace mqtt = aero::transport::mqtt;
using aero::broker::Config;
using aero::broker::NativeBroker;

namespace {

// Minimal hand-rolled MQTT client, same shape/primitives as tests/broker/native_broker.cpp's TestClient
// (this file intentionally keeps its own copy rather than sharing a header — matches this tree's
// existing precedent of every tests/broker/*.cpp file owning its own TestClient, e.g. broker_cluster.cpp).
// Only the subset this stress test actually needs (v4 CONNECT, SUBSCRIBE, QoS-1 PUBLISH, abrupt close).
class TestClient {
public:
    ~TestClient() { close(); }

    [[nodiscard]] bool connect(std::uint16_t port, const std::string& client_id, int timeout_ms = 2000) {
        auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
        if (!fd) return false;
        fd_ = *fd;
        const auto writable = aero::pal::wait_writable(fd_, timeout_ms);
        if (!writable || !*writable || !quark::pal::connect_result(fd_)) return false;

        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(std::byte{0x04});  // protocol level 4 == MQTT 3.1.1
        vh.push_back(std::byte{0x02});  // clean session, no will/user/pass
        mqtt::put_u16_be(vh, /*keep_alive_s=*/60);
        mqtt::put_str(vh, client_id);
        if (!mqtt::write_packet(fd_, std::byte{0x10}, vh)) return false;

        auto ack = wait_for(0x20, timeout_ms);
        return ack.has_value() && ack->body.size() >= 2 && std::to_integer<std::uint8_t>(ack->body[1]) == 0;
    }

    [[nodiscard]] bool subscribe(const std::string& filter, std::uint8_t qos, int timeout_ms = 2000) {
        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, next_id());
        mqtt::put_str(vh, filter);
        vh.push_back(static_cast<std::byte>(qos));
        if (!mqtt::write_packet(fd_, std::byte{0x82}, vh)) return false;
        return wait_for(0x90, timeout_ms).has_value();
    }

    [[nodiscard]] bool publish(const std::string& topic, const std::string& payload, std::uint8_t qos,
                              bool retain, int timeout_ms = 2000) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        std::uint8_t flags = static_cast<std::uint8_t>(qos << 1);
        if (retain) flags |= 0x01;
        if (!mqtt::write_packet(fd_, static_cast<std::byte>(0x30 | flags), vh)) return false;
        return qos == 0 || wait_for(0x40, timeout_ms).has_value();
    }

    // Waits for an inbound PUBLISH; PUBACKs it if QoS 1. Returns (topic, payload), or nullopt on timeout
    // (the expected, non-error outcome when nothing has arrived yet within `timeout_ms`).
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> wait_publish(int timeout_ms) {
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

    // Just closes the fd — deliberately NO DISCONNECT packet sent first. From the broker's point of view
    // this is indistinguishable from a crashed/reset client (3.1.1 §3.1.2.5 / §3.14) and is exactly what
    // drives `teardown_session`'s non-clean-disconnect path — the same contract native_broker.cpp's
    // test_will relies on ("dying.close(); // no DISCONNECT sent").
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
            if (!pkt) break;  // peer closed / running_ flipped false / socket error
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
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
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

}  // namespace

int main() {
    bool ok = true;

    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    auto started = broker.start();
    if (!started.has_value()) {
        std::printf("broker start failed: %s\n", started.error().c_str());
        std::printf("FAIL\n");
        return 1;
    }
    const std::uint16_t port = broker.listen_port();

    // Sustained-load window. No round cap — a wall-clock bound is what the redesign doc's Experiment C
    // asks for ("time-bounded loop of at least 10-15 seconds of sustained concurrent load"); this test
    // runs 12s of it inside one process invocation. (Round counts are still measured and reported below
    // purely for diagnostics — they end up far above the 200-500 floor anyway on any real machine.)
    constexpr auto kStressDuration = std::chrono::seconds(12);
    constexpr int kNumSteadySubscribers = 3;
    constexpr int kNumPublishers = 3;
    const std::string kTopic = "stress/concurrent/topic";

    // --- steady subscribers: connected once, subscribed once, NEVER torn down for the whole run --------
    // These are the clients under correctness test: they must keep receiving PUBLISHes on `kTopic`
    // throughout the entire stress window, concurrently with the teardown thread hammering fresh
    // connect/subscribe/abrupt-close cycles against the SAME broker and SAME topic.
    std::array<std::unique_ptr<TestClient>, kNumSteadySubscribers> steady_clients;
    for (int i = 0; i < kNumSteadySubscribers; ++i) {
        steady_clients[i] = std::make_unique<TestClient>();
        ok &= steady_clients[i]->connect(port, "steady-sub-" + std::to_string(i));
        ok &= steady_clients[i]->subscribe(kTopic, /*qos=*/1);
    }
    if (!ok) {
        std::printf("failed to establish steady subscribers before the stress phase\n");
        std::printf("FAIL\n");
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto ms_since_start = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    };

    std::atomic<bool> stop{false};

    // recv_count[i] / last_recv_ms[i]: how many PUBLISHes steady_clients[i] has drained so far, and the
    // timestamp (ms since stress start) of the most recent one — used below to assert delivery didn't
    // just work early on and then silently stop (which a hang/race would look like).
    std::array<std::atomic<std::uint64_t>, kNumSteadySubscribers> recv_count{};
    std::array<std::atomic<std::int64_t>, kNumSteadySubscribers> last_recv_ms{};
    for (auto& v : last_recv_ms) v.store(-1, std::memory_order_relaxed);

    std::vector<std::thread> steady_threads;
    steady_threads.reserve(kNumSteadySubscribers);
    for (int i = 0; i < kNumSteadySubscribers; ++i) {
        steady_threads.emplace_back([&, i] {
            TestClient* c = steady_clients[i].get();
            while (!stop.load(std::memory_order_acquire)) {
                auto got = c->wait_publish(/*timeout_ms=*/200);  // short: rechecks `stop` promptly
                if (got && got->first == kTopic) {
                    recv_count[i].fetch_add(1, std::memory_order_relaxed);
                    last_recv_ms[i].store(ms_since_start(), std::memory_order_relaxed);
                }
            }
        });
    }

    // --- publisher threads: tight-loop QoS-1 PUBLISH to the shared topic, no sleeps -----------------
    std::atomic<std::uint64_t> publish_ok_count{0};
    std::atomic<std::uint64_t> publish_fail_count{0};
    std::atomic<bool> publisher_connect_failed{false};
    std::vector<std::thread> pub_threads;
    pub_threads.reserve(kNumPublishers);
    for (int i = 0; i < kNumPublishers; ++i) {
        pub_threads.emplace_back([&, i] {
            TestClient pub;
            if (!pub.connect(port, "stress-pub-" + std::to_string(i))) {
                publisher_connect_failed.store(true, std::memory_order_release);
                return;
            }
            std::uint64_t local_round = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const std::string payload = "p" + std::to_string(i) + "-" + std::to_string(local_round++);
                if (pub.publish(kTopic, payload, /*qos=*/1, /*retain=*/false)) {
                    publish_ok_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    publish_fail_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // --- teardown thread: connect a FRESH client, SUBSCRIBE, then abruptly close (no DISCONNECT) -------
    // This is the exact scenario suspected (never proven — see 017-Native-Broker-Performance-Redesign.md
    // §Background) to have caused the reverted fan-out-pool attempt's intermittent stall: a session
    // tearing down (teardown_session's non-clean-disconnect path) while PUBLISHes are concurrently
    // in-flight to the same topic from other threads.
    std::atomic<std::uint64_t> teardown_rounds{0};
    std::atomic<std::uint64_t> teardown_connect_failures{0};
    std::atomic<std::uint64_t> teardown_subscribe_failures{0};
    std::thread teardown_thread([&] {
        std::uint64_t round = 0;
        while (!stop.load(std::memory_order_acquire)) {
            TestClient victim;
            const std::string cid = "stress-teardown-" + std::to_string(round++);
            if (!victim.connect(port, cid)) {
                teardown_connect_failures.fetch_add(1, std::memory_order_relaxed);
                continue;  // broker still up (asserted independently below) — just retry a fresh round
            }
            if (!victim.subscribe(kTopic, /*qos=*/1)) {
                teardown_subscribe_failures.fetch_add(1, std::memory_order_relaxed);
            }
            victim.close();  // abrupt: no DISCONNECT — forces the non-clean teardown path
            teardown_rounds.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(kStressDuration);
    stop.store(true, std::memory_order_release);

    // Join order matters only for cleanliness here (each thread independently rechecks `stop` and exits
    // within a bounded window — at most one in-flight 2000ms ack wait for the publisher/teardown threads,
    // at most 200ms for the steady-subscriber threads) — not for correctness of what was measured above.
    teardown_thread.join();
    for (auto& t : pub_threads) t.join();
    for (auto& t : steady_threads) t.join();

    // --- post-stress liveness check: the broker must still be fully functional after the storm --------
    // Not just "didn't crash" — proves it can still complete a brand-new CONNECT/SUBSCRIBE/PUBLISH/
    // deliver round trip immediately after 12s of concurrent publish-vs-teardown pressure.
    TestClient post_sub, post_pub;
    ok &= post_sub.connect(port, "post-stress-sub");
    ok &= post_sub.subscribe(kTopic, /*qos=*/1);
    ok &= post_pub.connect(port, "post-stress-pub");
    ok &= post_pub.publish(kTopic, "post-stress-payload", /*qos=*/1, /*retain=*/false);
    auto post_got = post_sub.wait_publish(2000);
    ok &= post_got.has_value() && post_got->first == kTopic && post_got->second == "post-stress-payload";
    post_sub.close();
    post_pub.close();

    for (auto& c : steady_clients) c->close();
    broker.stop();

    // --- assertions ---------------------------------------------------------------------------------
    const std::uint64_t total_pub_attempts = publish_ok_count.load() + publish_fail_count.load();
    const std::uint64_t total_teardown_attempts =
        teardown_rounds.load() + teardown_connect_failures.load();
    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(kStressDuration).count();

    // (a) publishers actually got real sustained throughput (rules out "the whole loop silently stalled
    // from round 1" reading as a false pass) and connected cleanly in the first place.
    ok &= !publisher_connect_failed.load(std::memory_order_acquire);
    ok &= publish_ok_count.load() >= 300;
    // publish() failure here means a QoS-1 PUBACK didn't arrive within its 2000ms timeout — tolerate a
    // small fraction (real scheduler/CI jitter) but a broken/hung broker would fail ~100% of attempts,
    // not a few percent, so this still catches the failure mode this test exists to catch.
    ok &= total_pub_attempts == 0 || publish_fail_count.load() * 20 <= total_pub_attempts;  // <=5%

    // (b) the teardown thread itself made real sustained progress (many abrupt-teardown rounds actually
    // completed against the live broker) rather than stalling out early.
    ok &= teardown_rounds.load() >= 100;
    ok &= total_teardown_attempts == 0 || teardown_connect_failures.load() * 5 <= total_teardown_attempts;  // <=20%

    // (c) THE central claim under test: every steady, never-torn-down subscriber kept receiving messages
    // throughout — both "received plenty" (not just one lucky early message) and "still receiving near
    // the very end of the window" (last delivery within 2s of the stress phase ending) — the second
    // check is what would catch a race that only manifests after some teardown churn has accumulated,
    // where delivery silently stops partway through instead of hanging outright.
    for (int i = 0; i < kNumSteadySubscribers; ++i) {
        const std::uint64_t rc = recv_count[i].load(std::memory_order_relaxed);
        const std::int64_t last_ms = last_recv_ms[i].load(std::memory_order_relaxed);
        ok &= rc >= 50;
        ok &= last_ms >= 0 && last_ms >= (duration_ms - 2000);
        std::printf("steady_sub[%d]: received=%llu last_recv_ms=%lld (window=%lldms)\n", i,
                    static_cast<unsigned long long>(rc), static_cast<long long>(last_ms),
                    static_cast<long long>(duration_ms));
    }

    std::printf(
        "concurrency_stress: publishes ok=%llu fail=%llu | teardown rounds=%llu connect_fail=%llu "
        "subscribe_fail=%llu | post-stress round trip=%s\n",
        static_cast<unsigned long long>(publish_ok_count.load()),
        static_cast<unsigned long long>(publish_fail_count.load()),
        static_cast<unsigned long long>(teardown_rounds.load()),
        static_cast<unsigned long long>(teardown_connect_failures.load()),
        static_cast<unsigned long long>(teardown_subscribe_failures.load()),
        (post_got.has_value() ? "ok" : "FAILED"));

    std::printf("concurrency_stress: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
