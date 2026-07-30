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
// MQTT 3.1.1 CONNECT/SUBSCRIBE/PUBLISH/PUBACK/PUBREC/PUBREL/PUBCOMP/PINGREQ/DISCONNECT, QoS 0/1/2, `+`/`#`
// wildcard subscriptions, retained messages, Last Will & Testament, keep-alive enforcement, and
// persistent (clean-session=0) sessions with offline message queuing. No authentication (v1 is a
// trusted-network broker).
//
// MILESTONE 1 (EMQX-parity follow-on to Phase 1): adds QoS 2 (§4.3.3), Will (§3.1.2.5/§3.14), keep-alive
// timeout (§3.1.2.10), and persistent sessions / session takeover (§3.1.2.4/§3.1.4) on top of Phase 1's
// QoS 0/1 broker. Still single-node only — everything the Phase 1 scope banner above says still holds.
//
// MILESTONE 6 (017 §4, cross-node topic routing — OPT IN, backward compatible): this file itself stays
// single-node — it still only ever writes PUBLISHes to ITS OWN sessions_/stored_sessions_. What M6 adds
// is two seams a co-located `aero::broker::BrokerCluster` (broker_cluster.hpp) wires up: `deliver_remote_publish()`
// (public — the far side of a cross-node relay hands a PUBLISH here for LOCAL match-and-deliver only) and
// `set_peer_forwarder()` (a hook `deliver_publish()` calls, AFTER local delivery, for every LOCALLY-
// originated PUBLISH). Neither is ever invoked unless a caller opts in — an unset peer_forwarder_ is a
// no-op, so every existing single-node behavior/test is byte-for-byte unchanged. The two seams together
// are also this file's loop-prevention boundary: a relay-delivered PUBLISH enters ONLY via
// deliver_remote_publish(), which never calls the forwarder — so it can never bounce back out to peers.
//
// PORTABILITY / REUSE: built on quark::pal::net + aero::pal::poll (this session's PAL work) and the
// MQTT wire codec shared with `MqttClientTransport` (mqtt_codec.hpp, 017 §9 N3) — no new socket or
// framing code, only the server-side session state machine and topic routing are new.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
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

    // M6 seam (017 §4): a hook `deliver_publish()` calls AFTER local delivery, for every LOCALLY-
    // originated PUBLISH only (never for one arriving via deliver_remote_publish() below — that is
    // exactly the loop-prevention invariant). Unset (the default) ⇒ no-op ⇒ single-node behavior is
    // unchanged. `BrokerCluster` (broker_cluster.hpp) is the intended caller: it wires this to broadcast
    // the PUBLISH to every peer node via `DistributedRouter`/`BrokerRelayActor`. Same signature as
    // `on_publish()` (topic/payload/qos) — no retain flag, mirroring `deliver_remote_publish()` below.
    void set_peer_forwarder(std::function<void(std::string_view topic, std::span<const std::byte> payload,
                                               std::uint8_t qos)> cb) {
        peer_forwarder_ = std::move(cb);
    }

    // M6 seam (017 §4): the far side of a cross-node relay. A peer node's `BrokerRelayActor` calls this
    // for a PUBLISH that arrived over the wire from ANOTHER broker instance — it runs the SAME local
    // match-and-deliver as a locally-originated PUBLISH (route_publish: every connected session with a
    // matching subscription, plus offline persistent-session queuing), and on_publish_ fires here too
    // (documented default: on_publish_ means "every PUBLISH this broker instance handles", local or
    // relayed — a flow/MES consumer downstream of on_publish() should see relayed device data exactly
    // like locally-received data). What it deliberately does NOT do: touch retained_ (a cross-node
    // PUBLISH's retain semantics stay a property of the node the publisher actually dialed — see
    // broker_cluster.hpp's banner for why), or call peer_forwarder_ (the loop-prevention boundary — a
    // relay-delivered PUBLISH is never re-broadcast onward).
    void deliver_remote_publish(std::string_view topic, std::span<const std::byte> payload,
                                std::uint8_t qos) {
        if (on_publish_) on_publish_(topic, payload, qos);
        route_publish(std::string(topic), std::vector<std::byte>(payload.begin(), payload.end()), qos);
    }

private:
    struct Subscription {
        std::string filter;
        std::uint8_t qos = 0;  // granted QoS (v1: min(requested, 1))
    };

    // A PUBLISH(QoS 2) that has been PUBREC'd but not yet PUBREL'd — 4.3.3's whole point is that the
    // message is NOT acted on (retained/routed/ingested) until PUBREL confirms, so we have to hold onto
    // it somewhere in the meantime. Session-scoped (packet ids are only unique per connection).
    struct PendingQos2 {
        std::string topic;
        std::vector<std::byte> payload;
        bool retain = false;
    };

    // One Session per accepted connection. io_mu_ serializes every write to `fd` — a PUBLISH fanned
    // out to this session from ANOTHER session's reader thread must not interleave on the wire with
    // this session's own SUBACK/PINGRESP/PUBACK writes.
    //
    // Everything below `subs`/`subs_mu` (client_id, keep_alive_s, clean_session, last_activity, the Will
    // fields, qos2_inflight) is touched ONLY by this session's own reader thread (session_loop runs one
    // thread per Session, and MQTT packets on one connection are inherently serialized) — no lock needed.
    // `kicked` is the one exception: a DIFFERENT session's thread (a session-takeover CONNECT, 3.1.4)
    // sets it, so it alone is atomic.
    struct Session {
        explicit Session(quark::pal::fd_t f) : fd(f) {}
        quark::pal::fd_t fd;
        std::mutex io_mu;
        std::mutex subs_mu;
        std::vector<Subscription> subs;

        std::string client_id;
        std::uint16_t keep_alive_s = 0;
        bool clean_session = true;
        std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
        std::atomic<bool> kicked{false};  // superseded by a newer CONNECT under the same client-id (3.1.4)

        bool has_will = false;
        std::string will_topic;
        std::vector<std::byte> will_payload;
        std::uint8_t will_qos = 0;
        bool will_retain = false;

        std::unordered_map<std::uint16_t, PendingQos2> qos2_inflight;

        bool send_packet(std::byte type_flags, const std::vector<std::byte>& body) {
            std::lock_guard<std::mutex> g(io_mu);
            return aero::transport::mqtt::write_packet(fd, type_flags, body);
        }
    };

    // A QoS ≥1 message that arrived for a client-id while it had no live connection (persistent session,
    // clean_session=0) — held so it can be redelivered the moment that client-id reconnects.
    struct QueuedMessage {
        std::string topic;
        std::vector<std::byte> payload;
        std::uint8_t qos = 0;
    };

    // A clean_session=0 client's state while it is OFFLINE: its subscription list (so it doesn't have to
    // re-SUBSCRIBE) and any QoS≥1 messages that arrived for it in the meantime. Only ever holds OFFLINE
    // clients — the moment a client-id reconnects its entry is removed and ownership moves to the live
    // Session (see handle_connect/teardown_session) — so route_publish() never has to cross-check "is
    // this client-id actually live" before queuing into it.
    struct StoredSession {
        std::vector<Subscription> subs;
        std::deque<QueuedMessage> queued;
    };

    // Cap on how many offline QoS≥1 messages a single persistent session accumulates before the oldest
    // are dropped (bounded memory for a client that never comes back) — 100 is an arbitrary but generous
    // "a device is offline for a while, not forever" allowance; revisit if a real deployment needs more.
    static constexpr std::size_t kQueuedMessageCap = 100;

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

    // 0 == no keep-alive timeout at all (3.1.1 §3.1.2.10, spec-legal). Otherwise the server "MAY"
    // disconnect after 1.5x the keep-alive interval of silence — we do, deterministically.
    [[nodiscard]] static bool keep_alive_expired(const Session& s) noexcept {
        if (s.keep_alive_s == 0) return false;
        const auto limit = std::chrono::milliseconds(static_cast<std::int64_t>(s.keep_alive_s) * 1500);
        return std::chrono::steady_clock::now() - s.last_activity > limit;
    }

    void session_loop(const std::shared_ptr<Session>& s) {
        bool clean_disconnect = false;  // true only for an explicit DISCONNECT (0xE0) — gates the Will
        while (running_.load(std::memory_order_acquire)) {
            // Checked every iteration (not just on a read timeout): a session-takeover CONNECT (3.1.4)
            // must end this session promptly even if it's mid-read of something else.
            if (s->kicked.load(std::memory_order_acquire)) break;

            // Explicit poll-then-read (rather than letting read_packet's own internal 200ms polling loop
            // block until a full packet arrives) so a silent-but-still-open connection gets its keep-alive
            // checked every ~200ms too, not just when a packet actually shows up (mirrors
            // tcp_transport.hpp's accept_loop poll-timeout-as-heartbeat idiom).
            const auto ready = aero::pal::wait_readable(s->fd, 200);
            if (!ready) break;  // poll itself failed
            if (!*ready) {
                if (keep_alive_expired(*s)) break;  // 3.1.1 §3.1.2.10 — silent longer than 1.5x keep-alive
                continue;
            }

            auto pkt = aero::transport::mqtt::read_packet(s->fd, running_);
            if (!pkt) break;  // peer closed / error / running flipped false
            s->last_activity = std::chrono::steady_clock::now();  // ANY inbound packet refreshes it
            const std::uint8_t type = pkt->type_flags & 0xF0;
            if (type == 0x10) {          // CONNECT
                if (!handle_connect(s, *pkt)) break;
            } else if (type == 0x80) {   // SUBSCRIBE
                if (!handle_subscribe(*s, *pkt)) break;
            } else if (type == 0x30) {   // PUBLISH
                if (!handle_publish(*s, *pkt)) break;
            } else if (type == 0x60) {   // PUBREL (4.3.3 step 3; low nibble MUST be 0x2, not checked —
                                          // mirrors this file's existing tolerance of e.g. SUBSCRIBE's flags)
                if (!handle_pubrel(*s, *pkt)) break;
            } else if (type == 0xC0) {   // PINGREQ
                if (!s->send_packet(std::byte{0xD0}, {})) break;  // PINGRESP, no body
            } else if (type == 0xE0) {   // DISCONNECT (3.1.1 §3.14): graceful — no Will on a clean
                                          // disconnect, so discard it BEFORE teardown runs.
                s->has_will = false;
                clean_disconnect = true;
                break;
            }
            // 0x40 PUBACK from the peer (ack of a QoS-1 delivery we sent): no retry state kept in v1
            // (mirrors MqttClientTransport's own honest "no QoS-1 sender-side retransmit" scope) — read
            // and discard, already consumed by read_packet above.
        }
        teardown_session(s, clean_disconnect);
    }

    // Common end-of-life path for every way a session stops (clean DISCONNECT, socket error/reset,
    // keep-alive timeout, or losing a session-takeover race) — Will delivery and persistent-session
    // handoff both key off exactly this one place so neither can be bypassed by a code path forgetting to
    // call them.
    void teardown_session(const std::shared_ptr<Session>& s, bool clean_disconnect) {
        // Will (3.1.2.5 / 3.14): fires on every ungraceful end (socket error, peer reset, keep-alive
        // expiry, session takeover) — `clean_disconnect` is the ONLY case that already cleared has_will.
        if (!clean_disconnect && s->has_will)
            deliver_publish(s->will_topic, s->will_payload, s->will_qos, s->will_retain);

        // Persistent-session handoff (3.1.1 §3.1.2.4): only persist if this session is STILL the
        // client-id's current owner in client_sessions_ — a session that just lost a takeover race must
        // not resurrect stale state over the new connection that already replaced it there.
        if (!s->client_id.empty()) {
            std::lock_guard<std::mutex> g(client_sessions_mu_);
            const auto it = client_sessions_.find(s->client_id);
            const bool still_owner = it != client_sessions_.end() && it->second == s;
            if (still_owner) {
                client_sessions_.erase(it);
                if (!s->clean_session) {
                    StoredSession stored;
                    {
                        std::lock_guard<std::mutex> subg(s->subs_mu);
                        stored.subs = s->subs;
                    }
                    std::lock_guard<std::mutex> sg(stored_sessions_mu_);
                    stored_sessions_[s->client_id] = std::move(stored);
                }
            }
        }

        remove_session(s);
        quark::pal::close_fd(s->fd);
    }

    bool handle_connect(const std::shared_ptr<Session>& s, const aero::transport::mqtt::Packet& pkt) {
        const std::vector<std::byte>& b = pkt.body;
        auto read_u16 = [&](std::size_t p) -> std::uint16_t {
            return static_cast<std::uint16_t>((std::to_integer<std::uint8_t>(b[p]) << 8) |
                                               std::to_integer<std::uint8_t>(b[p + 1]));
        };
        std::size_t pos = 0;
        if (pos + 2 > b.size()) return false;
        const std::uint16_t proto_len = read_u16(pos);
        pos += 2;
        if (pos + proto_len > b.size()) return false;
        pos += proto_len;  // protocol name ("MQTT"/"MQIsdp") — not validated, matches Phase 1's posture
        if (pos + 1 > b.size()) return false;
        pos += 1;  // protocol level — not validated

        if (pos + 1 > b.size()) return false;
        const std::uint8_t connect_flags = std::to_integer<std::uint8_t>(b[pos]);
        pos += 1;
        if (pos + 2 > b.size()) return false;
        const std::uint16_t keep_alive = read_u16(pos);
        pos += 2;

        if (pos + 2 > b.size()) return false;
        const std::uint16_t client_id_len = read_u16(pos);
        pos += 2;
        if (pos + client_id_len > b.size()) return false;
        std::string client_id(reinterpret_cast<const char*>(b.data() + pos), client_id_len);
        pos += client_id_len;

        // Connect-flags bit layout (3.1.1 §3.1.2.3): bit2=Will Flag, bits4-3=Will QoS, bit5=Will Retain,
        // bit1=Clean Session.
        const bool will_flag = (connect_flags & 0x04) != 0;
        const std::uint8_t will_qos = (connect_flags >> 3) & 0x03;
        const bool will_retain = (connect_flags & 0x20) != 0;
        const bool clean_session = (connect_flags & 0x02) != 0;

        std::string will_topic;
        std::vector<std::byte> will_payload;
        if (will_flag) {
            if (pos + 2 > b.size()) return false;
            const std::uint16_t wt_len = read_u16(pos);
            pos += 2;
            if (pos + wt_len > b.size()) return false;
            will_topic.assign(reinterpret_cast<const char*>(b.data() + pos), wt_len);
            pos += wt_len;
            if (pos + 2 > b.size()) return false;
            const std::uint16_t wm_len = read_u16(pos);
            pos += 2;
            if (pos + wm_len > b.size()) return false;
            will_payload.assign(b.begin() + static_cast<std::ptrdiff_t>(pos),
                                b.begin() + static_cast<std::ptrdiff_t>(pos + wm_len));
            pos += wm_len;
        }
        // Username/password (if present) are only skipped over, never consulted — v1 has no
        // authentication (Phase 1 scope banner) — but they still have to be parsed past so `pos` would be
        // correctly aligned for anything that follows (defensive; nothing currently does).
        if (connect_flags & 0x80) {  // username flag
            if (pos + 2 > b.size()) return false;
            const std::uint16_t ulen = read_u16(pos);
            pos += 2;
            if (pos + ulen > b.size()) return false;
            pos += ulen;
        }
        if (connect_flags & 0x40) {  // password flag
            if (pos + 2 > b.size()) return false;
            const std::uint16_t plen = read_u16(pos);
            pos += 2;
            if (pos + plen > b.size()) return false;
            pos += plen;
        }

        s->client_id = client_id;
        s->keep_alive_s = keep_alive;
        s->clean_session = clean_session;
        s->last_activity = std::chrono::steady_clock::now();
        if (will_flag) {
            s->has_will = true;
            s->will_topic = std::move(will_topic);
            s->will_payload = std::move(will_payload);
            s->will_qos = will_qos;
            s->will_retain = will_retain;
        }

        bool session_present = false;
        std::vector<Subscription> restored_subs;
        std::vector<QueuedMessage> to_flush;
        if (!client_id.empty()) {
            // Session takeover (3.1.4): a second CONNECT under a client-id already live closes the
            // first. "Closes" here means flip the OLD session's `kicked` flag, which IT notices in its
            // own poll loop and tears down on ITS OWN thread — this file never closes a socket from a
            // thread that doesn't own it (matches stop()'s and session_loop's existing discipline).
            {
                std::lock_guard<std::mutex> g(client_sessions_mu_);
                const auto it = client_sessions_.find(client_id);
                if (it != client_sessions_.end() && it->second != s)
                    it->second->kicked.store(true, std::memory_order_release);
                client_sessions_[client_id] = s;
            }

            std::lock_guard<std::mutex> g(stored_sessions_mu_);
            if (clean_session) {
                stored_sessions_.erase(client_id);  // 3.1.2.4: clean=1 discards any prior session state
            } else {
                const auto it = stored_sessions_.find(client_id);
                if (it != stored_sessions_.end()) {
                    restored_subs = it->second.subs;
                    to_flush.assign(it->second.queued.begin(), it->second.queued.end());
                    stored_sessions_.erase(it);  // ownership moves to this now-live session
                    session_present = true;
                }
            }
        }
        if (!restored_subs.empty()) {
            std::lock_guard<std::mutex> g(s->subs_mu);
            s->subs = std::move(restored_subs);
        }

        std::vector<std::byte> body{static_cast<std::byte>(session_present ? 0x01 : 0x00), std::byte{0x00}};  // rc=0
        if (!s->send_packet(std::byte{0x20}, body)) return false;  // CONNACK

        // Flush anything that arrived while this persistent session was offline — AFTER CONNACK, per
        // 3.1.1 (the client only knows to expect them once it's seen session-present=1).
        for (const QueuedMessage& m : to_flush)
            if (!publish_to(*s, m.topic, m.payload, m.qos, /*retain=*/false)) return false;
        return true;
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

        if (qos == 2) {
            // Exactly-once (4.3.3): stash until PUBREL confirms receipt, then — and ONLY then — retain/
            // ingest/route it. Re-inserting under the same packet_id on a retransmitted PUBLISH (the
            // sender's PUBREC was lost) is a harmless overwrite with identical content, not a double
            // routing — the actual routing only ever happens once, from handle_pubrel.
            s.qos2_inflight[packet_id] = PendingQos2{topic, payload, retain};
            std::vector<std::byte> ack;
            aero::transport::mqtt::put_u16_be(ack, packet_id);
            return s.send_packet(std::byte{0x50}, ack);  // PUBREC
        }

        // QoS 0/1: act BEFORE acking — a publisher that has received its PUBACK must be able to assume a
        // subsequent SUBSCRIBE/retained-replay from anyone will already see this value/delivery (no
        // ack-before-visible race).
        deliver_publish(topic, payload, qos, retain);
        if (qos == 1) {
            std::vector<std::byte> ack;
            aero::transport::mqtt::put_u16_be(ack, packet_id);
            if (!s.send_packet(std::byte{0x40}, ack)) return false;  // PUBACK
        }
        return true;
    }

    // PUBREL (4.3.3 step 3): the sender has now durably committed to this packet_id, so this is where a
    // QoS-2 PUBLISH FINALLY gets acted on — the one thing that distinguishes QoS 2 from QoS 1 (which acts
    // immediately). PUBCOMP is sent unconditionally, even for an unknown packet_id (a retried PUBREL after
    // we already completed it and forgot it) — the sender's handshake must terminate either way.
    bool handle_pubrel(Session& s, const aero::transport::mqtt::Packet& pkt) {
        const std::vector<std::byte>& b = pkt.body;
        if (b.size() < 2) return false;
        const std::uint16_t packet_id =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        const auto it = s.qos2_inflight.find(packet_id);
        if (it != s.qos2_inflight.end()) {
            deliver_publish(it->second.topic, it->second.payload, /*qos=*/2, it->second.retain);
            s.qos2_inflight.erase(it);
        }
        std::vector<std::byte> ack;
        aero::transport::mqtt::put_u16_be(ack, packet_id);
        return s.send_packet(std::byte{0x70}, ack);  // PUBCOMP
    }

    // The action a PUBLISH ultimately performs once its QoS contract permits it: QoS 0/1 call this
    // immediately (handle_publish); QoS 2 defers it until PUBREL (handle_pubrel); a Will "publishes" this
    // directly with no wire packet or ack at all (teardown_session) — one shared path so retention,
    // ingestion, and routing can never drift between the three. Every caller of THIS function is, by
    // construction, a LOCALLY-originated PUBLISH (a directly-connected session's own PUBLISH/PUBREL, or
    // its Will) — never a relay-delivered one (that path is deliver_remote_publish(), above, which is
    // deliberately NOT routed through here — seeing peer_forwarder_ fire is exactly the loop-prevention
    // invariant 017 M6 requires).
    void deliver_publish(const std::string& topic, const std::vector<std::byte>& payload, std::uint8_t qos,
                         bool retain) {
        if (retain) {
            std::lock_guard<std::mutex> g(retained_mu_);
            if (payload.empty())
                retained_.erase(topic);  // zero-length retained PUBLISH clears retention (3.1.1 §3.3.1.3)
            else
                retained_[topic] = payload;
        }
        if (on_publish_) on_publish_(topic, payload, qos);
        route_publish(topic, payload, qos);
        // M6 (017 §4): AFTER local delivery, so a peer's relayed copy can never arrive before this node's
        // own subscribers see it. No-op single-node default (peer_forwarder_ unset) — see set_peer_forwarder().
        if (peer_forwarder_) peer_forwarder_(topic, payload, qos);
    }

    // Fan out to every connected session with a matching subscription, at min(publish qos, granted
    // qos) — a snapshot copy of sessions_ is taken under the lock so the actual socket writes (which
    // may block briefly on backpressure) never happen while holding sessions_mu_. Also queues into any
    // OFFLINE persistent (clean_session=0) session whose stored subscriptions match — QoS ≥1 only, since
    // there is nothing to durably queue a QoS-0 "at most once" message as.
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

        if (qos == 0) return;
        std::lock_guard<std::mutex> g(stored_sessions_mu_);
        for (auto& [client_id, stored] : stored_sessions_) {
            for (const Subscription& sub : stored.subs) {
                if (!topic_matches(sub.filter, topic)) continue;
                const std::uint8_t deliver_qos = qos < sub.qos ? qos : sub.qos;
                if (stored.queued.size() >= kQueuedMessageCap) stored.queued.pop_front();  // drop-oldest
                stored.queued.push_back(QueuedMessage{topic, payload, deliver_qos});
                break;  // one queued copy per client-id even if multiple subs match the same topic
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

    // client-id → the Session currently claiming it (session takeover, 3.1.4). Only ever holds LIVE
    // sessions; an entry is removed the moment its Session tears down (teardown_session).
    std::mutex client_sessions_mu_;
    std::unordered_map<std::string, std::shared_ptr<Session>> client_sessions_;

    // client-id → offline persistent-session state (3.1.2.4). Only ever holds OFFLINE clients; see
    // StoredSession's comment for the invariant this relies on.
    std::mutex stored_sessions_mu_;
    std::unordered_map<std::string, StoredSession> stored_sessions_;

    std::atomic<std::uint16_t> packet_id_{1};
    std::function<void(std::string_view, std::span<const std::byte>, std::uint8_t)> on_publish_;

    // M6 (017 §4): unset by default (single-node — 100% of Phase 1 through M3 behavior unchanged). Set by
    // BrokerCluster via set_peer_forwarder() to broadcast every locally-originated PUBLISH to peer nodes.
    std::function<void(std::string_view, std::span<const std::byte>, std::uint8_t)> peer_forwarder_;
};

}  // namespace aero::broker
