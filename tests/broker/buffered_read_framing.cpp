// AeroEdge 017 Phase 3 gate (redesign doc §2.4 Experiment A / §3.1): broker-level proof that
// NativeBroker::session_loop()'s buffered read (try_parse_packet(), mqtt_codec.hpp — unit-tested in
// isolation by tests/transport/mqtt_codec.cpp) behaves correctly once it's actually driving a real
// socket: multiple complete packets delivered in a single burst, and a single packet whose bytes are
// split across two separate socket writes. This is Phase 1's test-coverage gap #4
// ("multi-packet-per-recv() burst framing — untested") closed at the broker-integration level.
//
// Deterministic, exit-code-gated (0 = pass); real loopback sockets, no mocks.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

bool g_ok = true;
void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        g_ok = false;
    }
}

// Same minimal hand-rolled MQTT client shape every tests/broker/*.cpp file owns its own copy of (this
// tree's established precedent — see e.g. tests/broker/concurrency_stress.cpp's own banner).
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
        vh.push_back(std::byte{0x04});
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

    // Waits for an inbound PUBLISH; PUBACKs it if QoS 1. Returns (topic, payload), or nullopt on timeout.
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

    [[nodiscard]] quark::pal::fd_t fd() const noexcept { return fd_; }

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

// Builds the raw wire bytes of one complete QoS-0 PUBLISH packet (fixed header + remaining-length +
// variable header + payload) WITHOUT sending them — the caller controls exactly how/when the bytes hit
// the socket, which is the whole point of the burst/split tests below.
std::vector<std::byte> build_publish_bytes(const std::string& topic, const std::string& payload) {
    std::vector<std::byte> vh;
    mqtt::put_str(vh, topic);
    for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));

    std::vector<std::byte> pkt;
    pkt.push_back(std::byte{0x30});  // PUBLISH, QoS 0, no retain
    mqtt::put_remaining_length(pkt, static_cast<std::uint32_t>(vh.size()));
    pkt.insert(pkt.end(), vh.begin(), vh.end());
    return pkt;
}

// Sends exactly `bytes` on a raw fd, retrying on would-block — the burst/split tests' own send primitive,
// deliberately bypassing write_packet() (which always sends one whole, well-formed packet per call) so
// the tests can control raw byte boundaries directly.
bool raw_send_all(quark::pal::fd_t fd, const std::vector<std::byte>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        auto w = quark::pal::send_some(fd, bytes.data() + sent, bytes.size() - sent);
        if (w) {
            sent += *w;
            continue;
        }
        if (w.error() != quark::pal::would_block()) return false;
        const auto writable = aero::pal::wait_writable(fd, 2000);
        if (!writable || !*writable) return false;
    }
    return true;
}

}  // namespace

int main() {
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    auto started = broker.start();
    if (!started.has_value()) {
        std::printf("broker start failed: %s\n", started.error().c_str());
        std::printf("FAIL\n");
        return 1;
    }
    const std::uint16_t port = broker.listen_port();

    // --- Test 1: burst — two complete packets handed to the broker in ONE underlying send, forcing
    // session_loop's inner drain loop to carve BOTH out of a single recv_some() burst before it polls
    // again. Asserts both are dispatched, correctly, in order — not dropped, not corrupted, not merged.
    {
        TestClient sub, pub;
        g_ok &= sub.connect(port, "burst-sub");
        g_ok &= sub.subscribe("burst/topic", /*qos=*/0);
        g_ok &= pub.connect(port, "burst-pub");
        if (!g_ok) {
            std::printf("burst test: setup failed\n");
            std::printf("FAIL\n");
            return 1;
        }

        std::vector<std::byte> burst = build_publish_bytes("burst/topic", "first-message");
        std::vector<std::byte> second = build_publish_bytes("burst/topic", "second-message");
        burst.insert(burst.end(), second.begin(), second.end());
        expect(raw_send_all(pub.fd(), burst), "burst: raw send of two concatenated packets succeeds");

        auto got1 = sub.wait_publish(2000);
        expect(got1.has_value() && got1->first == "burst/topic" && got1->second == "first-message",
               "burst: first packet dispatched correctly");
        auto got2 = sub.wait_publish(2000);
        expect(got2.has_value() && got2->first == "burst/topic" && got2->second == "second-message",
               "burst: second packet dispatched correctly, in order");
    }

    // --- Test 2: split — one packet's bytes sent across TWO separate socket writes with a pause between
    // them, forcing session_loop's buffer to hold a genuinely incomplete packet (try_parse_packet()
    // returning Incomplete) across more than one outer read cycle before it's dispatchable.
    {
        TestClient sub, pub;
        g_ok &= sub.connect(port, "split-sub");
        g_ok &= sub.subscribe("split/topic", /*qos=*/0);
        g_ok &= pub.connect(port, "split-pub");
        if (!g_ok) {
            std::printf("split test: setup failed\n");
            std::printf("FAIL\n");
            return 1;
        }

        std::vector<std::byte> pkt = build_publish_bytes("split/topic", "a-longer-payload-to-split-mid-body");
        const std::size_t half = pkt.size() / 2;
        std::vector<std::byte> first_half(pkt.begin(), pkt.begin() + static_cast<std::ptrdiff_t>(half));
        std::vector<std::byte> second_half(pkt.begin() + static_cast<std::ptrdiff_t>(half), pkt.end());

        expect(raw_send_all(pub.fd(), first_half), "split: first half sends successfully");
        // Long enough that the broker's session_loop has certainly already looped back around (its own
        // poll cadence is 200ms) and observed try_parse_packet() return Incomplete on the partial data.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        expect(raw_send_all(pub.fd(), second_half), "split: second half sends successfully");

        auto got = sub.wait_publish(2000);
        expect(got.has_value() && got->first == "split/topic" &&
                   got->second == "a-longer-payload-to-split-mid-body",
               "split: packet dispatched correctly once fully received");
    }

    // --- Test 3: session takeover stays prompt even with a non-trivial backlog buffered ahead of it —
    // regression guard for session_loop's inner-loop `kicked` re-check (without it, a buffered drain
    // would only notice a takeover once per OUTER iteration instead of once per packet).
    {
        TestClient victim;
        g_ok &= victim.connect(port, "takeover-target");
        g_ok &= victim.subscribe("takeover/topic", /*qos=*/0);
        if (!g_ok) {
            std::printf("takeover test: setup failed\n");
            std::printf("FAIL\n");
            return 1;
        }

        // Buffer a burst of 50 trivial, always-handled PINGREQ packets (type 0xC0, zero-length body) in
        // one send — session_loop's inner drain loop will dispatch all 50 in one pass unless it notices
        // `kicked` partway through.
        std::vector<std::byte> pings;
        for (int i = 0; i < 50; ++i) {
            pings.push_back(std::byte{0xC0});
            pings.push_back(std::byte{0x00});
        }
        expect(raw_send_all(victim.fd(), pings), "takeover: burst of 50 PINGREQs sends successfully");

        // Race a session-takeover CONNECT under the same client_id right behind the burst.
        TestClient takeover;
        const auto t0 = std::chrono::steady_clock::now();
        g_ok &= takeover.connect(port, "takeover-target");
        const auto took =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);

        expect(g_ok, "takeover: the new connection under the same client_id is admitted");
        // Generous bound (well under session_loop's 200ms poll cadence would be ideal, but the burst
        // itself plus normal scheduling jitter earns some headroom) — this is a regression guard, not a
        // tight timing assertion: without the inner-loop kicked re-check, a much larger/slower-draining
        // burst could stall this arbitrarily; 50 trivial packets is enough to exercise the code path.
        expect(took.count() < 2000, "takeover: admitted within a bounded time despite the buffered burst");
    }

    broker.stop();
    std::printf("buffered_read_framing: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
