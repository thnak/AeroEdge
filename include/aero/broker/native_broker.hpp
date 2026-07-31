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
//
// M5 (TLS+ACL milestone): adds an optional TLS listener (aero/pal/tls.hpp, mbedTLS-backed) alongside the
// plaintext one, and an optional CONNECT-time Authenticator + per-topic Authorizer (aero/broker/acl.hpp).
// Both are OFF by default (Config::tls unset, Config::authenticate/authorizer unset) — a Config that sets
// neither behaves EXACTLY as Phase 1/Milestone 1 did, no exceptions. See Config's own field comments.
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "pal/net.hpp"  // quark::pal::* — fd_t, tcp_listen/accept_one/recv_some/send_some/...

#if !defined(_WIN32)
#include <arpa/inet.h>  // inet_pton (Windows: transitively via pal/net.hpp's ws2tcpip.h)
#endif

#include "aero/broker/acl.hpp"          // Authorizer/Authenticator/AclAction/TopicAclAuthorizer (M5)
#include "aero/broker/topic_match.hpp"  // topic_matches() — shared with acl.hpp (017 N3 precedent)
#include "aero/pal/poll.hpp"
#include "aero/pal/tls.hpp"              // TlsServerContext/TlsSession (M5)
#include "aero/transport/io_channel.hpp"  // PlainChannel/TlsChannel — Session's channel seam (M5)
#include "aero/transport/mqtt_codec.hpp"

namespace aero::broker {

struct Config {
    std::string bind_host = "0.0.0.0";  // interface to listen on
    std::uint16_t listen_port = 1883;   // MQTT's conventional plaintext port; 0 => ephemeral
    int backlog = 64;

    // M5: unset (default) => no TLS listener at all — start()/stop() behave exactly as before this
    // milestone. Set it to bind a SECOND listener on `tls_port` that speaks TLS (mbedTLS handshake via
    // aero::pal::tls::TlsServerContext) before MQTT framing begins; `ca_file` inside ServerConfig, if
    // non-empty, additionally requires + verifies a client certificate (mTLS). The plaintext listener on
    // `listen_port` keeps running unchanged alongside it — this is additive, not a replacement.
    std::optional<aero::pal::tls::ServerConfig> tls;
    std::uint16_t tls_port = 8883;  // MQTT's conventional TLS port; only relevant when `tls` is set

    // M5: unset (default) => every CONNECT is accepted with no credential check, exactly as before this
    // milestone (Session::principal stays "anonymous"). Set it to gate CONNECT on username/password —
    // nullopt from the callback rejects the CONNECT (CONNACK rc=0x04) BEFORE any session state is
    // touched; a returned string becomes the session's `principal`, consulted by `authorizer` below.
    Authenticator authenticate;

    // M5: unset (default) => every PUBLISH/SUBSCRIBE is allowed, exactly as before this milestone. Set it
    // (e.g. a TopicAclAuthorizer) to gate PUBLISH/SUBSCRIBE per (principal, topic, action) — see
    // handle_subscribe/handle_publish/handle_pubrel below for exactly where/how it's consulted.
    std::shared_ptr<Authorizer> authorizer;
};

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

        // M5: a second, TLS-speaking listener — additive to the plaintext one above, never a replacement.
        // Fail-closed to match the plaintext listener's own posture: if cfg_.tls is set but the TLS
        // context or the second listener can't be stood up, start() fails outright (no half-started
        // broker with a silently-missing TLS port).
        if (cfg_.tls) {
            auto ctx = aero::pal::tls::TlsServerContext::create(*cfg_.tls);
            if (!ctx) {
                quark::pal::close_fd(listen_fd_);
                listen_fd_ = quark::pal::invalid_fd;
                return std::unexpected("native broker: tls context create failed: " + ctx.error());
            }
            tls_ctx_ = std::make_unique<aero::pal::tls::TlsServerContext>(std::move(*ctx));

            auto lfd_tls = quark::pal::tcp_listen(addr_u64, cfg_.tls_port, cfg_.backlog);
            if (!lfd_tls) {
                quark::pal::close_fd(listen_fd_);
                listen_fd_ = quark::pal::invalid_fd;
                tls_ctx_.reset();
                return std::unexpected("native broker: tls tcp_listen failed: " + lfd_tls.error().message());
            }
            listen_fd_tls_ = *lfd_tls;
            if (auto p = quark::pal::local_port(listen_fd_tls_)) resolved_port_tls_ = *p;
        }

        running_.store(true, std::memory_order_release);
        accept_thread_ = std::thread([this] { accept_loop(); });
        if (cfg_.tls) accept_thread_tls_ = std::thread([this] { accept_loop_tls(); });
        return {};
    }

    // Stop the accept loop(s), close every session + the listener(s), join every thread. Idempotent.
    // ORDER MATTERS (TSan-clean, mirrors tcp_transport.hpp): flip running_ false, join the accept
    // thread(s) FIRST (each exits within one poll timeout and stops spawning new sessions), then close
    // the listener(s), then join every session thread (each notices running_ within its own poll
    // timeout). The TLS accept thread/listener follow the exact same ordering as the plaintext ones —
    // when cfg_.tls was never set, accept_thread_tls_ was never started (not joinable) and
    // listen_fd_tls_ stays invalid_fd, so every TLS-specific step below is a harmless no-op.
    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (accept_thread_.joinable()) accept_thread_.join();
        if (accept_thread_tls_.joinable()) accept_thread_tls_.join();
        if (listen_fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(listen_fd_);
            listen_fd_ = quark::pal::invalid_fd;
        }
        if (listen_fd_tls_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(listen_fd_tls_);
            listen_fd_tls_ = quark::pal::invalid_fd;
        }
        {
            std::lock_guard<std::mutex> g(sessions_mu_);
            for (std::thread& t : session_threads_)
                if (t.joinable()) t.join();
            session_threads_.clear();
            sessions_.clear();
        }
        tls_ctx_.reset();
    }

    [[nodiscard]] std::uint16_t listen_port() const noexcept { return resolved_port_; }

    // M5: the TLS listener's resolved port (0 until start() with cfg_.tls set has run; meaningful when
    // cfg_.tls_port == 0, i.e. "pick an ephemeral port", the way listen_port() already works for the
    // plaintext listener).
    [[nodiscard]] std::uint16_t listen_port_tls() const noexcept { return resolved_port_tls_; }

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

    // 017 M7.2 PR A: carries the subset of MQTT 5 PUBLISH properties this broker acts on end-to-end, from
    // ingestion (handle_publish/Will-parse) through to the outbound wire (publish_to). This PR populates
    // only expiry_deadline; response_topic/correlation_data/user_properties are always empty until a
    // future PR B — shaped now so that PR doesn't need a second pass touching every call site below.
    struct PublishExtras {
        std::optional<std::chrono::steady_clock::time_point> expiry_deadline;  // absolute deadline,
            // computed ONCE at ingestion time — never re-derived from a re-sent "original interval".
        std::optional<std::string> response_topic;                // future PR B
        std::optional<std::vector<std::byte>> correlation_data;    // future PR B
        std::vector<std::pair<std::string, std::string>> user_properties;  // future PR B
    };

    // A PUBLISH(QoS 2) that has been PUBREC'd but not yet PUBREL'd — 4.3.3's whole point is that the
    // message is NOT acted on (retained/routed/ingested) until PUBREL confirms, so we have to hold onto
    // it somewhere in the meantime. Session-scoped (packet ids are only unique per connection).
    struct PendingQos2 {
        std::string topic;
        std::vector<std::byte> payload;
        bool retain = false;
        PublishExtras extras;  // 017 M7.2 PR A: stashed at PUBLISH time, threaded through unchanged to
                                // handle_pubrel's deliver_publish() — a QoS2 message's Message Expiry must
                                // not silently reset to "never expires" between PUBLISH and PUBREL.
    };

    // One Session per accepted connection. io_mu_ serializes every write to `channel` — a PUBLISH fanned
    // out to this session from ANOTHER session's reader thread must not interleave on the wire with
    // this session's own SUBACK/PINGRESP/PUBACK writes.
    //
    // Everything below `subs`/`subs_mu` (client_id, keep_alive_s, clean_session, last_activity, the Will
    // fields, qos2_inflight, principal) is touched ONLY by this session's own reader thread (session_loop
    // runs one thread per Session, and MQTT packets on one connection are inherently serialized) — no
    // lock needed. `kicked` is the one exception: a DIFFERENT session's thread (a session-takeover
    // CONNECT, 3.1.4) sets it, so it alone is atomic.
    //
    // M5: `channel` is EITHER a PlainChannel OR a TlsChannel, decided once at accept time (accept_loop()
    // vs accept_loop_tls()) and never changed after — a std::variant is the idiomatic shape for "exactly
    // one of these, chosen once" (io_channel.hpp's own banner anticipated this exact use). `tls_owner`
    // heap-owns the TlsSession when this is a TLS session (null for plaintext) so its address stays
    // stable across any Session moves — TlsChannel only ever holds a non-owning observer pointer into it
    // (mirrors tls.hpp's own heap-ownership-for-address-stability reasoning, see that file's banner).
    // `fd` is kept alongside the channel (not folded away) because session_loop's keep-alive poll
    // (aero::pal::wait_readable(s->fd, 200)) and teardown's close_fd() both need the raw OS fd regardless
    // of channel kind — TLS still rides a real socket underneath.
    struct Session {
        explicit Session(quark::pal::fd_t f) : fd(f), channel(aero::transport::PlainChannel{f}) {}
        Session(quark::pal::fd_t f, aero::pal::tls::TlsSession tls_session)
            : fd(f),
              tls_owner(std::make_unique<aero::pal::tls::TlsSession>(std::move(tls_session))),
              channel(aero::transport::TlsChannel{tls_owner.get()}) {}

        quark::pal::fd_t fd;
        std::unique_ptr<aero::pal::tls::TlsSession> tls_owner;  // non-null only for a TLS session
        std::variant<aero::transport::PlainChannel, aero::transport::TlsChannel> channel;

        std::mutex io_mu;
        std::mutex subs_mu;
        std::vector<Subscription> subs;

        std::string client_id;
        std::uint16_t keep_alive_s = 0;
        bool clean_session = true;
        // M7: negotiated in handle_connect, set ONCE (never mutated again) and read afterward only by
        // this SAME session's own reader thread (matches this struct's existing single-thread-owns-
        // Session-state discipline, see this struct's banner) — 4 (MQTT 3.1.1) is the default so every
        // function that doesn't branch on it yet keeps behaving exactly as before this milestone.
        std::uint8_t protocol_version = 4;
        // M7.1: negotiated (v5 only) in handle_connect, set ONCE (never mutated again) — same discipline
        // as protocol_version above. nullopt means "no Session Expiry Interval property on CONNECT" (v4
        // sessions never populate this, and neither does a v5 session that omitted the property) — treated
        // identically to "never expires" by teardown_session, so the pre-M7.1 default behavior is unchanged.
        std::optional<std::uint32_t> session_expiry_interval;
        // M7.2 PR A: negotiated (v5 only) in handle_connect from CONNECT's Maximum Packet Size property
        // (0x27), set ONCE (never mutated again) — same single-thread-owned/set-once discipline as
        // session_expiry_interval above. nullopt means "no cap advertised" (v4 sessions, and v5 sessions
        // that omitted the property) — publish_to()'s size choke point is a no-op in that case, so pre-
        // M7.2 behavior (no outbound size cap at all) is unchanged.
        std::optional<std::uint32_t> max_packet_size;
        std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
        std::atomic<bool> kicked{false};  // superseded by a newer CONNECT under the same client-id (3.1.4)

        bool has_will = false;
        std::string will_topic;
        std::vector<std::byte> will_payload;
        std::uint8_t will_qos = 0;
        bool will_retain = false;
        // M7.2 PR A: the Will's own PublishExtras (Message Expiry, if the Will Properties block carried
        // one) — populated in handle_connect alongside will_topic/will_payload/etc., consulted in
        // teardown_session's Will-delivery call. See deliver_publish's banner: Will/regular PUBLISH share
        // one delivery path so retention/ingestion/routing can never drift between them; leaving this
        // always-empty would quietly violate that for Message Expiry specifically.
        PublishExtras will_extras;

        std::string principal = "anonymous";  // M5: set from Config::authenticate's return, else default

        std::unordered_map<std::uint16_t, PendingQos2> qos2_inflight;
        // M7.1: inbound Topic Alias table (MQTT 5 §3.3.2.3.4) — client-established alias -> topic string,
        // this session's own. Same single-thread-owned, no-lock discipline as qos2_inflight above (only
        // this session's own reader thread ever touches it, via handle_publish).
        std::unordered_map<std::uint16_t, std::string> topic_aliases;

        bool send_packet(std::byte type_flags, const std::vector<std::byte>& body) {
            std::lock_guard<std::mutex> g(io_mu);
            return std::visit(
                [&](auto& ch) { return aero::transport::mqtt::write_packet(ch, type_flags, body); }, channel);
        }
    };

    // A QoS ≥1 message that arrived for a client-id while it had no live connection (persistent session,
    // clean_session=0) — held so it can be redelivered the moment that client-id reconnects.
    struct QueuedMessage {
        std::string topic;
        std::vector<std::byte> payload;
        std::uint8_t qos = 0;
        PublishExtras extras;  // 017 M7.2 PR A: carried through so an offline-queued message's Message
                                // Expiry is still honored (checked in publish_to()) when it's finally
                                // flushed to a reconnecting client.
    };

    // 017 M7.2 PR A: the retained-message table's value type — was a bare `std::vector<std::byte>`
    // payload; now carries that PUBLISH's extras too (Message Expiry, specifically) so a retained message
    // replayed to a future SUBSCRIBE still honors its original TTL instead of being treated as eternal.
    struct RetainedMessage {
        std::vector<std::byte> payload;
        PublishExtras extras;
    };

    // A clean_session=0 client's state while it is OFFLINE: its subscription list (so it doesn't have to
    // re-SUBSCRIBE) and any QoS≥1 messages that arrived for it in the meantime. Only ever holds OFFLINE
    // clients — the moment a client-id reconnects its entry is removed and ownership moves to the live
    // Session (see handle_connect/teardown_session) — so route_publish() never has to cross-check "is
    // this client-id actually live" before queuing into it.
    struct StoredSession {
        std::vector<Subscription> subs;
        std::deque<QueuedMessage> queued;
        // M7.1: Session Expiry Interval TTL (MQTT 5 §3.1.2.11.2) — nullopt means "never expires" (v4
        // sessions, and v5 sessions that didn't send the property), identical to pre-M7.1 behavior.
        // Set in teardown_session when the owning Session had a session_expiry_interval; consulted in
        // handle_connect's session-restore logic to discard a stale entry instead of restoring it.
        std::optional<std::chrono::steady_clock::time_point> expires_at;
    };

    // Cap on how many offline QoS≥1 messages a single persistent session accumulates before the oldest
    // are dropped (bounded memory for a client that never comes back) — 100 is an arbitrary but generous
    // "a device is offline for a while, not forever" allowance; revisit if a real deployment needs more.
    static constexpr std::size_t kQueuedMessageCap = 100;

    // M7.1: the highest inbound Topic Alias value this broker accepts (advertised to v5 clients via
    // CONNACK's Topic Alias Maximum property, MQTT 5 §3.1.2.11.2) — 16 is an arbitrary but generous
    // "a device's PUBLISH topic set is small" allowance; revisit if a real deployment needs more.
    static constexpr std::uint16_t kTopicAliasMax = 16;

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

    // M5: mirrors accept_loop() exactly except for the one real difference — a TLS handshake
    // (tls_ctx_->accept()) must complete BEFORE the Session is constructed/registered and session_loop
    // spawned, since session_loop assumes MQTT framing can start immediately. A handshake failure/timeout
    // just closes the fd and keeps accepting (matches accept_loop()'s own tolerant "not this one, try the
    // next" posture on a failed accept_one) — it must never crash or wedge this loop.
    void accept_loop_tls() {
        while (running_.load(std::memory_order_acquire)) {
            const auto ready = aero::pal::wait_readable(listen_fd_tls_, 200);
            if (!ready || !*ready) continue;  // timeout/err → re-check running_
            auto cfd = quark::pal::accept_one(listen_fd_tls_);
            if (!cfd) continue;
            auto handshake = tls_ctx_->accept(*cfd);
            if (!handshake) {
                quark::pal::close_fd(*cfd);
                continue;  // handshake failed/timed out — not this connection's fault to wedge the loop
            }
            auto session = std::make_shared<Session>(*cfd, std::move(*handshake));
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
            if (s->kicked.load(std::memory_order_acquire)) {
                send_disconnect(*s, 0x8E);  // Session taken over (M7.1)
                break;
            }

            // Explicit poll-then-read (rather than letting read_packet's own internal 200ms polling loop
            // block until a full packet arrives) so a silent-but-still-open connection gets its keep-alive
            // checked every ~200ms too, not just when a packet actually shows up (mirrors
            // tcp_transport.hpp's accept_loop poll-timeout-as-heartbeat idiom).
            const auto ready = aero::pal::wait_readable(s->fd, 200);
            if (!ready) break;  // poll itself failed
            if (!*ready) {
                if (keep_alive_expired(*s)) {  // 3.1.1 §3.1.2.10 — silent longer than 1.5x keep-alive
                    send_disconnect(*s, 0x8D);  // Keep Alive timeout (M7.1)
                    break;
                }
                continue;
            }

            auto pkt = std::visit(
                [&](auto& ch) { return aero::transport::mqtt::read_packet(ch, running_); }, s->channel);
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
            deliver_publish(s->will_topic, s->will_payload, s->will_qos, s->will_retain, s->will_extras);

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
                    // M7.1: only set when the CONNECT actually carried Session Expiry Interval — nullopt
                    // (v4, or v5 without the property) stays nullopt on `stored`, i.e. "never expires",
                    // exactly matching pre-M7.1 behavior. An explicit 0 is handled correctly here too (no
                    // special case needed): `now() + 0s` == `now()`, so handle_connect's `now() >=
                    // expires_at` check treats it as already expired the moment it's looked up.
                    if (s->session_expiry_interval)
                        stored.expires_at = std::chrono::steady_clock::now() +
                                            std::chrono::seconds(*s->session_expiry_interval);
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
        const std::uint8_t protocol_level = std::to_integer<std::uint8_t>(b[pos]);
        pos += 1;

        // M7: protocol version negotiation — the FIRST thing this function does that can reject the
        // CONNECT, deliberately earlier than the M5 auth-gate below (nothing that mutates session state
        // has been parsed yet at this point, so there is nothing to be careful about undoing). 0x04 =
        // MQTT 3.1.1 (existing path, byte-for-byte unchanged below); 0x05 = MQTT 5 (new path, gated by
        // `is_v5`/`s->protocol_version` throughout this file). Anything else: reject with the 3.1.1-shaped
        // 2-byte CONNACK body (rc=0x01, "unacceptable protocol version", 3.1.1 §3.2.2.3) regardless of
        // what the client might have claimed — we can't trust a v5-shaped ack for a version we don't
        // recognize, and this 2-byte shape is legal for a rejecting server to send either way.
        if (protocol_level != 0x04 && protocol_level != 0x05) {
            (void)s->send_packet(std::byte{0x20}, {std::byte{0x00}, std::byte{0x01}});
            return false;
        }
        const bool is_v5 = protocol_level == 0x05;
        s->protocol_version = is_v5 ? 5 : 4;  // set once here, read-only from here on (see Session's field
                                               // comment) — a rejected CONNECT still gets to report the
                                               // wire-level version it negotiated in its reject CONNACK's
                                               // shape below, which is not part of the "zero trace" the M5
                                               // auth gate protects (client_id/has_will/session tables).

        if (pos + 1 > b.size()) return false;
        const std::uint8_t connect_flags = std::to_integer<std::uint8_t>(b[pos]);
        pos += 1;
        if (pos + 2 > b.size()) return false;
        const std::uint16_t keep_alive = read_u16(pos);
        pos += 2;

        // M7 v5: CONNECT Properties sit here in the wire order (Protocol Name -> Level -> Flags ->
        // Keep Alive -> Properties -> Client Identifier, MQTT 5 §3.1.2) — a new insertion point relative
        // to 3.1.1's Keep-Alive-straight-to-Client-ID order. M7.1: Session Expiry Interval is now stored
        // onto s->session_expiry_interval below and its TTL enforced (see StoredSession::expires_at,
        // teardown_session, and the session-restore logic further down). M7.2 PR A: Maximum Packet Size is
        // now stored onto s->max_packet_size below and enforced in publish_to()'s choke point 2; every
        // other property in the table is recognized only so read_properties() can correctly skip past it.
        std::optional<std::uint32_t> connect_session_expiry;
        std::optional<std::uint32_t> connect_max_packet_size;
        if (is_v5) {
            auto props = aero::transport::mqtt::read_properties(b, pos);
            if (!props) return false;  // malformed CONNECT properties
            connect_session_expiry = props->session_expiry_interval;
            connect_max_packet_size = props->maximum_packet_size;
        }

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
        PublishExtras will_extras;
        if (will_flag) {
            // M7 v5: a SEPARATE Will Properties block, positioned immediately before Will Topic (MQTT 5
            // §3.1.3.2) — distinct from the CONNECT-level Properties block already consumed above. Most
            // properties in it (Will Delay Interval/Payload Format/etc.) still aren't acted on in v1/M7.2
            // PR A; malformed -> treated exactly like any other malformed CONNECT field. M7.2 PR A: Message
            // Expiry Interval (0x02) IS now captured into will_extras so a Will PUBLISH's TTL isn't
            // silently dropped while regular PUBLISHes honor it (see Session::will_extras's own comment —
            // this file documents Will/regular PUBLISH as one shared delivery path so they can't drift).
            if (is_v5) {
                auto will_props = aero::transport::mqtt::read_properties(b, pos);
                if (!will_props) return false;
                if (will_props->message_expiry_interval)
                    will_extras.expiry_deadline = std::chrono::steady_clock::now() +
                                                  std::chrono::seconds(*will_props->message_expiry_interval);
            }
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
        // Username/password (if present): M5 materializes them (previously only skipped over — Phase 1
        // had no authentication at all) so Config::authenticate below can consult them. Still parsed past
        // unconditionally either way so `pos` stays correctly aligned for anything that follows.
        std::string username, password;
        if (connect_flags & 0x80) {  // username flag
            if (pos + 2 > b.size()) return false;
            const std::uint16_t ulen = read_u16(pos);
            pos += 2;
            if (pos + ulen > b.size()) return false;
            username.assign(reinterpret_cast<const char*>(b.data() + pos), ulen);
            pos += ulen;
        }
        if (connect_flags & 0x40) {  // password flag
            if (pos + 2 > b.size()) return false;
            const std::uint16_t plen = read_u16(pos);
            pos += 2;
            if (pos + plen > b.size()) return false;
            password.assign(reinterpret_cast<const char*>(b.data() + pos), plen);
            pos += plen;
        }

        // M5 auth gate: this MUST be the first thing after parsing that can cause an early return — a
        // rejected CONNECT must leave ZERO trace in broker state (no client_id, no has_will, no
        // client_sessions_/stored_sessions_ entry). Everything below this point mutates one of those, so
        // nothing below may run before this check. Unset Config::authenticate ⇒ unchanged Phase-1
        // behavior (every CONNECT accepted, principal stays "anonymous").
        if (cfg_.authenticate) {
            auto principal = cfg_.authenticate(username, password);
            if (!principal) {
                // 3.2.2.3 (v4) / 3.2.2.2 (v5): reject with the version-appropriate CONNACK shape.
                // session_present/ack_flags is always 0 here — nothing was ever restored. Best-effort send
                // (write failure doesn't change the outcome: the connection closes either way) then close,
                // WITHOUT touching client_id/has_will/client_sessions_/stored_sessions_ — s->client_id etc.
                // are still their pre-CONNECT defaults.
                if (is_v5) {
                    std::vector<std::byte> body{std::byte{0x00}, std::byte{0x86}};  // Bad User Name or Password
                    aero::transport::mqtt::put_empty_properties(body);
                    (void)s->send_packet(std::byte{0x20}, body);
                } else {
                    (void)s->send_packet(std::byte{0x20}, {std::byte{0x00}, std::byte{0x04}});
                }
                return false;
            }
            s->principal = std::move(*principal);
        }

        s->client_id = client_id;
        s->keep_alive_s = keep_alive;
        s->clean_session = clean_session;
        s->session_expiry_interval = connect_session_expiry;  // M7.1: nullopt for v4 / v5-without-property
        s->max_packet_size = connect_max_packet_size;  // M7.2 PR A: nullopt for v4 / v5-without-property
        s->last_activity = std::chrono::steady_clock::now();
        if (will_flag) {
            s->has_will = true;
            s->will_topic = std::move(will_topic);
            s->will_payload = std::move(will_payload);
            s->will_qos = will_qos;
            s->will_retain = will_retain;
            s->will_extras = std::move(will_extras);  // M7.2 PR A
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
                    // M7.1: a Session Expiry Interval TTL that has already elapsed means this stored
                    // session is stale — treat it as if no stored session existed at all (erase, don't
                    // restore subs/queued messages, session_present stays false) instead of resurrecting
                    // state the client no longer has any right to expect back.
                    const bool expired = it->second.expires_at &&
                                        std::chrono::steady_clock::now() >= *it->second.expires_at;
                    if (expired) {
                        stored_sessions_.erase(it);
                    } else {
                        restored_subs = it->second.subs;
                        to_flush.assign(it->second.queued.begin(), it->second.queued.end());
                        stored_sessions_.erase(it);  // ownership moves to this now-live session
                        session_present = true;
                    }
                }
            }
        }
        if (!restored_subs.empty()) {
            std::lock_guard<std::mutex> g(s->subs_mu);
            s->subs = std::move(restored_subs);
        }

        // v4 (unchanged): {session_present_byte, rc_byte}, rc=0. v5: {ack_flags_byte, reason_code_byte}
        // followed by a Properties block — same session-present bit, bit 0, MQTT 5 §3.2.2.1.1 — then
        // reason code 0x00 Success (verified against the spec: CONNACK's v5 variable header is Ack Flags,
        // Reason Code, Properties, in that order). M7.1: the Properties block is no longer empty — it now
        // carries Topic Alias Maximum (0x22) so a v5 client knows the broker accepts inbound aliases up to
        // kTopicAliasMax (MQTT 5 §3.1.2.11.2).
        std::vector<std::byte> body{static_cast<std::byte>(session_present ? 0x01 : 0x00), std::byte{0x00}};
        if (is_v5) aero::transport::mqtt::put_topic_alias_max_properties(body, kTopicAliasMax);
        if (!s->send_packet(std::byte{0x20}, body)) return false;  // CONNACK

        // Flush anything that arrived while this persistent session was offline — AFTER CONNACK, per
        // 3.1.1 (the client only knows to expect them once it's seen session-present=1).
        for (const QueuedMessage& m : to_flush)
            if (!publish_to(*s, m.topic, m.payload, m.qos, /*retain=*/false, m.extras)) return false;
        return true;
    }

    bool handle_subscribe(Session& s, const aero::transport::mqtt::Packet& pkt) {
        const std::vector<std::byte>& b = pkt.body;
        if (b.size() < 2) return false;
        const std::uint16_t packet_id =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        std::size_t pos = 2;

        // M7 v5: SUBSCRIBE Properties (MQTT 5 §3.8.2.1) sit here, before the per-filter payload loop.
        // Discarded — Subscription Identifier/User Property aren't acted on in v1 — but must still be
        // walked past correctly or the filter loop below would misparse the first filter's length as
        // properties bytes. Malformed -> treated like any other malformed SUBSCRIBE (return false).
        if (s.protocol_version == 5) {
            auto props = aero::transport::mqtt::read_properties(b, pos);
            if (!props) return false;
        }

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

            // M5 ACL gate: unset Config::authorizer ⇒ unchanged Phase-1 behavior (everything granted). A
            // denied filter still gets a SUBACK byte (3.9.3 requires one per filter) but it's 0x80
            // (failure) and the filter is NOT added to `added` — so it never enters s.subs and can never
            // match in route_publish()/the retained-replay loop below.
            const bool allowed =
                !cfg_.authorizer || cfg_.authorizer->allow(s.principal, filter, AclAction::Subscribe);
            if (allowed) {
                added.push_back(Subscription{filter, qos});
                granted.push_back(static_cast<std::byte>(qos));
            } else {
                // 3.9.3 (v4): 0x80 SUBACK failure. MQTT 5 §3.9.3: 0x87 Not Authorized is the v5-specific
                // reason code for the same denial — v4 sessions keep 0x80 unchanged.
                granted.push_back(s.protocol_version == 5 ? std::byte{0x87} : std::byte{0x80});
            }
        }
        {
            // Copy, not move — `added` is still read below for the retained-message replay.
            std::lock_guard<std::mutex> g(s.subs_mu);
            for (const Subscription& sub : added) s.subs.push_back(sub);
        }
        // SUBACK variable header (MQTT 5 §3.9.2): Packet Identifier, then Properties, THEN the payload's
        // reason-code list — verified against the spec rather than assumed (Properties precede the
        // reason codes, mirroring PUBACK/PUBREC's own Packet-Id-then-Properties shape elsewhere in v5).
        std::vector<std::byte> vh;
        aero::transport::mqtt::put_u16_be(vh, packet_id);
        if (s.protocol_version == 5) aero::transport::mqtt::put_empty_properties(vh);
        vh.insert(vh.end(), granted.begin(), granted.end());
        if (!s.send_packet(std::byte{0x90}, vh)) return false;  // SUBACK

        // Retained-message replay (3.1.1 §3.8.4): a new matching SUBSCRIBE gets the retained payload
        // immediately, at the subscription's granted QoS. M7.2 PR A: msg.extras carries the retained
        // message's own Message Expiry through — publish_to()'s choke point re-checks it at replay time,
        // so a long-stale retained message is silently dropped instead of replayed as if fresh.
        std::lock_guard<std::mutex> rg(retained_mu_);
        for (const Subscription& sub : added) {
            for (const auto& [topic, msg] : retained_) {
                if (topic_matches(sub.filter, topic)) {
                    if (!publish_to(s, topic, msg.payload, sub.qos, /*retain=*/true, msg.extras)) return false;
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

        // M7 v5: PUBLISH Properties (MQTT 5 §3.3.2.3) sit here — after Packet Identifier for QoS>0, or
        // directly after Topic Name for QoS 0 (there is no packet id to parse first) — and BEFORE the
        // payload. `pos` MUST land exactly past them before the payload slice below runs, or the
        // properties bytes would leak into (or truncate) the delivered payload. Malformed -> treated like
        // any other malformed PUBLISH (return false). M7.1: Topic Alias (0x23) is now captured. M7.2 PR A:
        // Message Expiry Interval (0x02) is now captured too, turned into an absolute deadline below
        // (everything else in the block is still discarded — Response Topic/Correlation Data/User
        // Properties remain out of scope for this PR, see PublishExtras's own comment).
        std::optional<std::uint16_t> topic_alias;
        PublishExtras extras;
        if (s.protocol_version == 5) {
            auto props = aero::transport::mqtt::read_properties(b, pos);
            if (!props) return false;
            topic_alias = props->topic_alias;
            if (props->message_expiry_interval)
                extras.expiry_deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(*props->message_expiry_interval);
        }

        std::vector<std::byte> payload(b.begin() + static_cast<std::ptrdiff_t>(pos), b.end());

        // M7.1: inbound Topic Alias resolution (MQTT 5 §3.3.2.3.4) — MUST happen BEFORE the ACL gate
        // below, so an alias can never be used to bypass per-topic ACL by hiding the real topic string
        // behind a numeric alias. A non-empty topic name + alias together ESTABLISHES/refreshes the
        // mapping; an empty topic name + alias LOOKS UP a previously-established mapping. `topic_alias`
        // is only ever populated when s.protocol_version == 5 (the block above is v5-gated), so
        // `resolved_topic` always equals `topic` unchanged for v4 sessions — zero v4 behavior change.
        std::string resolved_topic = topic;
        if (topic_alias) {
            if (*topic_alias == 0 || *topic_alias > kTopicAliasMax) {
                send_disconnect(s, 0x94);  // Topic Alias invalid (MQTT 5 §3.14.4)
                return false;
            }
            if (!topic.empty()) {
                s.topic_aliases[*topic_alias] = topic;  // establish/refresh
            } else {
                auto it = s.topic_aliases.find(*topic_alias);
                if (it == s.topic_aliases.end()) {
                    send_disconnect(s, 0x94);  // unknown alias
                    return false;
                }
                resolved_topic = it->second;
            }
        }

        // M5 ACL gate: unset Config::authorizer ⇒ unchanged Phase-1 behavior (everything allowed). On
        // denial: deliver_publish() (retain/on_publish_/route_publish) is never called, and for QoS 1/2
        // no ack is sent at all (silent drop — the publisher gets no confirmation, which IS the signal;
        // there is nothing to ack for QoS 0 either way).
        const bool allowed =
            !cfg_.authorizer || cfg_.authorizer->allow(s.principal, resolved_topic, AclAction::Publish);

        if (qos == 2) {
            // DOCUMENTED CHOICE: QoS 2's authorization check happens HERE, at PUBLISH/PUBREC time, not
            // deferred to the PUBREL-time actual-delivery step in handle_pubrel below — an unauthorized
            // QoS-2 publisher doesn't even get a PUBREC, matching the QoS-1 silent-drop treatment exactly
            // (both "ack the client would use to know it can move on" are withheld at the earliest wire
            // point that knows the verdict). A denied publish is simply never stashed in qos2_inflight, so
            // handle_pubrel's later "unknown packet_id" tolerance (a harmless ack-only no-op, see its own
            // comment) is what a stray PUBREL for it hits — deliver_publish() still never runs.
            if (!allowed) return true;  // no PUBREC — connection stays open, handshake just never proceeds
            // Exactly-once (4.3.3): stash until PUBREL confirms receipt, then — and ONLY then — retain/
            // ingest/route it. Re-inserting under the same packet_id on a retransmitted PUBLISH (the
            // sender's PUBREC was lost) is a harmless overwrite with identical content, not a double
            // routing — the actual routing only ever happens once, from handle_pubrel.
            s.qos2_inflight[packet_id] = PendingQos2{resolved_topic, payload, retain, extras};
            std::vector<std::byte> ack;
            aero::transport::mqtt::put_u16_be(ack, packet_id);
            return s.send_packet(std::byte{0x50}, ack);  // PUBREC
        }

        // QoS 0/1: act BEFORE acking — a publisher that has received its PUBACK must be able to assume a
        // subsequent SUBSCRIBE/retained-replay from anyone will already see this value/delivery (no
        // ack-before-visible race).
        if (allowed) deliver_publish(resolved_topic, payload, qos, retain, extras);
        if (qos == 1) {
            if (!allowed) return true;  // silent drop: no PUBACK
            std::vector<std::byte> ack;
            aero::transport::mqtt::put_u16_be(ack, packet_id);
            if (!s.send_packet(std::byte{0x40}, ack)) return false;  // PUBACK
        }
        return true;
    }

    // PUBREL (4.3.3 step 3): the sender has now durably committed to this packet_id, so this is where a
    // QoS-2 PUBLISH FINALLY gets acted on — the one thing that distinguishes QoS 2 from QoS 1 (which acts
    // immediately). PUBCOMP is sent unconditionally, even for an unknown packet_id (a retried PUBREL after
    // we already completed it and forgot it) — the sender's handshake must terminate either way. M5: no
    // separate ACL check here — an unauthorized QoS-2 publish was already refused a PUBREC in
    // handle_publish (see its "DOCUMENTED CHOICE" comment) and so was never stashed here; this path only
    // ever delivers something that already passed the gate.
    bool handle_pubrel(Session& s, const aero::transport::mqtt::Packet& pkt) {
        const std::vector<std::byte>& b = pkt.body;
        if (b.size() < 2) return false;
        const std::uint16_t packet_id =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        const auto it = s.qos2_inflight.find(packet_id);
        if (it != s.qos2_inflight.end()) {
            // M7.2 PR A: pass the PendingQos2 entry's STORED extras through, not a fresh empty one — a
            // QoS2 message's Message Expiry was already computed at PUBLISH time and must survive
            // unchanged to this PUBREL-time delivery (see PendingQos2's own comment).
            deliver_publish(it->second.topic, it->second.payload, /*qos=*/2, it->second.retain,
                            it->second.extras);
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
                         bool retain, const PublishExtras& extras = {}) {
        if (retain) {
            std::lock_guard<std::mutex> g(retained_mu_);
            if (payload.empty())
                retained_.erase(topic);  // zero-length retained PUBLISH clears retention (3.1.1 §3.3.1.3)
            else
                retained_[topic] = RetainedMessage{payload, extras};  // M7.2 PR A: carry extras along
        }
        if (on_publish_) on_publish_(topic, payload, qos);
        route_publish(topic, payload, qos, extras);
        // M6 (017 §4): AFTER local delivery, so a peer's relayed copy can never arrive before this node's
        // own subscribers see it. No-op single-node default (peer_forwarder_ unset) — see set_peer_forwarder().
        if (peer_forwarder_) peer_forwarder_(topic, payload, qos);
    }

    // Fan out to every connected session with a matching subscription, at min(publish qos, granted
    // qos) — a snapshot copy of sessions_ is taken under the lock so the actual socket writes (which
    // may block briefly on backpressure) never happen while holding sessions_mu_. Also queues into any
    // OFFLINE persistent (clean_session=0) session whose stored subscriptions match — QoS ≥1 only, since
    // there is nothing to durably queue a QoS-0 "at most once" message as. M7.2 PR A: `extras` (Message
    // Expiry, primarily) is threaded through to both the live fan-out (publish_to's own choke point
    // re-checks it at actual send time) and the offline queue (QueuedMessage::extras, re-checked when
    // flushed on reconnect).
    void route_publish(const std::string& topic, const std::vector<std::byte>& payload, std::uint8_t qos,
                       const PublishExtras& extras = {}) {
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
                (void)publish_to(*session, topic, payload, deliver_qos, /*retain=*/false, extras);
            }
        }

        if (qos == 0) return;
        std::lock_guard<std::mutex> g(stored_sessions_mu_);
        for (auto& [client_id, stored] : stored_sessions_) {
            for (const Subscription& sub : stored.subs) {
                if (!topic_matches(sub.filter, topic)) continue;
                const std::uint8_t deliver_qos = qos < sub.qos ? qos : sub.qos;
                if (stored.queued.size() >= kQueuedMessageCap) stored.queued.pop_front();  // drop-oldest
                stored.queued.push_back(QueuedMessage{topic, payload, deliver_qos, extras});
                break;  // one queued copy per client-id even if multiple subs match the same topic
            }
        }
    }

    // Best-effort server->client DISCONNECT (MQTT 5 §3.14) — v5 only; a v4 session has no such packet, so
    // this is a silent no-op for it (every call site's socket-close behavior is unchanged for v4).
    void send_disconnect(Session& s, std::uint8_t reason_code) {
        if (s.protocol_version != 5) return;
        std::vector<std::byte> body{static_cast<std::byte>(reason_code)};
        aero::transport::mqtt::put_empty_properties(body);
        (void)s.send_packet(std::byte{0xE0}, body);
    }

    // 017 M7.2 PR A: this function used to write ZERO Properties bytes for outbound PUBLISH, for EVERY
    // protocol version — not even an empty MQTT 5 Properties field, which is actually a latent correctness
    // bug (§3.3.1 requires one, mandatory-even-empty, for v5; see this file's M7/M7.1 banners' own honest
    // "outbound PUBLISH framing is untouched" scope notes, now superseded). This rewrite fixes that AS A
    // DOCUMENTED BYPRODUCT while adding the two PR A choke points below. v4 sessions never enter the
    // `protocol_version == 5` branch — wire shape is byte-for-byte unchanged for them, a hard invariant.
    bool publish_to(Session& s, const std::string& topic, const std::vector<std::byte>& payload,
                    std::uint8_t qos, bool retain, const PublishExtras& extras = {}) {
        // Choke point 1 — Message Expiry: check first, before building anything, so an already-stale
        // message (offline queue flush, retained replay, or plain fanout racing a long expiry) never gets
        // serialized. Silent drop (return true, not false) — mirrors this file's existing ACL-denial idiom:
        // "this one message doesn't go out" must not tear the session down.
        if (extras.expiry_deadline && std::chrono::steady_clock::now() >= *extras.expiry_deadline) {
            return true;
        }

        std::vector<std::byte> vh;
        aero::transport::mqtt::put_str(vh, topic);
        if (qos > 0) aero::transport::mqtt::put_u16_be(vh, next_packet_id());

        if (s.protocol_version == 5) {
            aero::transport::mqtt::PropertyWriter pw;
            if (extras.expiry_deadline) {
                const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                    *extras.expiry_deadline - std::chrono::steady_clock::now())
                                          .count();
                // remaining > 0 here — choke point 1 above already returned on <= 0.
                pw.put_u32(0x02, static_cast<std::uint32_t>(remaining));
            }
            // A future PR B adds here: put_str(0x08, response_topic), put_binary(0x09, correlation_data),
            // a put_str_pair(0x26, ...) loop for user_properties — extras is already shaped for all three
            // (PublishExtras's own comment), this PR just never populates them.
            std::vector<std::byte> props;
            pw.write(props);  // valid Properties block even when pw is empty (one 0x00 byte) — the §3.3.1 fix
            vh.insert(vh.end(), props.begin(), props.end());
        }
        vh.insert(vh.end(), payload.begin(), payload.end());

        // Choke point 2 — Maximum Packet Size: after Properties are written (so the check covers the real
        // wire size, not an underestimate), before send.
        if (s.max_packet_size) {
            std::vector<std::byte> len_scratch;
            aero::transport::mqtt::put_remaining_length(len_scratch, static_cast<std::uint32_t>(vh.size()));
            const std::size_t total = 1 + len_scratch.size() + vh.size();  // fixed-header byte + rem-len + vh
            if (total > *s.max_packet_size) return true;  // silent drop, same idiom as choke point 1
        }

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

    // M5: the TLS listener/context — all stay at their default (invalid_fd / 0 / null / not-joinable)
    // and every TLS-specific step in start()/stop()/accept_loop_tls() is a no-op when cfg_.tls is unset.
    quark::pal::fd_t listen_fd_tls_ = quark::pal::invalid_fd;
    std::uint16_t resolved_port_tls_ = 0;
    std::thread accept_thread_tls_;
    std::unique_ptr<aero::pal::tls::TlsServerContext> tls_ctx_;

    std::mutex sessions_mu_;
    std::vector<std::shared_ptr<Session>> sessions_;
    std::vector<std::thread> session_threads_;

    std::mutex retained_mu_;
    std::unordered_map<std::string, RetainedMessage> retained_;

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
