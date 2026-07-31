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

// Minimal hand-rolled MQTT client — connect/subscribe/publish/wait_publish (QoS 1) only, slimmed from
// tests/broker/native_broker.cpp's TestClient (that file's version is anonymous-namespace-local, not a
// reusable header; a small duplication here is the pragmatic call given this test's narrow needs).
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
        vh.push_back(std::byte{0x02});  // connect flags: clean session, no Will
        mqtt::put_u16_be(vh, 60);       // keep-alive (s)
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

    // Waits for an inbound PUBLISH; PUBACKs it if QoS 1. nullopt on timeout — the expected, correct
    // outcome for the "must NOT see a second copy" loop-prevention assertion.
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

    sub1.close();
    sub2.close();
    pub2.close();

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
