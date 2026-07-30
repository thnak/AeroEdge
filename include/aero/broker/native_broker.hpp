// AeroEdge Broker — `NativeBroker`, AeroEdge's own embedded MQTT 3.1.1 server (017, Phase 1).
//
// WHY THIS EXISTS: EMQX moved to the Business Source License 1.1 — single-node production use is free,
// but clustering AND embedding into a shipped product both require a paid EMQ license, which is exactly
// AeroEdge's deployment shape. 017 §1. This is the broker AeroEdge owns the license to.
//
// ROLE (017 §2): AeroEdge is the MQTT SERVER here — devices/PLCs dial in directly ("southbound
// termination"). This is distinct from `MqttClientTransport` (014 §5, AeroEdge as a CLIENT to an
// EXTERNAL broker for inter-actor transport) and the (future) MQTT driver (006, AeroEdge as a CLIENT
// subscribing to an external broker for device ingestion) — those are unaffected by this file.
//
// PHASE 1 SCOPE (honest, 017 §"Phase 1 scope"): single-node only — a PUBLISH is delivered to
// subscribers connected to THIS broker instance, nothing more. Cross-node topic routing over Quark's
// DistributedRouter (017 §4), feeding PUBLISHed data into Flows/Drivers, MES reporting, TLS, and
// Quark 020 authorization are all explicitly deferred (see spec Open Questions / follow-on phases) —
// `on_publish()` is the entire ingestion seam for now, exactly like the client transports' honest
// scope banners already do for their own deferred pieces (QoS-1 retransmit, TLS, etc.).
//
// QoS 0/1 only (no QoS 2), MQTT 3.1.1 CONNECT/SUBSCRIBE/PUBLISH/PUBACK/PINGREQ/DISCONNECT, `+`/`#`
// wildcard subscriptions, retained messages. No authentication (v1 is a trusted-network broker).
//
// PORTABILITY / REUSE: built on quark::pal::net + aero::pal::poll (this session's PAL work) and the
// MQTT wire codec shared with `MqttClientTransport` (mqtt_codec.hpp, 017 §9 N3) — no new socket or
// framing code, only the server-side session state machine and topic routing are new.
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pal/net.hpp"  // quark::pal::* — fd_t, tcp_listen/accept_one/recv_some/send_some/...

#if !defined(_WIN32)
#include <arpa/inet.h>  // inet_pton (Windows: transitively via pal/net.hpp's ws2tcpip.h)
#endif

#include "aero/pal/poll.hpp"
#include "aero/transport/mqtt_codec.hpp"

namespace aero::broker {

struct Config {
    std::string bind_host = "0.0.0.0";  // interface to listen on
    std::uint16_t listen_port = 1883;   // MQTT's conventional plaintext port; 0 => ephemeral
    int backlog = 64;
};

// MQTT topic-filter matching (3.1.1 §4.7): `+` matches exactly one level, `#` matches the rest of the
// topic (including zero further levels) and must be the filter's last level to have any effect beyond
// literal comparison. Pure function, independently testable.
[[nodiscard]] inline bool topic_matches(std::string_view filter, std::string_view topic) noexcept {
    auto split = [](std::string_view s) {
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        for (;;) {
            const auto pos = s.find('/', start);
            if (pos == std::string_view::npos) {
                parts.push_back(s.substr(start));
                break;
            }
            parts.push_back(s.substr(start, pos - start));
            start = pos + 1;
        }
        return parts;
    };
    const auto f = split(filter);
    const auto t = split(topic);
    std::size_t i = 0;
    for (; i < f.size(); ++i) {
        if (f[i] == "#") return true;         // matches everything remaining, including zero levels
        if (i >= t.size()) return false;      // filter has more levels than topic, and it's not '#'
        if (f[i] == "+") continue;            // matches exactly this one level
        if (f[i] != t[i]) return false;
    }
    return i == t.size();  // no '#' consumed the tail => topic must end exactly where the filter does
}

class NativeBroker {
public:
    explicit NativeBroker(Config cfg) : cfg_(std::move(cfg)) {}
    ~NativeBroker() { stop(); }

    NativeBroker(const NativeBroker&) = delete;
    NativeBroker& operator=(const NativeBroker&) = delete;

    // Bind + listen + spawn the accept loop. Set on_publish() BEFORE start() so no early PUBLISH races
    // an unset sink. Returns the documented error string on any socket failure (fail-closed).
    [[nodiscard]] std::expected<void, std::string> start() {
        ::in_addr bind_addr{};
        if (::inet_pton(AF_INET, cfg_.bind_host.c_str(), &bind_addr) != 1)
            return std::unexpected("native broker: invalid bind_host '" + cfg_.bind_host + "'");
        const std::uint64_t addr_u64 = ::ntohl(bind_addr.s_addr);

        auto lfd = quark::pal::tcp_listen(addr_u64, cfg_.listen_port, cfg_.backlog);
        if (!lfd) return std::unexpected("native broker: tcp_listen failed: " + lfd.error().message());
        listen_fd_ = *lfd;

        if (auto p = quark::pal::local_port(listen_fd_)) resolved_port_ = *p;

        running_.store(true, std::memory_order_release);
        accept_thread_ = std::thread([this] { accept_loop(); });
        return {};
    }

    // Stop the accept loop, close every session + the listener, join every thread. Idempotent.
    // ORDER MATTERS (TSan-clean, mirrors tcp_transport.hpp): flip running_ false, join the accept
    // thread FIRST (it exits within one poll timeout and stops spawning new sessions), then close the
    // listener, then join every session thread (each notices running_ within its own poll timeout).
    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (accept_thread_.joinable()) accept_thread_.join();
        if (listen_fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(listen_fd_);
            listen_fd_ = quark::pal::invalid_fd;
        }
        {
            std::lock_guard<std::mutex> g(sessions_mu_);
            for (std::thread& t : session_threads_)
                if (t.joinable()) t.join();
            session_threads_.clear();
            sessions_.clear();
        }
    }

    [[nodiscard]] std::uint16_t listen_port() const noexcept { return resolved_port_; }

    // Phase 1's entire ingestion seam (017 §"Phase 1 scope"): fires for every PUBLISH this broker
    // instance handles (whether or not any subscriber is present), from whichever session's reader
    // thread received it. Callers needing thread-safe hand-off to a flow/actor must marshal it
    // themselves — mirrors how IDriver::run()'s producer thread hands frames to a bridge thread today.
    void on_publish(std::function<void(std::string_view topic, std::span<const std::byte> payload,
                                       std::uint8_t qos)> cb) {
        on_publish_ = std::move(cb);
    }

private:
    struct Subscription {
        std::string filter;
        std::uint8_t qos = 0;  // granted QoS (v1: min(requested, 1))
    };

    // One Session per accepted connection. io_mu_ serializes every write to `fd` — a PUBLISH fanned
    // out to this session from ANOTHER session's reader thread must not interleave on the wire with
    // this session's own SUBACK/PINGRESP/PUBACK writes.
    struct Session {
        explicit Session(quark::pal::fd_t f) : fd(f) {}
        quark::pal::fd_t fd;
        std::mutex io_mu;
        std::mutex subs_mu;
        std::vector<Subscription> subs;

        bool send_packet(std::byte type_flags, const std::vector<std::byte>& body) {
            std::lock_guard<std::mutex> g(io_mu);
            return aero::transport::mqtt::write_packet(fd, type_flags, body);
        }
    };

    void accept_loop() {
        while (running_.load(std::memory_order_acquire)) {
            const auto ready = aero::pal::wait_readable(listen_fd_, 200);
            if (!ready || !*ready) continue;  // timeout/err → re-check running_
            auto cfd = quark::pal::accept_one(listen_fd_);
            if (!cfd) continue;
            auto session = std::make_shared<Session>(*cfd);
            std::lock_guard<std::mutex> g(sessions_mu_);
            sessions_.push_back(session);
            session_threads_.emplace_back([this, session] { session_loop(session); });
        }
    }

    void session_loop(const std::shared_ptr<Session>& s) {
        while (running_.load(std::memory_order_acquire)) {
            auto pkt = aero::transport::mqtt::read_packet(s->fd, running_);
            if (!pkt) break;  // peer closed / error / running flipped false
            const std::uint8_t type = pkt->type_flags & 0xF0;
            if (type == 0x10) {          // CONNECT
                if (!handle_connect(*s)) break;
            } else if (type == 0x80) {   // SUBSCRIBE
                if (!handle_subscribe(*s, *pkt)) break;
            } else if (type == 0x30) {   // PUBLISH
                if (!handle_publish(*s, *pkt)) break;
            } else if (type == 0xC0) {   // PINGREQ
                if (!s->send_packet(std::byte{0xD0}, {})) break;  // PINGRESP, no body
            } else if (type == 0xE0) {   // DISCONNECT
                break;
            }
            // 0x40 PUBACK from the peer (ack of a QoS-1 delivery we sent): no retry state kept in v1
            // (mirrors MqttClientTransport's own honest "no QoS-1 sender-side retransmit" scope) — read
            // and discard, already consumed by read_packet above.
        }
        remove_session(s);
        quark::pal::close_fd(s->fd);
    }

    bool handle_connect(Session& s) {
        std::vector<std::byte> body{std::byte{0x00}, std::byte{0x00}};  // session-present=0, rc=0 (accepted)
        return s.send_packet(std::byte{0x20}, body);                   // CONNACK
    }

    bool handle_subscribe(Session& s, const aero::transport::mqtt::Packet& pkt) {
        const std::vector<std::byte>& b = pkt.body;
        if (b.size() < 2) return false;
        const std::uint16_t packet_id =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        std::size_t pos = 2;
        std::vector<Subscription> added;
        std::vector<std::byte> granted;  // SUBACK return codes, one per filter, in order
        while (pos + 2 <= b.size()) {
            const std::uint16_t flen =
                (std::to_integer<std::uint8_t>(b[pos]) << 8) | std::to_integer<std::uint8_t>(b[pos + 1]);
            pos += 2;
            if (pos + flen + 1 > b.size()) break;  // malformed — stop, ack whatever parsed so far
            std::string filter(reinterpret_cast<const char*>(b.data() + pos), flen);
            pos += flen;
            const std::uint8_t requested_qos = std::to_integer<std::uint8_t>(b[pos]);
            pos += 1;
            const std::uint8_t qos = requested_qos > 1 ? 1 : requested_qos;  // v1 ceiling: QoS 1
            added.push_back(Subscription{filter, qos});
            granted.push_back(static_cast<std::byte>(qos));
        }
        {
            // Copy, not move — `added` is still read below for the retained-message replay.
            std::lock_guard<std::mutex> g(s.subs_mu);
            for (const Subscription& sub : added) s.subs.push_back(sub);
        }
        std::vector<std::byte> vh;
        aero::transport::mqtt::put_u16_be(vh, packet_id);
        vh.insert(vh.end(), granted.begin(), granted.end());
        if (!s.send_packet(std::byte{0x90}, vh)) return false;  // SUBACK

        // Retained-message replay (3.1.1 §3.8.4): a new matching SUBSCRIBE gets the retained payload
        // immediately, at the subscription's granted QoS.
        std::lock_guard<std::mutex> rg(retained_mu_);
        for (const Subscription& sub : added) {
            for (const auto& [topic, payload] : retained_) {
                if (topic_matches(sub.filter, topic)) {
                    if (!publish_to(s, topic, payload, sub.qos, /*retain=*/true)) return false;
                }
            }
        }
        return true;
    }

    bool handle_publish(Session& s, const aero::transport::mqtt::Packet& pkt) {
        const std::uint8_t qos = (pkt.type_flags >> 1) & 0x03;
        const bool retain = (pkt.type_flags & 0x01) != 0;
        const std::vector<std::byte>& b = pkt.body;
        if (b.size() < 2) return false;
        const std::uint16_t topic_len =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        std::size_t pos = 2 + topic_len;
        if (pos > b.size()) return false;
        const std::string topic(reinterpret_cast<const char*>(b.data() + 2), topic_len);

        std::uint16_t packet_id = 0;
        if (qos > 0) {
            if (pos + 2 > b.size()) return false;
            packet_id = (std::to_integer<std::uint8_t>(b[pos]) << 8) | std::to_integer<std::uint8_t>(b[pos + 1]);
            pos += 2;
        }

        std::vector<std::byte> payload(b.begin() + static_cast<std::ptrdiff_t>(pos), b.end());

        // Retention is stored (and, below, fanned out to subscribers) BEFORE the PUBACK is sent, not
        // after — a publisher that has received its PUBACK must be able to assume a subsequent SUBSCRIBE
        // from anyone will already see this retained value / delivery (no ack-before-visible race).
        if (retain) {
            std::lock_guard<std::mutex> g(retained_mu_);
            if (payload.empty())
                retained_.erase(topic);  // zero-length retained PUBLISH clears retention (3.1.1 §3.3.1.3)
            else
                retained_[topic] = payload;
        }

        if (qos > 0) {
            std::vector<std::byte> ack;
            aero::transport::mqtt::put_u16_be(ack, packet_id);
            if (!s.send_packet(std::byte{0x40}, ack)) return false;  // PUBACK
        }

        if (on_publish_) on_publish_(topic, payload, qos);
        route_publish(topic, payload, qos);
        return true;
    }

    // Fan out to every connected session with a matching subscription, at min(publish qos, granted
    // qos) — a snapshot copy of sessions_ is taken under the lock so the actual socket writes (which
    // may block briefly on backpressure) never happen while holding sessions_mu_.
    void route_publish(const std::string& topic, const std::vector<std::byte>& payload, std::uint8_t qos) {
        std::vector<std::shared_ptr<Session>> snapshot;
        {
            std::lock_guard<std::mutex> g(sessions_mu_);
            snapshot = sessions_;
        }
        for (const auto& session : snapshot) {
            std::vector<Subscription> matches;
            {
                std::lock_guard<std::mutex> g(session->subs_mu);
                for (const Subscription& sub : session->subs)
                    if (topic_matches(sub.filter, topic)) matches.push_back(sub);
            }
            for (const Subscription& sub : matches) {
                const std::uint8_t deliver_qos = qos < sub.qos ? qos : sub.qos;
                (void)publish_to(*session, topic, payload, deliver_qos, /*retain=*/false);
            }
        }
    }

    bool publish_to(Session& s, const std::string& topic, const std::vector<std::byte>& payload,
                    std::uint8_t qos, bool retain) {
        std::vector<std::byte> vh;
        aero::transport::mqtt::put_str(vh, topic);
        if (qos > 0) aero::transport::mqtt::put_u16_be(vh, next_packet_id());
        vh.insert(vh.end(), payload.begin(), payload.end());
        std::uint8_t flags = static_cast<std::uint8_t>(qos << 1);
        if (retain) flags |= 0x01;
        return s.send_packet(static_cast<std::byte>(0x30 | flags), vh);
    }

    std::uint16_t next_packet_id() {
        const std::uint16_t id = packet_id_.fetch_add(1, std::memory_order_relaxed);
        return id == 0 ? packet_id_.fetch_add(1, std::memory_order_relaxed) : id;  // MQTT packet id != 0
    }

    void remove_session(const std::shared_ptr<Session>& s) {
        std::lock_guard<std::mutex> g(sessions_mu_);
        std::erase(sessions_, s);
    }

    Config cfg_;
    quark::pal::fd_t listen_fd_ = quark::pal::invalid_fd;
    std::uint16_t resolved_port_ = 0;
    std::atomic<bool> running_{false};

    std::thread accept_thread_;
    std::mutex sessions_mu_;
    std::vector<std::shared_ptr<Session>> sessions_;
    std::vector<std::thread> session_threads_;

    std::mutex retained_mu_;
    std::unordered_map<std::string, std::vector<std::byte>> retained_;

    std::atomic<std::uint16_t> packet_id_{1};
    std::function<void(std::string_view, std::span<const std::byte>, std::uint8_t)> on_publish_;
};

}  // namespace aero::broker
