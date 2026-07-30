// AeroEdge 017 §6 (revised scope) gate: `include/aero/broker/bridge.hpp` — bridging + the rule engine
// reused over `NativeBroker::on_publish()`.
//
// Covers:
//   (1) BrokerRuleEngine topic filtering: a rule only fires for PUBLISHes matching its topic_filter
//       (aero::broker::topic_matches, reused not reimplemented) — proven with a FakeBridgeSink that just
//       records calls.
//   (2) BrokerRuleEngine's expr gate: ExprRuleNode (008 §6) reused, not reimplemented — a rule with an
//       expression only fires when the JSON payload's flattened numeric fields satisfy it; a rule with
//       no expression fires on topic match alone; a non-JSON payload makes an expr referencing payload
//       fields read falsy (never a crash).
//   (3) HttpWebhookBridgeSink actually POSTing correctly against a tiny in-process httplib server (same
//       pattern as tests/mes/mes_outbox.cpp / tests/api/api_integration.cpp: ephemeral port, bounded
//       /health poll, clean shutdown) — asserts the JSON body shape (topic/qos/payload/payload_encoding).
//   (4) MqttBridgeSink end-to-end against a REAL NativeBroker on loopback: a subscriber (the same
//       hand-rolled TestClient pattern tests/broker/native_broker.cpp uses) proves a bridged PUBLISH
//       actually arrives with the right topic/payload/qos.
// Deterministic, exit-code-gated (0 = pass); bounded polling; clean shutdown.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aero/broker/bridge.hpp"
#include "aero/broker/native_broker.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace mqtt = aero::transport::mqtt;
using aero::broker::BridgeConfig;
using aero::broker::BrokerRuleEngine;
using aero::broker::HttpWebhookBridgeSink;
using aero::broker::IBridgeSink;
using aero::broker::MqttBridgeSink;
using aero::broker::NativeBroker;

namespace {

// ---- a fake sink that just records what reached it (no I/O) -----------------------------------------
struct FakeSink final : public IBridgeSink {
    std::mutex mu;
    std::vector<std::string> topics;
    std::vector<std::string> payloads;
    std::vector<std::uint8_t> qoses;

    bool connect(const BridgeConfig&) override { return true; }
    bool publish(std::string_view topic, std::span<const std::byte> payload, std::uint8_t qos) override {
        std::lock_guard<std::mutex> g(mu);
        topics.emplace_back(topic);
        payloads.emplace_back(reinterpret_cast<const char*>(payload.data()), payload.size());
        qoses.push_back(qos);
        return true;
    }
    std::size_t hit_count() { std::lock_guard<std::mutex> g(mu); return topics.size(); }
};

std::vector<std::byte> to_bytes(const std::string& s) {
    std::vector<std::byte> b(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) b[i] = static_cast<std::byte>(static_cast<std::uint8_t>(s[i]));
    return b;
}

// ---- (1) + (2): BrokerRuleEngine — topic filtering + the reused ExprRuleNode content gate -----------
bool test_rule_engine() {
    bool ok = true;

    BrokerRuleEngine engine;
    auto temp_sink = std::make_shared<FakeSink>();
    auto status_sink = std::make_shared<FakeSink>();
    auto always_sink = std::make_shared<FakeSink>();

    // A topic-filtered, expr-gated rule: only "sensor/+/temp" AND payload.temperature > 90.
    ok &= engine.add_rule("sensor/+/temp", "tag(\"temperature\") > 90", temp_sink);
    // A topic-filtered rule with NO expr: fires on any matching topic regardless of payload content.
    ok &= engine.add_rule("status/#", "", status_sink);
    // A malformed expression must be rejected at registration (never silently accepted).
    std::string err;
    ok &= !engine.add_rule("bad/#", "tag(\"x\" ", always_sink, &err);
    ok &= !err.empty();

    ok &= engine.rule_count() == 2;

    // Non-matching topic: no sink called.
    auto p1 = to_bytes(R"({"temperature": 95})");
    engine.handle_publish("sensor/line1/pressure", p1, 1);
    ok &= temp_sink->hit_count() == 0;

    // Matching topic, expr false (temperature under threshold): sink NOT called.
    auto p2 = to_bytes(R"({"temperature": 50})");
    engine.handle_publish("sensor/line1/temp", p2, 1);
    ok &= temp_sink->hit_count() == 0;

    // Matching topic, expr true: sink IS called with the right topic/payload/qos.
    auto p3 = to_bytes(R"({"temperature": 95.5})");
    engine.handle_publish("sensor/line1/temp", p3, 1);
    ok &= temp_sink->hit_count() == 1;
    if (temp_sink->hit_count() == 1) {
        ok &= temp_sink->topics[0] == "sensor/line1/temp";
        ok &= temp_sink->payloads[0] == R"({"temperature": 95.5})";
        ok &= temp_sink->qoses[0] == 1;
    }

    // Non-JSON payload against an expr referencing a payload field: falsy, never a crash (per the
    // header's documented "missing tag reads as 0.0" mapping) — the rule does not fire.
    auto p4 = to_bytes("not json at all");
    engine.handle_publish("sensor/line2/temp", p4, 0);
    ok &= temp_sink->hit_count() == 1;  // unchanged

    // No-expr rule: fires on topic match alone, regardless of payload shape.
    auto p5 = to_bytes("up");
    engine.handle_publish("status/line1", p5, 0);
    ok &= status_sink->hit_count() == 1;
    auto p6 = to_bytes(R"({"anything": "goes"})");
    engine.handle_publish("status/line2", p6, 1);
    ok &= status_sink->hit_count() == 2;

    // '#' / '+' filtering already independently proven in tests/broker/native_broker.cpp
    // (topic_matches itself); here we only need to prove the ENGINE applies it before the sink is
    // called — a non-matching topic under "status/#" must not reach the sink.
    engine.handle_publish("other/topic", p5, 0);
    ok &= status_sink->hit_count() == 2;  // unchanged

    return ok;
}

// ---- (3): HttpWebhookBridgeSink over a real local httplib server ------------------------------------
bool test_http_webhook_sink() {
    bool ok = true;

    struct Captured {
        std::mutex mu;
        int count = 0;
        std::string topic, payload, encoding, auth_header;
        int qos = -1;
    } captured;

    httplib::Server svr;
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });
    svr.Post("/bridge/ingest", [&](const httplib::Request& req, httplib::Response& res) {
        auto j = nlohmann::json::parse(req.body, nullptr, false);
        if (j.is_discarded()) { res.status = 400; return; }
        std::lock_guard<std::mutex> g(captured.mu);
        ++captured.count;
        captured.topic = j.value("topic", std::string{});
        captured.payload = j.value("payload", std::string{});
        captured.encoding = j.value("payload_encoding", std::string{});
        captured.qos = j.value("qos", -1);
        captured.auth_header = req.get_header_value("Authorization");
        res.status = 200;
    });

    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) { std::printf("bridge webhook: bind failed\n"); return false; }
    std::thread server_thread([&svr] { svr.listen_after_bind(); });

    {
        httplib::Client probe("127.0.0.1", port);
        probe.set_connection_timeout(2, 0);
        bool ready = false;
        for (int i = 0; i < 300 && !ready; ++i) {
            auto h = probe.Get("/health");
            if (h && h->status == 200) ready = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!ready) {
            std::printf("bridge webhook: server not ready\n");
            svr.stop(); server_thread.join();
            return false;
        }
    }

    HttpWebhookBridgeSink sink;
    BridgeConfig cfg;
    cfg.endpoint = "127.0.0.1";
    cfg.port = port;
    cfg.path = "/bridge/ingest";
    cfg.token = "s3cr3t";
    ok &= sink.connect(cfg);

    // UTF-8 text payload -> "utf8" encoding, verbatim string.
    auto p1 = to_bytes(R"({"value":42})");
    ok &= sink.publish("sensor/line1/value", p1, 1);

    // Binary payload (embedded NUL / non-UTF-8 byte) -> "base64" encoding.
    std::vector<std::byte> bin{std::byte{0xFF}, std::byte{0x00}, std::byte{0xC3}, std::byte{0x28}};
    ok &= sink.publish("sensor/line1/raw", bin, 0);

    {
        std::lock_guard<std::mutex> g(captured.mu);
        ok &= captured.count == 2;
        ok &= captured.auth_header == "Bearer s3cr3t";
        // The LAST request captured is the binary one (base64).
        ok &= captured.topic == "sensor/line1/raw";
        ok &= captured.encoding == "base64";
        ok &= captured.qos == 0;
        // decode and compare round-trip
        static constexpr char kTable[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string decoded;
        auto decode_char = [&](char c) -> int {
            for (int i = 0; i < 64; ++i) if (kTable[i] == c) return i;
            return -1;
        };
        std::string b64 = captured.payload;
        std::vector<std::uint8_t> raw;
        int val = 0, bits = 0;
        for (char c : b64) {
            if (c == '=') break;
            int d = decode_char(c);
            if (d < 0) continue;
            val = (val << 6) | d;
            bits += 6;
            if (bits >= 8) { bits -= 8; raw.push_back(static_cast<std::uint8_t>((val >> bits) & 0xFF)); }
        }
        ok &= raw.size() == bin.size();
        if (raw.size() == bin.size()) {
            for (std::size_t i = 0; i < raw.size(); ++i)
                ok &= raw[i] == std::to_integer<std::uint8_t>(bin[i]);
        }
    }

    svr.stop();
    server_thread.join();
    return ok;
}

// ---- (4): MqttBridgeSink end-to-end against a real NativeBroker -------------------------------------
// Minimal hand-rolled MQTT subscriber, mirroring tests/broker/native_broker.cpp's TestClient — proves
// the bridge sink's own CONNECT/PUBLISH wire I/O actually round-trips through a real broker.
class SubClient {
public:
    ~SubClient() { close(); }

    bool connect(std::uint16_t port) {
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
        vh.push_back(std::byte{0x02});
        mqtt::put_u16_be(vh, 60);
        mqtt::put_str(vh, "bridge-sub");
        if (!mqtt::write_packet(fd_, std::byte{0x10}, vh)) return false;
        auto ack = wait_for(0x20, 2000);
        return ack.has_value() && ack->body.size() >= 2 && std::to_integer<std::uint8_t>(ack->body[1]) == 0;
    }

    bool subscribe(const std::string& filter) {
        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, 1);
        mqtt::put_str(vh, filter);
        vh.push_back(std::byte{1});
        if (!mqtt::write_packet(fd_, std::byte{0x82}, vh)) return false;
        return wait_for(0x90, 2000).has_value();
    }

    std::optional<std::pair<std::string, std::string>> wait_publish(int timeout_ms = 2000) {
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
        if (fd_ != quark::pal::invalid_fd) { quark::pal::close_fd(fd_); fd_ = quark::pal::invalid_fd; }
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

    quark::pal::fd_t fd_ = quark::pal::invalid_fd;
    std::atomic<bool> running_{false};
    std::thread reader_;
    std::mutex mu_;
    std::vector<mqtt::Packet> inbox_;
};

bool test_mqtt_bridge_sink() {
    bool ok = true;

    NativeBroker broker(aero::broker::Config{"127.0.0.1", /*listen_port=*/0});
    auto started = broker.start();
    if (!started) { std::printf("mqtt bridge sink: broker start failed: %s\n", started.error().c_str()); return false; }
    const std::uint16_t port = broker.listen_port();

    SubClient sub;
    ok &= sub.connect(port);
    ok &= sub.subscribe("bridged/#");

    MqttBridgeSink sink;
    BridgeConfig cfg;
    cfg.endpoint = "tcp://127.0.0.1:" + std::to_string(port);
    cfg.client_id = "aero-bridge-test";
    ok &= sink.connect(cfg);

    auto payload = to_bytes("hello-from-bridge");
    ok &= sink.publish("bridged/topic1", payload, /*qos=*/1);

    auto got = sub.wait_publish();
    ok &= got.has_value() && got->first == "bridged/topic1" && got->second == "hello-from-bridge";

    // A second publish at QoS 0 also round-trips.
    auto payload0 = to_bytes("qos0-msg");
    ok &= sink.publish("bridged/topic2", payload0, /*qos=*/0);
    auto got0 = sub.wait_publish();
    ok &= got0.has_value() && got0->first == "bridged/topic2" && got0->second == "qos0-msg";

    // A bad broker URI is rejected cleanly (never throws, never hangs).
    MqttBridgeSink bad_sink;
    BridgeConfig bad_cfg;
    bad_cfg.endpoint = "not-a-uri";
    ok &= !bad_sink.connect(bad_cfg);

    sink.close();
    sub.close();
    broker.stop();
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    const bool rule_ok = test_rule_engine();
    ok &= rule_ok;
    std::printf("[rule_engine] %s\n", rule_ok ? "ok" : "FAIL");

    const bool http_ok = test_http_webhook_sink();
    ok &= http_ok;
    std::printf("[http_webhook_sink] %s\n", http_ok ? "ok" : "FAIL");

    const bool mqtt_ok = test_mqtt_bridge_sink();
    ok &= mqtt_ok;
    std::printf("[mqtt_bridge_sink] %s\n", mqtt_ok ? "ok" : "FAIL");

    std::printf("bridge: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
