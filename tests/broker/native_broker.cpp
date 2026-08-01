// AeroEdge 017 Phase-1 gate + Milestone-1 gate: `NativeBroker`, AeroEdge's own embedded MQTT 3.1.1 server,
// exercised over real loopback sockets by a hand-rolled test client built on the SAME shared codec
// (mqtt_codec.hpp) the broker itself uses — no external MQTT client dependency (matches this repo's
// deterministic, exit-code-gated, no-external-deps-where-avoidable test posture, e.g.
// tests/transport/tcp_transport.cpp).
//
// Proves, over a real socket:
//   Phase 1 —
//   (1) CONNECT/CONNACK, SUBSCRIBE/SUBACK, PUBLISH/PUBACK (QoS 1) all round-trip correctly;
//   (2) topic routing: a PUBLISH reaches only sessions with a matching subscription filter;
//   (3) `+` matches exactly one level and does not cross a `/`; `#` matches multiple trailing levels;
//   (4) retained messages: a PUBLISH with the retain flag replays to a NEW matching SUBSCRIBE;
//   (5) the on_publish() ingestion seam fires for every PUBLISH this broker instance handles.
//   Milestone 1 —
//   (6) QoS 2: PUBLISH → PUBREC → PUBREL → PUBCOMP round-trips and delivers exactly once, including
//       when a PUBLISH is retransmitted under the same packet id (dedup, not double-delivery);
//   (7) Last Will & Testament: delivered on an ABRUPT disconnect (socket just closed, no DISCONNECT
//       packet), NOT delivered on a clean DISCONNECT (3.1.1 §3.14);
//   (8) keep-alive: a silent connection is closed by the broker after ~1.5x its keep-alive interval;
//   (9) persistent sessions (clean session=0): a reconnect under the same client-id gets
//       session-present=1, its prior subscription back without re-SUBSCRIBEing, and any QoS≥1 message
//       that arrived while it was offline, flushed immediately.
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
#include <unordered_map>
#include <utility>
#include <vector>

#include "aero/broker/native_broker.hpp"

namespace mqtt = aero::transport::mqtt;
using aero::broker::Config;
using aero::broker::NativeBroker;
using aero::broker::topic_matches;

namespace {

// CONNECT knobs a test needs beyond the plain "clean session, 60s keep-alive, no will" default — kept as
// an options struct (not more connect() overloads) so each new Milestone-1 feature didn't need its own
// bespoke connect variant.
struct ConnectOptions {
    std::uint16_t keep_alive_s = 60;
    bool clean_session = true;
    bool has_will = false;
    std::string will_topic;
    std::string will_message;
    std::uint8_t will_qos = 0;
    bool will_retain = false;
};

// Minimal hand-rolled MQTT client for the test — built on the same portable primitives NativeBroker
// itself uses (quark::pal::net, aero::pal::poll, the shared mqtt_codec.hpp).
class TestClient {
public:
    ~TestClient() { close(); }

    [[nodiscard]] bool connect(std::uint16_t port, const std::string& client_id,
                               const ConnectOptions& opts = {}) {
        auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
        if (!fd) return false;
        fd_ = *fd;
        const auto writable = aero::pal::wait_writable(fd_, 2000);
        if (!writable || !*writable || !quark::pal::connect_result(fd_)) return false;

        running_.store(true, std::memory_order_release);
        peer_closed_.store(false, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        std::uint8_t flags = 0;
        if (opts.clean_session) flags |= 0x02;
        if (opts.has_will) {
            flags |= 0x04;
            flags |= static_cast<std::uint8_t>((opts.will_qos & 0x03) << 3);
            if (opts.will_retain) flags |= 0x20;
        }
        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(std::byte{0x04});  // protocol level 4 == MQTT 3.1.1
        vh.push_back(static_cast<std::byte>(flags));
        mqtt::put_u16_be(vh, opts.keep_alive_s);
        mqtt::put_str(vh, client_id);
        if (opts.has_will) {
            mqtt::put_str(vh, opts.will_topic);
            mqtt::put_str(vh, opts.will_message);  // will "message" is length-prefixed binary; a UTF-8
                                                    // string is encoded identically for this test's needs
        }
        if (!mqtt::write_packet(fd_, std::byte{0x10}, vh)) return false;

        auto ack = wait_for(0x20, 2000);
        if (!ack.has_value() || ack->body.size() < 2) return false;
        session_present_ = (std::to_integer<std::uint8_t>(ack->body[0]) & 0x01) != 0;
        return std::to_integer<std::uint8_t>(ack->body[1]) == 0;
    }

    // Session-present flag off the most recent CONNACK (3.1.1 §3.2.2.2) — set only for a clean_session=0
    // reconnect that found a stored prior session.
    [[nodiscard]] bool session_present() const noexcept { return session_present_; }

    // True once the reader thread has observed the PEER (broker) close the connection — as opposed to
    // this TestClient's own close() flipping running_ false. The keep-alive test's only way to tell
    // "the broker reaped me" from "nothing happened yet".
    [[nodiscard]] bool peer_closed() const noexcept { return peer_closed_.load(std::memory_order_acquire); }

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

    // 017 M7.2 PR B: a v5 CONNECT (protocol level 5) with an empty CONNECT Properties block — this test
    // file's TestClient is otherwise v4-only (connect() above hardcodes level 4); this minimal v5 path
    // exists solely so main()'s on_publish() test can prove the new 4th PublishProperties parameter is
    // populated correctly, which requires a PUBLISH able to carry Properties at all.
    [[nodiscard]] bool connect_v5(std::uint16_t port, const std::string& client_id) {
        auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
        if (!fd) return false;
        fd_ = *fd;
        const auto writable = aero::pal::wait_writable(fd_, 2000);
        if (!writable || !*writable || !quark::pal::connect_result(fd_)) return false;

        running_.store(true, std::memory_order_release);
        peer_closed_.store(false, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(std::byte{0x05});  // protocol level 5 == MQTT 5
        vh.push_back(std::byte{0x02});  // connect flags: clean session, no will/user/pass
        mqtt::put_u16_be(vh, /*keep_alive_s=*/60);
        mqtt::put_empty_properties(vh);  // CONNECT Properties — empty is legal (§3.1.2.11)
        mqtt::put_str(vh, client_id);
        if (!mqtt::write_packet(fd_, std::byte{0x10}, vh)) return false;

        auto ack = wait_for(0x20, 2000);
        if (!ack.has_value() || ack->body.size() < 2) return false;
        session_present_ = (std::to_integer<std::uint8_t>(ack->body[0]) & 0x01) != 0;
        return std::to_integer<std::uint8_t>(ack->body[1]) == 0;
    }

    // 017 M7.2 PR B: a v5 PUBLISH carrying Response Topic (0x08), Correlation Data (0x09), and User
    // Properties (0x26) — the wire-level counterpart of NativeBroker::PublishExtras/PublishProperties.
    [[nodiscard]] bool publish_v5_with_props(
        const std::string& topic, const std::string& payload, std::uint8_t qos, bool retain,
        const std::string& response_topic, const std::vector<std::byte>& correlation_data,
        const std::vector<std::pair<std::string, std::string>>& user_properties) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        mqtt::PropertyWriter pw;
        pw.put_str(0x08, response_topic);
        pw.put_binary(0x09, correlation_data);
        for (const auto& [k, v] : user_properties) pw.put_str_pair(0x26, k, v);
        std::vector<std::byte> props;
        pw.write(props);
        vh.insert(vh.end(), props.begin(), props.end());
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

    // --- QoS 2 (4.3.3): PUBLISH → PUBREC → PUBREL → PUBCOMP ------------------------------------------
    // Split into raw steps (not just one round-trip helper) so a test can inject a retransmit — send the
    // same PUBLISH/packet-id twice before ever sending PUBREL — to prove the broker dedupes instead of
    // double-routing.
    [[nodiscard]] bool send_publish_raw(const std::string& topic, const std::string& payload,
                                        std::uint8_t qos, bool retain, std::uint16_t packet_id) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, packet_id);
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        std::uint8_t flags = static_cast<std::uint8_t>(qos << 1);
        if (retain) flags |= 0x01;
        return mqtt::write_packet(fd_, static_cast<std::byte>(0x30 | flags), vh);
    }
    [[nodiscard]] bool wait_pubrec(std::uint16_t expect_id, int timeout_ms = 2000) {
        return matches_id(wait_for(0x50, timeout_ms), expect_id);
    }
    [[nodiscard]] bool send_pubrel(std::uint16_t packet_id) {
        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, packet_id);
        return mqtt::write_packet(fd_, std::byte{0x62}, vh);  // flags MUST be 0010 (4.3.3)
    }
    [[nodiscard]] bool wait_pubcomp(std::uint16_t expect_id, int timeout_ms = 2000) {
        return matches_id(wait_for(0x70, timeout_ms), expect_id);
    }
    // Convenience: the full QoS-2 sender-side handshake in one call, for tests that don't need to inject
    // a retransmit.
    [[nodiscard]] bool publish_qos2(const std::string& topic, const std::string& payload,
                                    bool retain = false) {
        const std::uint16_t pid = next_id();
        return send_publish_raw(topic, payload, /*qos=*/2, retain, pid) && wait_pubrec(pid) &&
               send_pubrel(pid) && wait_pubcomp(pid);
    }
    [[nodiscard]] std::uint16_t reserve_id() { return next_id(); }

    // Graceful shutdown (3.1.1 §3.14): sends DISCONNECT (0xE0) before tearing down the socket — the
    // broker must NOT deliver this client's Will after seeing this. Contrast with plain close() below,
    // which is deliberately the OTHER case: no DISCONNECT at all, simulating a socket reset/crash.
    void disconnect_clean() {
        (void)mqtt::write_packet(fd_, std::byte{0xE0}, {});
        close();
    }

    // Just closes the fd — no DISCONNECT packet. From the broker's point of view this is indistinguishable
    // from a crashed/network-partitioned client, which is exactly the case the Will and keep-alive tests
    // need: an "ungraceful" end (3.1.1 §3.1.2.5 / §3.14).
    void close() {
        running_.store(false, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
        if (fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(fd_);
            fd_ = quark::pal::invalid_fd;
        }
    }

private:
    [[nodiscard]] static bool matches_id(const std::optional<mqtt::Packet>& pkt, std::uint16_t expect_id) {
        if (!pkt || pkt->body.size() < 2) return false;
        const std::uint16_t id =
            (std::to_integer<std::uint8_t>(pkt->body[0]) << 8) | std::to_integer<std::uint8_t>(pkt->body[1]);
        return id == expect_id;
    }

    void reader_loop() {
        while (running_.load(std::memory_order_acquire)) {
            auto pkt = mqtt::read_packet(fd_, running_);
            if (!pkt) {
                // Distinguish "the peer (broker) closed on us" from "our own close() asked the read loop
                // to stop" — only the former should register as peer_closed_ (running_ is still true here
                // iff nobody local asked for shutdown).
                if (running_.load(std::memory_order_acquire)) peer_closed_.store(true, std::memory_order_release);
                break;
            }
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
    std::atomic<bool> peer_closed_{false};
    bool session_present_ = false;
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

// Each Milestone-1 test gets its OWN broker instance (ephemeral port) — keeps them fully isolated from
// each other and from the Phase-1 block above (whose on_publish() callback-count assertion is scoped
// exactly to its own broker instance and would otherwise have to account for every PUBLISH every other
// test below it performs too).

// (6) QoS 2 (4.3.3): PUBLISH → PUBREC → PUBREL → PUBCOMP delivers exactly once, and a PUBLISH
// retransmitted under the same packet id (as if the original PUBREC were lost) is deduped, not
// double-delivered.
bool test_qos2() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    TestClient sub, pub;
    ok &= sub.connect(port, "qos2-sub");
    ok &= pub.connect(port, "qos2-pub");
    ok &= sub.subscribe("qos2/topic", /*qos=*/1);

    ok &= pub.publish_qos2("qos2/topic", "hello2");
    auto got = sub.wait_publish();
    ok &= got.has_value() && got->first == "qos2/topic" && got->second == "hello2";

    // Simulate the sender retrying PUBLISH under the same packet id (its PUBREC was "lost") BEFORE ever
    // sending PUBREL — the broker must still PUBREC it (idempotent ack) but route it only once, when
    // PUBREL finally arrives.
    const std::uint16_t pid = pub.reserve_id();
    ok &= pub.send_publish_raw("qos2/topic", "dup-payload", /*qos=*/2, /*retain=*/false, pid);
    ok &= pub.wait_pubrec(pid);
    ok &= pub.send_publish_raw("qos2/topic", "dup-payload", /*qos=*/2, /*retain=*/false, pid);
    ok &= pub.wait_pubrec(pid);
    ok &= pub.send_pubrel(pid);
    ok &= pub.wait_pubcomp(pid);
    auto got2 = sub.wait_publish();
    ok &= got2.has_value() && got2->second == "dup-payload";
    auto got3 = sub.wait_publish(500);  // must NOT be delivered a second time
    ok &= !got3.has_value();

    sub.close();
    pub.close();
    broker.stop();
    return ok;
}

// (7) Last Will & Testament (3.1.2.5 / 3.14): delivered on an ABRUPT disconnect (socket just closed, no
// DISCONNECT packet); NOT delivered on a clean DISCONNECT.
bool test_will() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    TestClient observer;
    ok &= observer.connect(port, "will-observer");
    ok &= observer.subscribe("will/#", /*qos=*/1);

    {
        TestClient dying;
        ConnectOptions opts;
        opts.has_will = true;
        opts.will_topic = "will/abrupt";
        opts.will_message = "gone";
        opts.will_qos = 1;
        ok &= dying.connect(port, "will-abrupt", opts);
        dying.close();  // no DISCONNECT sent — simulates a crash / network reset (§3.1.2.5)
    }
    auto got = observer.wait_publish();
    ok &= got.has_value() && got->first == "will/abrupt" && got->second == "gone";

    {
        TestClient graceful;
        ConnectOptions opts;
        opts.has_will = true;
        opts.will_topic = "will/clean";
        opts.will_message = "should-not-arrive";
        ok &= graceful.connect(port, "will-clean", opts);
        graceful.disconnect_clean();  // §3.14: a clean DISCONNECT discards the Will
    }
    auto got2 = observer.wait_publish(500);
    ok &= !got2.has_value();

    observer.close();
    broker.stop();
    return ok;
}

// (8) Keep-alive (3.1.1 §3.1.2.10): a connection that never sends anything (not even PINGREQ) after
// CONNECT is closed by the broker once it has been silent longer than 1.5x its declared keep-alive.
bool test_keep_alive() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    TestClient silent;
    ConnectOptions opts;
    opts.keep_alive_s = 1;  // reaped after ~1.5s of silence
    ok &= silent.connect(port, "silent-client", opts);

    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    ok &= silent.peer_closed();  // broker must have closed it well before now

    silent.close();
    broker.stop();
    return ok;
}

// (9) Persistent sessions (3.1.1 §3.1.2.4): a clean_session=0 reconnect under the same client-id gets
// session-present=1, its prior subscription restored without re-SUBSCRIBEing, and any QoS≥1 message that
// arrived while it was offline flushed immediately.
bool test_persistent_session() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    ConnectOptions persist_opts;
    persist_opts.clean_session = false;

    TestClient persist1;
    ok &= persist1.connect(port, "persist-1", persist_opts);
    ok &= !persist1.session_present();  // nothing stored yet the first time
    ok &= persist1.subscribe("queue/topic", /*qos=*/1);
    persist1.disconnect_clean();  // goes offline; a graceful disconnect here just keeps this test's
                                   // assertions focused on persistence, not Will (test_will covers that)

    // Give the broker's session thread a moment to finish tearing down persist1 (moving its state into
    // the offline store) before publishing — otherwise this publish could race a live session that is
    // mid-teardown. Test-only synchronization aid; the broker itself has no cross-thread signal for "a
    // torn-down session's state has been persisted" to wait on instead.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    TestClient other;
    ok &= other.connect(port, "other-pub");
    ok &= other.publish("queue/topic", "queued-msg", /*qos=*/1, /*retain=*/false);  // arrives while offline

    TestClient persist2;
    ok &= persist2.connect(port, "persist-1", persist_opts);  // same client-id, still clean_session=0
    ok &= persist2.session_present();                         // broker found the stored session

    // Queued message flushed right after CONNACK, with persist2 never having called subscribe() itself.
    auto got = persist2.wait_publish();
    ok &= got.has_value() && got->first == "queue/topic" && got->second == "queued-msg";

    // A brand-new live publish also reaches persist2 with no re-SUBSCRIBE — proves the subscription
    // itself was restored, not just the offline queue flushed.
    ok &= other.publish("queue/topic", "live-msg", /*qos=*/1, /*retain=*/false);
    auto got2 = persist2.wait_publish();
    ok &= got2.has_value() && got2->first == "queue/topic" && got2->second == "live-msg";

    other.close();
    persist2.close();
    broker.stop();
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});

    std::mutex cb_mu;
    std::vector<std::pair<std::string, std::string>> callback_hits;
    // 017 M7.2 PR B: on_publish() gained a 4th parameter, `props` — captured here (by topic) so the (5b)
    // block below can assert it was populated correctly for a PUBLISH that actually carried Response
    // Topic/Correlation Data/User Properties, while every other (v4) PUBLISH in this test still sees an
    // empty PublishProperties (proving the mechanical fallout didn't regress the v4 path).
    std::unordered_map<std::string, aero::broker::PublishProperties> callback_props;
    broker.on_publish([&](std::string_view topic, std::span<const std::byte> payload, std::uint8_t,
                          const aero::broker::PublishProperties& props) {
        std::lock_guard<std::mutex> g(cb_mu);
        callback_hits.emplace_back(std::string(topic),
                                   std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
        callback_props[std::string(topic)] = props;
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

    // (5b) 017 M7.2 PR B: on_publish()'s new 4th parameter (PublishProperties) is populated correctly for
    // a v5 PUBLISH carrying Response Topic/Correlation Data/User Properties — this file's job is "does the
    // public C++ API surface work" (mqtt5.cpp separately owns wire-level codec/protocol correctness).
    TestClient pub_v5;
    ok &= pub_v5.connect_v5(port, "pub-v5");
    const std::vector<std::byte> corr_data{std::byte{0xAB}, std::byte{0x00}, std::byte{0xCD}};  // embeds
                                                                                                  // a NUL
    ok &= pub_v5.publish_v5_with_props("props/topic", "v5-payload", /*qos=*/1, /*retain=*/false,
                                       "reply/to/me", corr_data,
                                       {{"k1", "v1"}, {"k2", "v2"}});
    pub_v5.close();

    sub_a.close();
    pub_b.close();
    sub_hash.close();
    sub_retained.close();
    broker.stop();

    // (5) the on_publish() ingestion seam saw every PUBLISH this broker instance handled.
    {
        std::lock_guard<std::mutex> g(cb_mu);
        ok &= callback_hits.size() == 7;  // temp, pressure, extra/temp, line1/temp(qos0), humidity, status,
                                          // props/topic (v5)
        const auto pit = callback_props.find("props/topic");
        ok &= pit != callback_props.end();
        if (pit != callback_props.end()) {
            const auto& props = pit->second;
            ok &= props.response_topic.has_value() && *props.response_topic == "reply/to/me";
            ok &= props.correlation_data.has_value() && *props.correlation_data == corr_data;
            ok &= props.user_properties.size() == 2;
            ok &= props.user_properties.size() >= 1 && props.user_properties[0].first == "k1" &&
                  props.user_properties[0].second == "v1";
            ok &= props.user_properties.size() >= 2 && props.user_properties[1].first == "k2" &&
                  props.user_properties[1].second == "v2";
        }
        // Every OTHER (v4) PUBLISH in this test must still see an empty PublishProperties — the mechanical
        // signature change must not leak stale/cross-talk data across unrelated PUBLISHes.
        const auto vit = callback_props.find("status/line1");
        ok &= vit != callback_props.end() && !vit->second.response_topic.has_value() &&
              !vit->second.correlation_data.has_value() && vit->second.user_properties.empty();
        bool saw_status_up = false;
        for (auto& [t, p] : callback_hits)
            if (t == "status/line1" && p == "up") saw_status_up = true;
        ok &= saw_status_up;
    }

    ok &= test_topic_matches();

    // Milestone 1 — each runs against its own fresh broker instance (see comment above test_qos2).
    ok &= test_qos2();
    ok &= test_will();
    ok &= test_keep_alive();
    ok &= test_persistent_session();

    std::printf("native_broker: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
