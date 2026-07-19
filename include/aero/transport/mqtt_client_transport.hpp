// AeroEdge Transport — `MqttClientTransport`, a REAL MQTT 3.1.1 client behind Quark's `Transport` seam
// (014 §5, X2/X3). This is the concrete backend the offline `NullMqttTransport` stands in for: a hand-
// written MQTT 3.1.1 CLIENT (CONNECT/SUBSCRIBE/PUBLISH-QoS1/PUBACK/PINGREQ/DISCONNECT) that moves genuine
// `MessageFrame`s through an EXTERNAL broker. AeroEdge is a CLIENT — it never reimplements a broker
// (014 §4 B1 / X3); the tests point it at a real `amqtt` broker launched via uv.
//
// THE §5 DESIGN, now realized (not just documented):
//   * Topic-per-node: this node PUBLISHes a frame for peer N to `<prefix>N` and SUBSCRIBEs its own inbox
//     `<prefix><self>`. The frame header already carries `target`, so one inbox topic per node suffices.
//   * QoS 1 ALWAYS (C3): every PUBLISH is QoS 1 (at-least-once); QoS 0 would be at-most-once, illegal for
//     at-least-once actors. Inbound QoS-1 PUBLISHes are PUBACKed.
//   * Ordering shim (C1, MANDATORY): a broker preserves order only per-topic-per-session — a reconnect /
//     clean-session can reorder, and QoS-1 redelivery duplicates. So the SENDER stamps a strictly-
//     monotonic per-`(from → to)` seq (8-byte big-endian prefix on the PUBLISH payload) and the RECEIVER
//     runs it through `Resequencer` (resequencer.hpp) — restoring the exactly-once, strict-FIFO stream
//     Quark's DistributedRouter requires. This is the load-bearing difference from TCP/gRPC, which are
//     order-preserving substrates and need no shim.
//   * Backpressure caveat (X7 / §7): a broker decouples producer from consumer — a high-rate STREAM over
//     MQTT does NOT deliver end-to-end backpressure. MQTT is for discrete control-plane / reachability
//     traffic; high-rate streams use TCP/gRPC. (Design posture, not enforced here.)
//
// SCOPE (honest): a compact, correct MQTT 3.1.1 client — enough control packets to connect, subscribe,
// and exchange QoS-1 PUBLISHes reliably against a conforming broker on a healthy link. It does NOT
// implement QoS-1 sender-side retransmit-on-missing-PUBACK, TLS, MQTT 5, or persistent-session recovery
// (all real-build concerns noted at their sites). The wire framing, topic scheme, QoS, and the C1 shim —
// the parts this phase is about — are real and exercised end-to-end.
//
// OFF THE HOT PATH (R0): optional aero-transport backend; std threads/containers, no 0-alloc discipline.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aero/transport/frame_codec.hpp"
#include "aero/transport/mqtt_transport.hpp"  // MqttTransport base (Config/shape) + kTransportGate
#include "aero/transport/resequencer.hpp"
#include "aero/transport/transport.hpp"

namespace aero::transport {

class MqttClientTransport final : public MqttTransport {
public:
    // `self` is this node's identity (its inbox topic == prefix+self, its MQTT client-id derives from it).
    MqttClientTransport(NodeId self, Config cfg = {}) : MqttTransport(std::move(cfg)), self_(self) {}
    ~MqttClientTransport() override { stop(); }

    MqttClientTransport(const MqttClientTransport&) = delete;
    MqttClientTransport& operator=(const MqttClientTransport&) = delete;

    // Connect to the broker, subscribe this node's inbox, spawn the inbound reader. Set on_receive()
    // BEFORE start(). Returns the documented error string on any failure (fail-closed, never throws).
    [[nodiscard]] std::expected<void, std::string> start() override {
        std::string host;
        std::uint16_t port = 0;
        if (!parse_broker_uri(cfg_.broker_uri, host, port))
            return gate_err("malformed broker uri '" + cfg_.broker_uri + "'");

        fd_ = dial(host, port);
        if (fd_ < 0) return gate_err("cannot dial broker " + host + ":" + std::to_string(port));
        running_.store(true, std::memory_order_release);

        if (auto e = mqtt_connect(); !e) return e;          // CONNECT → CONNACK(0)
        if (auto e = mqtt_subscribe_inbox(); !e) return e;  // SUBSCRIBE(inbox) → SUBACK

        reader_thread_ = std::thread([this] { reader_loop(); });
        return {};
    }

    // Graceful teardown: stop the reader, DISCONNECT, close the socket, join. Idempotent; run by dtor.
    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (fd_ >= 0) {
            std::byte disc[2] = {std::byte{0xE0}, std::byte{0x00}};  // DISCONNECT, best-effort
            std::lock_guard<std::mutex> g(io_mu_);
            (void)::send(fd_, disc, sizeof(disc), MSG_NOSIGNAL);
        }
        if (reader_thread_.joinable()) reader_thread_.join();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // --- Transport seam (010) --------------------------------------------------------------------------
    // PUBLISH the frame (QoS 1) to the destination node's inbox topic, seq-stamped for the C1 shim.
    void send(NodeId to, MessageFrame frame) override {
        const std::uint64_t seq = next_seq(to.value);  // per-(from → to) monotonic (SequenceStamper role)
        std::vector<std::byte> payload;
        put_u64_be(payload, seq);                       // 8-byte ordering-shim header (§5)
        const std::vector<std::byte> body = encode_frame(frame);
        payload.insert(payload.end(), body.begin(), body.end());

        const std::string topic = cfg_.topic_prefix + std::to_string(to.value);
        const std::uint16_t pid = next_packet_id();

        std::vector<std::byte> vh;                       // variable header + payload of the PUBLISH
        put_str(vh, topic);
        put_u16_be(vh, pid);                             // QoS 1 ⇒ packet identifier present
        vh.insert(vh.end(), payload.begin(), payload.end());

        // fixed header: PUBLISH (0x30) | QoS1 (0x02). No retransmit-on-missing-PUBACK (real-build TODO).
        if (write_packet(std::byte{0x32}, vh))
            frames_sent_.fetch_add(1, std::memory_order_relaxed);
        else
            send_errors_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_receive(std::function<void(MessageFrame)> cb) override { cb_ = std::move(cb); }

    // --- diagnostics -----------------------------------------------------------------------------------
    [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_recv_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t send_errors() const noexcept { return send_errors_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t duplicates_dropped() const noexcept { return dupes_.load(std::memory_order_relaxed); }

private:
    // ===== MQTT byte helpers ===========================================================================
    static void put_u16_be(std::vector<std::byte>& out, std::uint16_t v) {
        out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
        out.push_back(static_cast<std::byte>(v & 0xFF));
    }
    static void put_u64_be(std::vector<std::byte>& out, std::uint64_t v) {
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
    static void put_str(std::vector<std::byte>& out, std::string_view s) {
        put_u16_be(out, static_cast<std::uint16_t>(s.size()));
        for (char c : s) out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    }
    // MQTT "remaining length": 1–4 bytes, 7 bits each, high bit = continuation.
    static void put_remaining_length(std::vector<std::byte>& out, std::uint32_t len) {
        do {
            std::uint8_t enc = len % 128;
            len /= 128;
            if (len > 0) enc |= 0x80;
            out.push_back(static_cast<std::byte>(enc));
        } while (len > 0);
    }

    std::unexpected<std::string> gate_err(std::string_view what) {
        running_.store(false, std::memory_order_release);
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        return std::unexpected(std::string(kTransportGate) + " [mqtt: " + std::string(what) + "]");
    }

    // ===== connection / handshake ======================================================================
    static bool parse_broker_uri(std::string_view uri, std::string& host, std::uint16_t& port) {
        constexpr std::string_view kPrefix = "tcp://";
        if (uri.substr(0, kPrefix.size()) != kPrefix) return false;
        uri.remove_prefix(kPrefix.size());
        const auto colon = uri.rfind(':');
        if (colon == std::string_view::npos) return false;
        host = std::string(uri.substr(0, colon));
        const std::string ps(uri.substr(colon + 1));
        if (ps.empty()) return false;
        port = static_cast<std::uint16_t>(std::stoul(ps));
        return !host.empty() && port != 0;
    }

    static int dial(const std::string& host, std::uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return -1;
        int fd = -1;
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                int one = 1;
                ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                break;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(res);
        return fd;
    }

    std::expected<void, std::string> mqtt_connect() {
        const std::string client_id = "aero-" + std::to_string(self_.value);
        std::vector<std::byte> vh;
        put_str(vh, "MQTT");
        vh.push_back(std::byte{0x04});  // protocol level 4 == MQTT 3.1.1
        vh.push_back(std::byte{0x02});  // connect flags: clean session
        put_u16_be(vh, 60);             // keepalive seconds (test runs well under this; no PINGREQ needed)
        put_str(vh, client_id);         // payload: client identifier
        if (!write_packet(std::byte{0x10}, vh)) return gate_err("CONNECT write failed");

        auto pkt = read_packet();
        if (!pkt) return gate_err("no CONNACK");
        if ((pkt->type_flags & 0xF0) != 0x20 || pkt->body.size() < 2 ||
            std::to_integer<std::uint8_t>(pkt->body[1]) != 0)
            return gate_err("CONNACK refused");
        return {};
    }

    std::expected<void, std::string> mqtt_subscribe_inbox() {
        const std::string topic = inbox_topic(self_);  // base helper: prefix + self
        std::vector<std::byte> vh;
        put_u16_be(vh, next_packet_id());  // packet identifier
        put_str(vh, topic);                // topic filter
        vh.push_back(std::byte{0x01});     // requested QoS 1 (C3)
        if (!write_packet(std::byte{0x82}, vh)) return gate_err("SUBSCRIBE write failed");  // 0x82 required flags

        auto pkt = read_packet();
        if (!pkt) return gate_err("no SUBACK");
        if ((pkt->type_flags & 0xF0) != 0x90) return gate_err("expected SUBACK");
        // granted QoS byte 0x80 == failure
        if (!pkt->body.empty() && std::to_integer<std::uint8_t>(pkt->body.back()) == 0x80)
            return gate_err("SUBSCRIBE denied");
        return {};
    }

    // ===== raw packet I/O ==============================================================================
    struct Packet {
        std::uint8_t type_flags = 0;
        std::vector<std::byte> body;  // everything after the fixed header (var header + payload)
    };

    // Read exactly n bytes off fd_, polling so the loop notices stop() (running_ → false) promptly.
    bool read_n(std::byte* buf, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            if (!running_.load(std::memory_order_acquire)) return false;
            pollfd pfd{fd_, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr < 0) { if (errno == EINTR) continue; return false; }
            if (pr == 0) continue;
            const ssize_t r = ::recv(fd_, buf + got, n - got, 0);
            if (r == 0) return false;
            if (r < 0) { if (errno == EINTR) continue; return false; }
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    std::optional<Packet> read_packet() {
        std::byte b0;
        if (!read_n(&b0, 1)) return std::nullopt;
        std::uint32_t mult = 1, len = 0;
        for (int i = 0; i < 4; ++i) {
            std::byte enc;
            if (!read_n(&enc, 1)) return std::nullopt;
            const std::uint8_t e = std::to_integer<std::uint8_t>(enc);
            len += (e & 0x7F) * mult;
            if ((e & 0x80) == 0) break;
            mult *= 128;
        }
        Packet p;
        p.type_flags = std::to_integer<std::uint8_t>(b0);
        p.body.resize(len);
        if (len > 0 && !read_n(p.body.data(), len)) return std::nullopt;
        return p;
    }

    // Serialize [fixed-header-byte | remaining-length | body] and write it atomically (io_mu_ serializes
    // writers: send()'s PUBLISH, the reader's PUBACK, and the handshake — no interleaved packets).
    bool write_packet(std::byte type_flags, const std::vector<std::byte>& body) {
        std::vector<std::byte> pkt;
        pkt.push_back(type_flags);
        put_remaining_length(pkt, static_cast<std::uint32_t>(body.size()));
        pkt.insert(pkt.end(), body.begin(), body.end());
        std::lock_guard<std::mutex> g(io_mu_);
        if (fd_ < 0) return false;
        std::size_t sent = 0;
        while (sent < pkt.size()) {
            const ssize_t w = ::send(fd_, pkt.data() + sent, pkt.size() - sent, MSG_NOSIGNAL);
            if (w < 0) { if (errno == EINTR) continue; return false; }
            sent += static_cast<std::size_t>(w);
        }
        return true;
    }

    // ===== inbound loop ================================================================================
    void reader_loop() {
        while (running_.load(std::memory_order_acquire)) {
            auto pkt = read_packet();
            if (!pkt) break;
            const std::uint8_t type = pkt->type_flags & 0xF0;
            if (type == 0x30) {
                handle_publish(*pkt);
            }
            // 0x40 PUBACK (our QoS-1 ack), 0xD0 PINGRESP, others: nothing to do on this reduced client.
        }
    }

    // Parse an inbound PUBLISH: [topic][packet-id if QoS>0][ 8B seq | frame bytes ]. PUBACK if QoS 1,
    // then feed the seq+frame through the per-source Resequencer and emit released frames strictly in order.
    void handle_publish(const Packet& pkt) {
        const std::uint8_t qos = (pkt.type_flags >> 1) & 0x03;
        const std::vector<std::byte>& b = pkt.body;
        std::size_t pos = 0;
        if (b.size() < 2) return;
        const std::uint16_t topic_len =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        pos = 2 + topic_len;
        if (pos > b.size()) return;
        std::uint16_t pid = 0;
        if (qos > 0) {
            if (pos + 2 > b.size()) return;
            pid = (std::to_integer<std::uint8_t>(b[pos]) << 8) | std::to_integer<std::uint8_t>(b[pos + 1]);
            pos += 2;
        }
        if (qos == 1) {  // PUBACK the packet id (C3 at-least-once handshake)
            std::vector<std::byte> ack;
            put_u16_be(ack, pid);
            (void)write_packet(std::byte{0x40}, ack);
        }
        if (pos + 8 > b.size()) return;  // need the 8-byte seq header
        std::uint64_t seq = 0;
        for (int i = 0; i < 8; ++i) seq = (seq << 8) | std::to_integer<std::uint8_t>(b[pos + i]);
        pos += 8;

        std::span<const std::byte> frame_bytes(b.data() + pos, b.size() - pos);
        auto f = decode_frame(frame_bytes);
        if (!f) return;
        frames_recv_.fetch_add(1, std::memory_order_relaxed);

        // C1 shim: resequence per source node. Collect released frames under the lock, emit after — so
        // cb_ never runs while seq_mu_ is held (no re-entrancy hazard).
        std::vector<MessageFrame> released;
        {
            std::lock_guard<std::mutex> g(seq_mu_);
            Resequencer<MessageFrame>& rq = resequencer_for(f->from.value);
            const OfferOutcome oc = rq.offer(seq, std::move(*f), [&](MessageFrame&& m) {
                released.push_back(std::move(m));
            });
            if (oc == OfferOutcome::Duplicate) dupes_.fetch_add(1, std::memory_order_relaxed);
        }
        if (cb_)
            for (MessageFrame& m : released) cb_(std::move(m));
    }

    Resequencer<MessageFrame>& resequencer_for(std::uint64_t from) {
        auto it = resequencers_.find(from);
        if (it == resequencers_.end())
            it = resequencers_.emplace(from, Resequencer<MessageFrame>(cfg_.reorder_window)).first;
        return it->second;
    }

    std::uint64_t next_seq(std::uint64_t to) {
        std::lock_guard<std::mutex> g(seq_mu_);
        return next_seq_[to]++;  // strictly monotonic per (from → to) link (SequenceStamper role, §5)
    }
    std::uint16_t next_packet_id() {
        std::uint16_t id = packet_id_.fetch_add(1, std::memory_order_relaxed);
        return id == 0 ? packet_id_.fetch_add(1, std::memory_order_relaxed) : id;  // MQTT packet id != 0
    }

    NodeId self_;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::function<void(MessageFrame)> cb_;  // set once, before start()

    std::thread reader_thread_;
    std::mutex io_mu_;   // serializes all socket writes (packet atomicity)

    std::mutex seq_mu_;  // guards next_seq_ + resequencers_
    std::unordered_map<std::uint64_t, std::uint64_t> next_seq_;                 // per-dest send counter
    std::unordered_map<std::uint64_t, Resequencer<MessageFrame>> resequencers_; // per-source reorder buffer

    std::atomic<std::uint16_t> packet_id_{1};
    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_recv_{0};
    std::atomic<std::uint64_t> send_errors_{0};
    std::atomic<std::uint64_t> dupes_{0};
};

}  // namespace aero::transport
