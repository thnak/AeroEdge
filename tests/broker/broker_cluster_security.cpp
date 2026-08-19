// 017 M5.1 gate (§10) — BrokerCluster's optional mTLS cluster-link (broker_cluster_security.hpp) over
// REAL loopback TCP, mirroring tests/broker/broker_cluster.cpp's own "two real nodes over actual
// sockets" shape and reusing its TestClient verbatim (same narrow connect/subscribe/publish/wait_publish
// surface, no need to duplicate the MQTT wire idiom).
//
// Proves, over a real loopback TCP inter-broker link with mTLS enabled:
//   (1) a cross-node PUBLISH still crosses correctly (same M6 assertion broker_cluster.cpp makes) AND
//       the link actually SEALED/OPENED real frames (SecureTransport::sealed()/opened() > 0 on both
//       sides) — proving encryption is genuinely active on the wire, not merely "config accepted, still
//       happened to work over the plain path" (which would also pass a naive "PUBLISH arrived" check).
//   (2) a cluster_id mismatch between two otherwise-validly-certificated peers fails the mTLS handshake
//       (SecureTransport::install_session_from_result()'s identity cross-check) — a PUBLISH sent while
//       peers disagree on cluster_id never crosses, proving auth is genuinely enforced, not just that
//       encryption exists.
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
using aero::broker::BrokerClusterSecurityConfig;
using aero::broker::Config;
using aero::broker::NativeBroker;
using aero::broker::PeerSpec;

namespace {

const std::string kCertsDir = AERO_CLUSTER_TEST_CERTS_DIR;  // set by tests/CMakeLists.txt

// Identical to broker_cluster.cpp's own TestClient (see that file's banner for why a small duplication
// here is the pragmatic call) — connect/subscribe/publish/wait_publish (QoS 1) only.
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
        vh.push_back(std::byte{0x04});
        vh.push_back(std::byte{0x02});
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

    [[nodiscard]] bool publish(const std::string& topic, const std::string& payload, std::uint8_t qos = 1) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        const std::uint8_t flags = static_cast<std::uint8_t>(qos << 1);
        if (!mqtt::write_packet(fd_, static_cast<std::byte>(0x30 | flags), vh)) return false;
        return qos == 0 || wait_for(0x40, 2000).has_value();
    }

    [[nodiscard]] std::optional<std::pair<std::string, std::string>> wait_publish(int timeout_ms = 2000) {
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

BrokerClusterSecurityConfig node_security(quark::ClusterId cluster_id, const std::string& node_cert) {
    BrokerClusterSecurityConfig sec;
    sec.cluster_id = cluster_id;
    sec.certificate_chain_file = kCertsDir + "/" + node_cert + "_cert.der";
    sec.private_key_file = kCertsDir + "/" + node_cert + "_key.der";
    sec.trusted_roots_file = kCertsDir + "/ca_cert.der";
    return sec;
}

// ---- (1) real cross-node PUBLISH over a genuinely sealed/opened mTLS link -------------------------
bool test_secure_cross_node_publish() {
    bool ok = true;

    NativeBroker broker1(Config{"127.0.0.1", /*listen_port=*/0});
    NativeBroker broker2(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker1.start().has_value();
    ok &= broker2.start().has_value();
    if (!ok) {
        std::printf("secure_cross_node_publish: broker start failed\n");
        return false;
    }

    const quark::ClusterId cluster_id{1};
    BrokerCluster cluster1(quark::NodeId{1}, /*cluster_port=*/0,
                          {PeerSpec{quark::NodeId{2}, "127.0.0.1", 0}}, broker1,
                          node_security(cluster_id, "node1"));
    BrokerCluster cluster2(quark::NodeId{2}, /*cluster_port=*/0,
                          {PeerSpec{quark::NodeId{1}, "127.0.0.1", 0}}, broker2,
                          node_security(cluster_id, "node2"));
    auto c1 = cluster1.start();
    auto c2 = cluster2.start();
    ok &= c1.has_value() && c2.has_value();
    if (!ok) {
        std::printf("secure_cross_node_publish: cluster start failed: c1=%s c2=%s\n",
                    c1 ? "ok" : c1.error().c_str(), c2 ? "ok" : c2.error().c_str());
        broker1.stop();
        broker2.stop();
        return false;
    }
    cluster1.add_peer(quark::NodeId{2}, "127.0.0.1", cluster2.cluster_listen_port());
    cluster2.add_peer(quark::NodeId{1}, "127.0.0.1", cluster1.cluster_listen_port());

    const std::uint16_t port1 = broker1.listen_port();
    const std::uint16_t port2 = broker2.listen_port();

    TestClient sub1;
    ok &= sub1.connect(port1, "sub-on-1");
    ok &= sub1.subscribe("cluster/topic");

    TestClient pub2;
    ok &= pub2.connect(port2, "pub-on-2");

    // The FIRST relay attempt for a peer with no existing session kicks off an async mTLS handshake and
    // DROPS that one frame (SecureTransport's own S2 "no session -> no delivery" — a fresh publish per
    // retry, not a resend, since NativeBroker's own PUBLISH plumbing has no relay-retry of its own).
    // Bounded retry until the handshake completes and a publish actually gets sealed through.
    std::optional<std::pair<std::string, std::string>> got1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !got1.has_value()) {
        ok &= pub2.publish("cluster/topic", "hello-secure");
        got1 = sub1.wait_publish(300);
    }
    ok &= got1.has_value() && got1->first == "cluster/topic" && got1->second == "hello-secure";
    if (!got1.has_value()) std::printf("secure_cross_node_publish: PUBLISH never crossed the link\n");

    // The whole point of this test over broker_cluster.cpp's own: prove the link genuinely sealed/
    // opened real frames, not just that the (also-correct) plain path happened to still work.
    const bool node2_sealed = cluster2.cluster_link_frames_sealed() > 0;
    const bool node1_opened = cluster1.cluster_link_frames_opened() > 0;
    ok &= node2_sealed && node1_opened;
    if (!node2_sealed || !node1_opened) {
        std::printf("secure_cross_node_publish: link not actually sealed/opened (sealed=%llu opened=%llu)\n",
                    static_cast<unsigned long long>(cluster2.cluster_link_frames_sealed()),
                    static_cast<unsigned long long>(cluster1.cluster_link_frames_opened()));
    }

    sub1.close();
    pub2.close();
    broker1.stop();
    broker2.stop();
    cluster1.stop();
    cluster2.stop();

    if (!ok) std::printf("secure_cross_node_publish: assertion failed\n");
    return ok;
}

// ---- (2) a cluster_id mismatch fails the handshake -> PUBLISH never crosses -------------------------
bool test_cluster_id_mismatch_rejected() {
    bool ok = true;

    NativeBroker broker1(Config{"127.0.0.1", /*listen_port=*/0});
    NativeBroker broker2(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker1.start().has_value();
    ok &= broker2.start().has_value();
    if (!ok) {
        std::printf("cluster_id_mismatch: broker start failed\n");
        return false;
    }

    // Valid certs on both sides (signed by the same CA), but DIFFERENT cluster_id configured locally —
    // the handshake's own identity cross-check (result.peer_cluster_id == cluster_id_) must reject this.
    BrokerCluster cluster1(quark::NodeId{1}, /*cluster_port=*/0,
                          {PeerSpec{quark::NodeId{2}, "127.0.0.1", 0}}, broker1,
                          node_security(quark::ClusterId{1}, "node1"));
    BrokerCluster cluster2(quark::NodeId{2}, /*cluster_port=*/0,
                          {PeerSpec{quark::NodeId{1}, "127.0.0.1", 0}}, broker2,
                          node_security(quark::ClusterId{2}, "node2"));
    auto c1 = cluster1.start();
    auto c2 = cluster2.start();
    ok &= c1.has_value() && c2.has_value();
    if (!ok) {
        std::printf("cluster_id_mismatch: cluster start failed\n");
        broker1.stop();
        broker2.stop();
        return false;
    }
    cluster1.add_peer(quark::NodeId{2}, "127.0.0.1", cluster2.cluster_listen_port());
    cluster2.add_peer(quark::NodeId{1}, "127.0.0.1", cluster1.cluster_listen_port());

    const std::uint16_t port2 = broker2.listen_port();

    TestClient sub1;
    ok &= sub1.connect(broker1.listen_port(), "sub-on-1");
    ok &= sub1.subscribe("cluster/topic");

    TestClient pub2;
    ok &= pub2.connect(port2, "pub-on-2");
    ok &= pub2.publish("cluster/topic", "should-not-cross");

    // Bounded wait: this must NEVER arrive — a mismatched cluster_id keeps failing the handshake, so
    // send()'s own S2 "no session -> no delivery" drop applies to every relay attempt.
    auto got1 = sub1.wait_publish(2000);
    ok &= !got1.has_value();
    if (got1.has_value()) {
        std::printf("cluster_id_mismatch: PUBLISH crossed despite a cluster_id mismatch (BUG)\n");
    }

    sub1.close();
    pub2.close();
    broker1.stop();
    broker2.stop();
    cluster1.stop();
    cluster2.stop();

    if (!ok) std::printf("cluster_id_mismatch: assertion failed\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    const bool secure_ok = test_secure_cross_node_publish();
    ok &= secure_ok;
    std::printf("[secure_cross_node_publish] %s\n", secure_ok ? "ok" : "FAIL");

    const bool mismatch_ok = test_cluster_id_mismatch_rejected();
    ok &= mismatch_ok;
    std::printf("[cluster_id_mismatch_rejected] %s\n", mismatch_ok ? "ok" : "FAIL");

    std::printf("broker_cluster_security: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
