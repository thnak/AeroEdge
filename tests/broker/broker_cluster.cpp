// 017 M6 gate: BrokerCluster — cross-node PUBLISH broadcast (broker_cluster.hpp) over REAL loopback TCP.
// Two NativeBroker+BrokerCluster pairs on real sockets, ephemeral ports — mirrors
// tests/transport/tcp_transport.cpp's "two real nodes over actual sockets" shape (NOT
// cross_node_message.cpp's in-process LoopbackTransport one), since this has to prove the REAL
// transport path, not just the in-process DistributedRouter machinery. The MQTT client side reuses the
// same hand-rolled-client idiom as tests/broker/native_broker.cpp's TestClient, slimmed to just
// connect/subscribe/publish/wait_publish (QoS 1) — this test needs nothing else.
//
// Proves, over real loopback sockets:
//   (1) the cross-node hop: a client subscribed on node 1 receives a PUBLISH a DIFFERENT client sent to
//       node 2 — the whole point of 017 M6 (single-node NativeBroker cannot do this on its own).
//   (2) loop prevention: a client subscribed on node 2 to the SAME topic gets exactly ONE copy of node
//       2's own local PUBLISH, not a duplicate bounced back to itself through deliver_remote_publish()
//       (node 2 must never broadcast a relay-delivered PUBLISH onward — native_broker.hpp's M6 banner).
//   (3) M7.2 PR E: a v5 PUBLISH's Message Expiry Interval, Response Topic, Correlation Data, and User
//       Property all survive the cross-node relay hop intact — the gap deliver_remote_publish()'s OLD
//       banner (and BrokerRelayMsg's OLD comment) used to document directly. A minimal v5-capable
//       extension of TestClient (Properties-block CONNECT/PUBLISH + decode) is duplicated in here rather
//       than reused from tests/broker/mqtt5.cpp's own V5TestClient — same "small duplication is the
//       pragmatic call" precedent this file's own banner already set for the plain v4 TestClient above.
// Deterministic, exit-code-gated (0 = pass); bounded polling; clean shutdown.
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

#include "aero/broker/broker_cluster.hpp"
#include "aero/broker/native_broker.hpp"

namespace mqtt = aero::transport::mqtt;
using aero::broker::BrokerCluster;
using aero::broker::Config;
using aero::broker::NativeBroker;
using aero::broker::PeerSpec;

namespace {

// ===== M7.2 PR E test-side Properties-block ENCODER (mirrors tests/broker/mqtt5.cpp's own — mqtt_codec.hpp
//       only ships a DECODER, see that file's comment; these are hand-rolled test helpers) =============
void put_prop_u32(std::vector<std::byte>& recs, std::uint8_t id, std::uint32_t v) {
    recs.push_back(static_cast<std::byte>(id));
    for (int i = 3; i >= 0; --i) recs.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void put_prop_str(std::vector<std::byte>& recs, std::uint8_t id, const std::string& v) {
    recs.push_back(static_cast<std::byte>(id));
    mqtt::put_str(recs, v);
}
void put_prop_binary(std::vector<std::byte>& recs, std::uint8_t id, const std::vector<std::byte>& v) {
    recs.push_back(static_cast<std::byte>(id));
    mqtt::put_u16_be(recs, static_cast<std::uint16_t>(v.size()));
    recs.insert(recs.end(), v.begin(), v.end());
}
void put_prop_str_pair(std::vector<std::byte>& recs, std::uint8_t id, const std::string& k,
                       const std::string& val) {
    recs.push_back(static_cast<std::byte>(id));
    mqtt::put_str(recs, k);
    mqtt::put_str(recs, val);
}
void put_properties(std::vector<std::byte>& out, const std::vector<std::byte>& records) {
    mqtt::put_remaining_length(out, static_cast<std::uint32_t>(records.size()));
    out.insert(out.end(), records.begin(), records.end());
}

// Minimal hand-rolled MQTT client — connect/subscribe/publish/wait_publish (QoS 1) only, slimmed from
// tests/broker/native_broker.cpp's TestClient (that file's version is anonymous-namespace-local, not a
// reusable header; a small duplication here is the pragmatic call given this test's narrow needs).
// M7.2 PR E: connect()/publish() gained optional v5 knobs (protocol_level, Properties records) — every
// existing 2-arg connect()/3-arg publish() call site keeps working unchanged at MQTT 3.1.1, no Properties.
class TestClient {
public:
    ~TestClient() { close(); }

    [[nodiscard]] bool connect(std::uint16_t port, const std::string& client_id,
                               std::uint8_t protocol_level = 0x04) {
        protocol_level_ = protocol_level;
        auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
        if (!fd) return false;
        fd_ = *fd;
        const auto writable = aero::pal::wait_writable(fd_, 2000);
        if (!writable || !*writable || !quark::pal::connect_result(fd_)) return false;

        running_.store(true, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(static_cast<std::byte>(protocol_level));
        vh.push_back(std::byte{0x02});  // connect flags: clean session, no Will
        mqtt::put_u16_be(vh, 60);       // keep-alive (s)
        if (protocol_level == 0x05) mqtt::put_empty_properties(vh);
        mqtt::put_str(vh, client_id);
        if (!mqtt::write_packet(fd_, std::byte{0x10}, vh)) return false;

        auto ack = wait_for(0x20, 2000);
        return ack.has_value() && ack->body.size() >= 2 && std::to_integer<std::uint8_t>(ack->body[1]) == 0;
    }

    [[nodiscard]] bool subscribe(const std::string& filter, std::uint8_t qos = 1) {
        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, next_id());
        if (protocol_level_ == 0x05) mqtt::put_empty_properties(vh);
        mqtt::put_str(vh, filter);
        vh.push_back(static_cast<std::byte>(qos));
        if (!mqtt::write_packet(fd_, std::byte{0x82}, vh)) return false;
        return wait_for(0x90, 2000).has_value();
    }

    [[nodiscard]] bool publish(const std::string& topic, const std::string& payload, std::uint8_t qos = 1,
                               const std::vector<std::byte>& pub_props_records = {}) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        if (protocol_level_ == 0x05) put_properties(vh, pub_props_records);
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        const std::uint8_t flags = static_cast<std::uint8_t>(qos << 1);
        if (!mqtt::write_packet(fd_, static_cast<std::byte>(0x30 | flags), vh)) return false;
        return qos == 0 || wait_for(0x40, 2000).has_value();
    }

    // Waits for an inbound PUBLISH; PUBACKs it if QoS 1. nullopt on timeout — the expected, correct
    // outcome for the "must NOT see a second copy" loop-prevention assertion.
    // M7.2 PR E: v5-aware — publish_to() writes a real (possibly-empty) Properties block for every v5
    // session (017 M7.2 PR A), so a v5-connected TestClient MUST skip it here or the payload is misparsed
    // (mirrors tests/broker/mqtt5.cpp's V5TestClient::wait_publish() — same fix, same reason). `props`
    // (out param) receives the decoded Properties when non-null; ignored (and skipped-but-not-decoded is
    // impossible — decode failure just fails the wait) for v4 sessions, which never carry one at all.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> wait_publish(
        int timeout_ms = 2000, mqtt::ParsedProperties* props = nullptr) {
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
        if (protocol_level_ == 0x05) {
            auto parsed = mqtt::read_properties(b, pos);
            if (!parsed) return std::nullopt;
            if (props) *props = std::move(*parsed);
        }
        if (pos > b.size()) return std::nullopt;
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
            if (!pkt) break;  // peer closed / error / running flipped false
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
    std::uint8_t protocol_level_ = 0x04;  // set in connect() — see wait_publish()'s own comment
};

}  // namespace

int main() {
    bool ok = true;

    NativeBroker broker1(Config{"127.0.0.1", /*listen_port=*/0});
    NativeBroker broker2(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker1.start().has_value();
    ok &= broker2.start().has_value();
    if (!ok) {
        std::printf("broker start failed\n");
        std::printf("FAIL\n");
        return 1;
    }

    // cluster_port=0 (ephemeral) on both — a placeholder peer port of 0 is corrected below, once each
    // side's real inter-broker port is known (mirrors tcp_transport.hpp's own two-phase pattern).
    BrokerCluster cluster1(quark::NodeId{1}, /*cluster_port=*/0,
                           {PeerSpec{quark::NodeId{2}, "127.0.0.1", 0}}, broker1);
    BrokerCluster cluster2(quark::NodeId{2}, /*cluster_port=*/0,
                           {PeerSpec{quark::NodeId{1}, "127.0.0.1", 0}}, broker2);
    auto c1 = cluster1.start();
    auto c2 = cluster2.start();
    ok &= c1.has_value() && c2.has_value();
    if (!ok) {
        std::printf("cluster start failed: c1=%s c2=%s\n", c1 ? "ok" : c1.error().c_str(),
                    c2 ? "ok" : c2.error().c_str());
        std::printf("FAIL\n");
        return 1;
    }
    cluster1.add_peer(quark::NodeId{2}, "127.0.0.1", cluster2.cluster_listen_port());
    cluster2.add_peer(quark::NodeId{1}, "127.0.0.1", cluster1.cluster_listen_port());

    const std::uint16_t port1 = broker1.listen_port();
    const std::uint16_t port2 = broker2.listen_port();

    // (1) cross-node hop: a subscriber on node 1, a DIFFERENT publisher on node 2.
    TestClient sub1;
    ok &= sub1.connect(port1, "sub-on-1");
    ok &= sub1.subscribe("cluster/topic");

    // (2) loop-prevention control: ALSO a subscriber on node 2 itself, same topic — must see node 2's
    // own local PUBLISH exactly once, never a relayed-back duplicate.
    TestClient sub2;
    ok &= sub2.connect(port2, "sub-on-2");
    ok &= sub2.subscribe("cluster/topic");

    TestClient pub2;
    ok &= pub2.connect(port2, "pub-on-2");
    ok &= pub2.publish("cluster/topic", "hello-from-2");

    auto got1 = sub1.wait_publish();
    ok &= got1.has_value() && got1->first == "cluster/topic" && got1->second == "hello-from-2";

    auto got2a = sub2.wait_publish();
    ok &= got2a.has_value() && got2a->first == "cluster/topic" && got2a->second == "hello-from-2";
    auto got2b = sub2.wait_publish(500);  // must NOT be delivered a second time (loop prevention)
    ok &= !got2b.has_value();

    // (3) M7.2 PR E: PUBLISH extras survive the cross-node relay hop — v5 subscriber on node 1, v5
    // publisher on node 2, same cross-node shape as (1) but asserting on Message Expiry Interval/Response
    // Topic/Correlation Data/User Property, not just topic/payload.
    TestClient sub1_ext;
    ok &= sub1_ext.connect(port1, "sub-ext-on-1", /*protocol_level=*/0x05);
    ok &= sub1_ext.subscribe("cluster/ext");

    TestClient pub2_ext;
    ok &= pub2_ext.connect(port2, "pub-ext-on-2", /*protocol_level=*/0x05);
    std::vector<std::byte> pub_props;
    put_prop_u32(pub_props, 0x02, 30);  // Message Expiry Interval = 30s
    put_prop_str(pub_props, 0x08, "reply/to/ext");
    const std::vector<std::byte> corr{std::byte{'a'}, std::byte{0x00}, std::byte{'b'}};
    put_prop_binary(pub_props, 0x09, corr);
    put_prop_str_pair(pub_props, 0x26, "k1", "v1");
    ok &= pub2_ext.publish("cluster/ext", "hello-ext-from-2", /*qos=*/1, pub_props);

    mqtt::ParsedProperties ext_props;
    auto got_ext = sub1_ext.wait_publish(2000, &ext_props);
    ok &= got_ext.has_value() && got_ext->first == "cluster/ext" && got_ext->second == "hello-ext-from-2";
    // Relative-to-absolute-to-relative round trip: forwarded as "seconds remaining as of the relay", so a
    // little wall-clock slack is expected — assert it's present and close to 30, not stale/dropped/missing.
    ok &= ext_props.message_expiry_interval.has_value() && *ext_props.message_expiry_interval > 25 &&
          *ext_props.message_expiry_interval <= 30;
    ok &= ext_props.response_topic.has_value() && *ext_props.response_topic == "reply/to/ext";
    ok &= ext_props.correlation_data.has_value() && *ext_props.correlation_data == corr;
    ok &= ext_props.user_properties.size() == 1 && ext_props.user_properties[0].first == "k1" &&
          ext_props.user_properties[0].second == "v1";
    if (!got_ext || !ext_props.message_expiry_interval || !ext_props.response_topic ||
        !ext_props.correlation_data || ext_props.user_properties.size() != 1) {
        std::printf("cross-node extras: assertion failed\n");
    }

    sub1.close();
    sub2.close();
    pub2.close();
    sub1_ext.close();
    pub2_ext.close();

    // Stop order matters (mirrors Runtime::~Runtime()'s M6 comment, runtime.hpp): the broker's accept
    // loop + session threads FIRST, so no locally-originated PUBLISH can still be mid-flight into
    // peer_forwarder_ once the cluster's own engine/transport starts tearing down underneath it.
    broker1.stop();
    broker2.stop();
    cluster1.stop();
    cluster2.stop();

    std::printf("broker_cluster: sub1=%s sub2_dup=%s %s\n", got1.has_value() ? "delivered" : "MISSING",
                got2b.has_value() ? "BUG-duplicate" : "none", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
