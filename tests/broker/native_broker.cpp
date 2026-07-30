// AeroEdge 017 Phase-1 gate: `NativeBroker`, AeroEdge's own embedded MQTT 3.1.1 server, exercised over
// real loopback sockets by a hand-rolled test client built on the SAME shared codec (mqtt_codec.hpp) the
// broker itself uses — no external MQTT client dependency (matches this repo's deterministic,
// exit-code-gated, no-external-deps-where-avoidable test posture, e.g. tests/transport/tcp_transport.cpp).
//
// Proves, over a real socket:
//   (1) CONNECT/CONNACK, SUBSCRIBE/SUBACK, PUBLISH/PUBACK (QoS 1) all round-trip correctly;
//   (2) topic routing: a PUBLISH reaches only sessions with a matching subscription filter;
//   (3) `+` matches exactly one level and does not cross a `/`; `#` matches multiple trailing levels;
//   (4) retained messages: a PUBLISH with the retain flag replays to a NEW matching SUBSCRIBE;
//   (5) the on_publish() ingestion seam fires for every PUBLISH this broker instance handles.
// Deterministic, exit-code-gated (0 = pass); bounded polling; clean shutdown.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aero/broker/native_broker.hpp"

namespace mqtt = aero::transport::mqtt;
using aero::broker::Config;
using aero::broker::NativeBroker;
using aero::broker::topic_matches;

namespace {

// Minimal hand-rolled MQTT client for the test — built on the same portable primitives NativeBroker
// itself uses (quark::pal::net, aero::pal::poll, the shared mqtt_codec.hpp).
class TestClient {
public:
    ~TestClient() { close(); }

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
        vh.push_back(std::byte{0x04});  // protocol level 4 == MQTT 3.1.1
        vh.push_back(std::byte{0x02});  // clean session
        mqtt::put_u16_be(vh, 60);
        mqtt::put_str(vh, client_id);
        if (!mqtt::write_packet(fd_, std::byte{0x10}, vh)) return false;

        auto ack = wait_for(0x20, 2000);
        return ack.has_value() && ack->body.size() >= 2 && std::to_integer<std::uint8_t>(ack->body[1]) == 0;
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
                              bool retain) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        std::uint8_t flags = static_cast<std::uint8_t>(qos << 1);
        if (retain) flags |= 0x01;
        if (!mqtt::write_packet(fd_, static_cast<std::byte>(0x30 | flags), vh)) return false;
        return qos == 0 || wait_for(0x40, 2000).has_value();
    }

    // Waits for an inbound PUBLISH; PUBACKs it if QoS 1. Returns (topic, payload) or nullopt on timeout —
    // a timeout is the expected, correct outcome for the "topic does NOT match" test cases.
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

    // Polls the inbox for the first packet whose fixed-header type nibble matches; removes + returns it.
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

bool test_topic_matches() {
    bool ok = true;
    ok &= topic_matches("sensor/+/temp", "sensor/line1/temp");
    ok &= !topic_matches("sensor/+/temp", "sensor/line1/extra/temp");  // '+' does not cross '/'
    ok &= topic_matches("sensor/#", "sensor/line1/temp");
    ok &= topic_matches("sensor/#", "sensor/line1/humidity/pct");      // '#' matches multiple levels
    ok &= topic_matches("sensor/#", "sensor");                        // '#' matches zero further levels
    ok &= !topic_matches("sensor/+/temp", "sensor/line1/pressure");
    ok &= topic_matches("status/line1", "status/line1");
    ok &= !topic_matches("status/line1", "status/line2");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});

    std::mutex cb_mu;
    std::vector<std::pair<std::string, std::string>> callback_hits;
    broker.on_publish([&](std::string_view topic, std::span<const std::byte> payload, std::uint8_t) {
        std::lock_guard<std::mutex> g(cb_mu);
        callback_hits.emplace_back(std::string(topic),
                                   std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
    });

    auto started = broker.start();
    ok &= started.has_value();
    if (!ok) {
        std::printf("broker start failed: %s\n", started.error().c_str());
        std::printf("FAIL\n");
        return 1;
    }
    const std::uint16_t port = broker.listen_port();

    // (1)/(2) plain SUBSCRIBE + PUBLISH round-trip, including a non-matching topic being filtered out.
    TestClient sub_a, pub_b;
    ok &= sub_a.connect(port, "sub-a");
    ok &= pub_b.connect(port, "pub-b");
    ok &= sub_a.subscribe("sensor/+/temp", /*qos=*/1);

    ok &= pub_b.publish("sensor/line1/temp", "23.5", /*qos=*/1, /*retain=*/false);
    auto got1 = sub_a.wait_publish();
    ok &= got1.has_value() && got1->first == "sensor/line1/temp" && got1->second == "23.5";

    ok &= pub_b.publish("sensor/line1/pressure", "1013", /*qos=*/1, /*retain=*/false);  // does NOT match filter
    auto got2 = sub_a.wait_publish(500);
    ok &= !got2.has_value();  // correctly filtered out

    ok &= pub_b.publish("sensor/line1/extra/temp", "99.9", /*qos=*/1, /*retain=*/false);  // '+' boundary
    auto got3 = sub_a.wait_publish(500);
    ok &= !got3.has_value();

    // (3) '#' wildcard across multiple levels.
    TestClient sub_hash;
    ok &= sub_hash.connect(port, "sub-hash");
    ok &= sub_hash.subscribe("sensor/#", /*qos=*/1);
    ok &= pub_b.publish("sensor/line1/temp", "24.0", /*qos=*/0, /*retain=*/false);
    auto gotH1 = sub_hash.wait_publish();
    ok &= gotH1.has_value() && gotH1->first == "sensor/line1/temp";
    ok &= pub_b.publish("sensor/line2/humidity/pct", "55", /*qos=*/0, /*retain=*/false);
    auto gotH2 = sub_hash.wait_publish();
    ok &= gotH2.has_value() && gotH2->first == "sensor/line2/humidity/pct" && gotH2->second == "55";

    // (4) retained: publish-with-retain BEFORE any subscriber exists, then a NEW subscription still gets
    // it. QoS 1 here (not 0) so publish() blocks for the PUBACK — that's the test's only synchronization
    // point proving the broker has actually stored the retained value before the next connection races in.
    ok &= pub_b.publish("status/line1", "up", /*qos=*/1, /*retain=*/true);
    TestClient sub_retained;
    ok &= sub_retained.connect(port, "sub-retained");
    ok &= sub_retained.subscribe("status/line1", /*qos=*/0);
    auto gotR = sub_retained.wait_publish();
    ok &= gotR.has_value() && gotR->first == "status/line1" && gotR->second == "up";

    sub_a.close();
    pub_b.close();
    sub_hash.close();
    sub_retained.close();
    broker.stop();

    // (5) the on_publish() ingestion seam saw every PUBLISH this broker instance handled.
    {
        std::lock_guard<std::mutex> g(cb_mu);
        ok &= callback_hits.size() == 6;  // temp, pressure, extra/temp, line1/temp(qos0), humidity, status
        bool saw_status_up = false;
        for (auto& [t, p] : callback_hits)
            if (t == "status/line1" && p == "up") saw_status_up = true;
        ok &= saw_status_up;
    }

    ok &= test_topic_matches();

    std::printf("native_broker: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
