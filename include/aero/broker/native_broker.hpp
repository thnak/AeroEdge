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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

// 017 M7.2 PR B: the subset of MQTT 5 PUBLISH properties exposed on the PUBLIC on_publish() callback —
// Response Topic/Correlation Data/User Properties, the request/response pattern (§3.3.2.3.5-§3.3.2.3.7).
// A DELIBERATELY SEPARATE type from the private PublishExtras (below, inside NativeBroker): PublishExtras
// also bundles expiry_deadline, a steady_clock::time_point that is monotonic and meaningless outside
// publish_to()'s choke point — exposing it on a public callback would leak an implementation detail and
// force PublishExtras itself to become public. A small conversion at the two firing sites
// (to_publish_properties() below) keeps the public API surface independent of internal delivery-path
// bookkeeping.
struct PublishProperties {
    std::optional<std::string> response_topic;
    std::optional<std::vector<std::byte>> correlation_data;
    std::vector<std::pair<std::string, std::string>> user_properties;
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

        // 017 Phase 7: start the reactor loop + its hand-off worker pool BEFORE accept_thread_ — plaintext
        // connections are handed to the reactor the moment they're accepted (accept_loop()'s
        // reactor_io_.post()), so the reactor must already be running to receive them.
        reactor_thread_ = std::thread([this] { reactor_io_.run(); });
        reactor_thread_id_ = reactor_thread_.get_id();  // available immediately, no need to wait for run()
        handoff_workers_.reserve(kLegacyHandoffWorkerCount);
        for (int i = 0; i < kLegacyHandoffWorkerCount; ++i)
            handoff_workers_.emplace_back([this] { handoff_worker_loop(); });

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

        // 017 Phase 7: stop the reactor loop and join its thread BEFORE touching any reactor session's
        // state directly — mirrors quark::net::TcpTransport::stop()'s own shape (stop the loop, join the
        // I/O thread; only THEN is it safe to touch remaining connection state with no concurrency risk,
        // since nothing can dispatch a readiness/timer/posted callback once run() has returned).
        reactor_io_.stop();
        if (reactor_thread_.joinable()) reactor_thread_.join();

        {
            std::lock_guard<std::mutex> g(handoff_mu_);
            handoff_stop_ = true;
        }
        handoff_cv_.notify_all();
        for (std::thread& t : handoff_workers_)
            if (t.joinable()) t.join();
        handoff_workers_.clear();

        std::vector<std::shared_ptr<Session>> reactor_sessions_to_teardown;
        {
            std::lock_guard<std::mutex> g(sessions_mu_);
            for (std::thread& t : session_threads_)
                if (t.joinable()) t.join();
            session_threads_.clear();
            // Every legacy session's own thread already called teardown_session() (Will delivery,
            // persistent-session handoff, remove_session, close_fd) as the last thing it did before
            // exiting, per session_loop()'s existing unchanged shape — so `sessions_` should already be
            // empty of legacy entries by the time the joins above return. Reactor sessions never had a
            // session_threads_ entry (accept_loop() hands them to the reactor instead) — collect them
            // here and tear them down explicitly, OUTSIDE this lock (teardown_session() -> remove_session()
            // re-acquires sessions_mu_ itself; calling it while still holding the lock here would
            // self-deadlock).
            for (auto& s : sessions_)
                if (s->is_reactor_session) reactor_sessions_to_teardown.push_back(s);
            sessions_.clear();
        }
        for (auto& s : reactor_sessions_to_teardown) teardown_session(s, /*clean_disconnect=*/false);

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
    // 017 M7.2 PR B: gained a 4th parameter, `props` (Response Topic/Correlation Data/User Properties) —
    // always populated when the originating PUBLISH carried them (v4 sessions and relayed PUBLISHes via
    // deliver_remote_publish() below just see a default-constructed, all-empty PublishProperties).
    void on_publish(std::function<void(std::string_view topic, std::span<const std::byte> payload,
                                       std::uint8_t qos, const PublishProperties& props)> cb) {
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
        // 017 M7.2 PR B: relayed PUBLISHes never carry Response Topic/Correlation Data/User Properties
        // yet — same documented v1 cross-node gap PR A already left for Message Expiry (see this
        // function's own banner above); a future cross-node-relay PR would thread real extras through.
        if (on_publish_) on_publish_(topic, payload, qos, PublishProperties{});
        route_publish(std::string(topic), std::vector<std::byte>(payload.begin(), payload.end()), qos);
    }

private:
    struct Subscription {
        std::string filter;
        std::uint8_t qos = 0;  // granted QoS (v1: min(requested, 1))
    };

    // 017 M7.2: carries the subset of MQTT 5 PUBLISH properties this broker acts on end-to-end, from
    // ingestion (handle_publish/Will-parse) through to the outbound wire (publish_to). PR A populated only
    // expiry_deadline; PR B (this milestone) populates response_topic/correlation_data/user_properties too
    // — shaped from the start so PR B didn't need a second pass touching every call site below.
    struct PublishExtras {
        std::optional<std::chrono::steady_clock::time_point> expiry_deadline;  // absolute deadline,
            // computed ONCE at ingestion time — never re-derived from a re-sent "original interval".
        std::optional<std::string> response_topic;                // 017 M7.2 PR B
        std::optional<std::vector<std::byte>> correlation_data;    // 017 M7.2 PR B
        std::vector<std::pair<std::string, std::string>> user_properties;  // 017 M7.2 PR B
    };

    // 017 M7.2 PR B: converts the private, delivery-path-internal PublishExtras into the public
    // PublishProperties shape fired on on_publish() — drops expiry_deadline (meaningless outside
    // publish_to()'s choke point, see PublishProperties's own comment). Called at the two on_publish_
    // firing sites (deliver_publish below; deliver_remote_publish above fires PublishProperties{}
    // directly since a relayed PUBLISH's PublishExtras is always default anyway).
    static PublishProperties to_publish_properties(const PublishExtras& e) {
        return PublishProperties{e.response_topic, e.correlation_data, e.user_properties};
    }

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

    // 017 Phase 7: the per-Session outbound queue item shape for reactor-managed (plaintext) sessions.
    // Exactly one of {raw item, logical item} ever exists per QueuedOutbound — kept as two separate
    // struct types (rather than one struct with an is-raw flag) so each shape only carries the fields it
    // actually needs. `Raw` covers Session::send_packet()'s direct replies (CONNACK/SUBACK/PINGRESP/
    // PUBACK/PUBREC/PUBCOMP/DISCONNECT) — already fully built by their caller, no choke points apply.
    // `Publish` covers route_publish()'s fan-out deliveries — deliberately the LOGICAL message (topic/
    // payload/qos/retain/extras), not pre-serialized bytes, so the Message-Expiry-Interval and Maximum-
    // Packet-Size choke points (publish_to()'s own, reused via build_publish_variable_header_and_payload()
    // below) are re-checked at ACTUAL send time — which can be well after enqueue time for a backed-up
    // recipient — exactly the correctness fix Phase 6/§3.0's critique already established and Phase 6
    // itself preserved; only the wall-clock-blocking problem that sank Phase 6 is what this phase fixes.
    struct QueuedOutboundRaw {
        std::byte type_flags;
        std::vector<std::byte> body;
    };
    struct QueuedOutboundPublish {
        std::string topic;
        std::vector<std::byte> payload;
        std::uint8_t qos = 0;
        bool retain = false;
        PublishExtras extras;
    };
    struct QueuedOutbound {
        std::optional<QueuedOutboundRaw> raw;
        std::optional<QueuedOutboundPublish> publish;
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
    //
    // 017 Phase 7: `is_reactor_session` is `const`, set ONLY via the constructor (never a post-
    // construction assignment) — a Plan-agent critique of this phase's draft flagged that a post-
    // construction write has no established happens-before relationship with the session becoming
    // visible to other threads (the reactor thread via reactor_io_.post(), or a fan-out thread reading it
    // off the topic index), which would be an unsynchronized data race, not just a logic bug. Making it
    // `const` turns "set before any other thread can see this object" into a property the compiler
    // enforces, not documentation-only discipline — exactly `protocol_version`'s "negotiated once,
    // read-only after" precedent, but load-bearing from construction rather than from handle_connect.
    // The plaintext constructor (below) is reactor-managed; the TLS constructor never is (017 Phase 7's
    // own scope decision — TLS reactor integration is explicitly deferred, see the design doc).
    // `enable_shared_from_this` lets any Session member function obtain a weak_ptr to itself to safely
    // capture in a lambda handed to `reactor_io_.post()` (mirrors voice_channel.hpp's State class, which
    // solves the exact same "outlive an async continuation" problem the same way).
    struct Session : std::enable_shared_from_this<Session> {
        explicit Session(quark::pal::fd_t f)
            : fd(f), channel(aero::transport::PlainChannel{f}), is_reactor_session(true) {}
        Session(quark::pal::fd_t f, aero::pal::tls::TlsSession tls_session)
            : fd(f),
              tls_owner(std::make_unique<aero::pal::tls::TlsSession>(std::move(tls_session))),
              channel(aero::transport::TlsChannel{tls_owner.get()}),
              is_reactor_session(false) {}

        quark::pal::fd_t fd;
        std::unique_ptr<aero::pal::tls::TlsSession> tls_owner;  // non-null only for a TLS session
        std::variant<aero::transport::PlainChannel, aero::transport::TlsChannel> channel;
        const bool is_reactor_session;  // see banner above — plaintext (reactor-managed) vs TLS (legacy)

        std::mutex io_mu;
        std::mutex subs_mu;
        std::vector<Subscription> subs;

        // 017 Phase 7: reactor-session-only outbound state, all guarded by io_mu (reuses the existing
        // mutex rather than adding a new one — this state only ever interacts with `channel`, which io_mu
        // already protects). Never touched for a legacy (TLS) session — is_reactor_session gates every
        // access. `out_queue` holds items not yet started; `out_current`/`out_sent` track the ONE packet
        // currently mid-send (mirrors quark::net::tcp_transport.hpp's `Conn::out`/`out_sent` exactly:
        // rotation to the next queued item only happens BETWEEN whole packets, never mid-packet).
        // `write_interest_armed` avoids redundant mod_fd() calls once EPOLLOUT is already registered.
        std::deque<QueuedOutbound> out_queue;
        std::vector<std::byte> out_current;
        std::size_t out_sent = 0;
        bool write_interest_armed = false;
        // 017 Phase 7: guards against a reactor session being torn down twice (e.g. a hard write error
        // during try_drain_reactor_send and a concurrently-detected EOF both trying to schedule teardown)
        // — schedule_reactor_teardown() only actually posts the teardown task for whichever caller wins
        // the exchange. Legacy sessions never need this: session_loop's single owning thread only ever
        // calls teardown_session() once, at the very end of its own loop, by construction.
        std::atomic<bool> teardown_scheduled{false};

        // 017 Phase 3: the buffered-read inbound byte buffer (redesign doc §2.4 Experiment A / §3.1) —
        // same single-thread-owned discipline as client_id/keep_alive_s/etc. below (only this session's
        // own reader thread, session_loop, ever touches these — no lock needed). read_pos marks how much
        // of read_buf's prefix has already been dispatched as complete packets; session_loop compacts it
        // back to 0 once per outer read cycle.
        std::vector<std::byte> read_buf;
        std::size_t read_pos = 0;

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

        // 017 Phase 7: the old Session::send_packet() member moved out to a NativeBroker method of the
        // same name (see its definition below, near build_publish_variable_header_and_payload()) — it
        // needs to reach reactor_io_/try_drain_reactor_send() for reactor sessions, which a nested
        // struct's inline member body cannot do (NativeBroker is still an incomplete type at this point
        // in the class definition). Every call site in this file changed from `s->send_packet(...)` /
        // `s.send_packet(...)` to `send_packet(*s, ...)` / `send_packet(s, ...)` — a mechanical rename
        // only, no business logic touched.
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

    // 017 Phase 7: plaintext connections are reactor-managed (Session::is_reactor_session == true, set by
    // the constructor used here) — accept_loop() itself is UNCHANGED above the registration step; only
    // "spawn a session_threads_ entry" became "hand the session to the reactor via post()" (add_fd is
    // loop-thread-only per IoContext's documented contract, so registration must run ON reactor_thread_,
    // not here on the accept thread). accept_loop_tls() (below) is completely untouched — TLS sessions
    // stay on the legacy thread-per-connection path this round (see the design doc's scope decision).
    void accept_loop() {
        while (running_.load(std::memory_order_acquire)) {
            const auto ready = aero::pal::wait_readable(listen_fd_, 200);
            if (!ready || !*ready) continue;  // timeout/err → re-check running_
            auto cfd = quark::pal::accept_one(listen_fd_);
            if (!cfd) continue;
            auto session = std::make_shared<Session>(*cfd);
            {
                std::lock_guard<std::mutex> g(sessions_mu_);
                sessions_.push_back(session);
            }
            reactor_io_.post([this, session] { register_reactor_session(session); });
        }
    }

    // ============================================================================================
    // 017 Phase 7: IoContext reactor machinery for plaintext sessions. Everything in this block only
    // ever touches a reactor-managed Session (is_reactor_session == true) — legacy (TLS) sessions and
    // session_loop()/teardown_session() below are otherwise unaffected by this block's existence.
    // ============================================================================================

    // 200ms — matches legacy session_loop()'s own wait_readable(fd, 200) poll cadence, so keep-alive
    // promptness for reactor sessions is no worse than the legacy path's.
    static constexpr std::int64_t kReactorKeepAliveSweepNs = 200'000'000;

    [[nodiscard]] bool is_on_reactor_thread() const noexcept {
        return std::this_thread::get_id() == reactor_thread_id_;
    }

    // Runs ON the reactor loop thread (always called via reactor_io_.post() from accept_loop() above, per
    // add_fd()'s loop-thread-only contract). weak_ptr-captured (never a raw Session*, unlike
    // voice_channel.hpp's own zero-allocation-optimized precedent — this round deliberately favors
    // simplicity over that optimization; see 7a's own scope note: correctness this round, not throughput)
    // so a session already torn down by the time a stale readiness event's dispatch runs is a safe no-op.
    void register_reactor_session(const std::shared_ptr<Session>& s) {
        std::weak_ptr<Session> weak = s;
        reactor_io_.add_fd(s->fd, EPOLLIN, [this, weak](std::uint32_t events) {
            auto sp = weak.lock();
            if (!sp) return;
            on_reactor_ready(sp, events);
        });
        arm_keep_alive_sweep(s);
    }

    // Perpetual self-rescheduling timer (mirrors quark::net::voice_channel.hpp's arm_idle_sweep() exactly)
    // — replaces legacy session_loop()'s 200ms-poll-as-heartbeat idiom for reactor sessions. Always runs
    // on the reactor loop thread (post_after() callbacks fire from inside IoContext::run() itself), so
    // this never needs marshaling.
    void arm_keep_alive_sweep(const std::shared_ptr<Session>& s) {
        std::weak_ptr<Session> weak = s;
        reactor_io_.post_after(kReactorKeepAliveSweepNs, [this, weak] {
            auto sp = weak.lock();
            if (!sp) return;
            if (sp->teardown_scheduled.load(std::memory_order_acquire)) return;
            if (keep_alive_expired(*sp)) {
                send_disconnect(*sp, 0x8D);  // Keep Alive timeout (M7.1) — best-effort, see
                                              // schedule_reactor_teardown()'s final-flush-attempt comment
                schedule_reactor_teardown(sp, false);
                return;
            }
            arm_keep_alive_sweep(sp);
        });
    }

    // The reactor ReadyHandler's actual body — dispatched for EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP on a
    // reactor session's fd. Reuses Phase 3's exact buffered-read inner drain loop (try_parse_packet() +
    // dispatch to the SAME unchanged handle_connect/handle_subscribe/handle_publish/handle_pubrel) —
    // only the outer driving mechanism differs from session_loop(): event-driven readiness instead of a
    // blocking poll, and no thread-owned while(running_) loop to `break` out of — a session's lifetime
    // here is entirely registration-driven (add_fd/del_fd), so every exit path calls
    // schedule_reactor_teardown() instead.
    void on_reactor_ready(const std::shared_ptr<Session>& s, std::uint32_t events) {
        if (s->teardown_scheduled.load(std::memory_order_acquire)) return;  // already going away

        if (events & EPOLLOUT) {
            std::lock_guard<std::mutex> g(s->io_mu);
            try_drain_reactor_send(s);
        }
        if (s->teardown_scheduled.load(std::memory_order_acquire)) return;  // a hard write error above

        // Phase 7c fix: EPOLLHUP/EPOLLERR must NOT short-circuit before draining EPOLLIN. The Windows
        // WSAPoll backend (pal/windows_x86_64/net.hpp) reports POLLHUP alongside POLLRDNORM whenever a
        // peer writes a final burst and closes immediately after (the ordinary shape of a QoS-0
        // fire-and-forget publisher) — tearing down on that combination before reading discarded data
        // that was already fully received into the kernel socket buffer, a real message-loss bug found via
        // broker_bench (017 Phase 7c), not a hypothetical: a 1-publisher/1-subscriber QoS-0 burst of just
        // 10 messages lost ~20-70% of them, permanently, well within any timeout. recv_some()'s own return
        // value is the authoritative signal (0 = EOF, error = real fault, would-block = nothing left) —
        // EPOLLHUP/EPOLLERR alone must never skip a read that might still have data behind it.
        if (!(events & (EPOLLIN | EPOLLHUP | EPOLLERR))) return;

        std::byte recv_scratch[4096];
        auto got = std::visit(
            [&](auto& ch) { return ch.recv_some(recv_scratch, sizeof(recv_scratch)); }, s->channel);
        if (!got) {
            if (got.error() == quark::pal::would_block()) {
                // Nothing readable right now. A HUP/ERR-flagged dispatch with genuinely no data left means
                // the peer is gone — tear down. A HUP flagged alongside IN that we already fully drained
                // in an earlier call (or a spurious HUP with no IN at all) just waits for the next event.
                if (events & (EPOLLHUP | EPOLLERR)) schedule_reactor_teardown(s, false);
                return;
            }
            schedule_reactor_teardown(s, false);
            return;
        }
        if (*got == 0) {  // peer closed — same Ok(0)/EOF contract session_loop() uses
            schedule_reactor_teardown(s, false);
            return;
        }
        s->read_buf.insert(s->read_buf.end(), recv_scratch, recv_scratch + *got);

        bool clean_disconnect = false;
        bool stop_session = false;
        while (true) {
            if (s->kicked.load(std::memory_order_acquire)) {
                send_disconnect(*s, 0x8E);  // Session taken over (M7.1)
                stop_session = true;
                break;
            }
            auto pkt = aero::transport::mqtt::try_parse_packet(s->read_buf, s->read_pos);
            if (!pkt) {
                if (pkt.error() == aero::transport::mqtt::ParseStatus::Malformed) stop_session = true;
                break;
            }
            s->last_activity = std::chrono::steady_clock::now();
            const std::uint8_t type = pkt->type_flags & 0xF0;
            if (type == 0x10) {          // CONNECT
                if (!handle_connect(s, *pkt)) { stop_session = true; break; }
            } else if (type == 0x80) {   // SUBSCRIBE
                if (!handle_subscribe(s, *pkt)) { stop_session = true; break; }
            } else if (type == 0x30) {   // PUBLISH
                if (!handle_publish(*s, *pkt)) { stop_session = true; break; }
            } else if (type == 0x60) {   // PUBREL
                if (!handle_pubrel(*s, *pkt)) { stop_session = true; break; }
            } else if (type == 0xC0) {   // PINGREQ
                send_packet(*s, std::byte{0xD0}, {});  // PINGRESP — reactor sessions never report a
                                                        // synchronous failure here, see send_packet()'s banner
            } else if (type == 0xE0) {   // DISCONNECT
                s->has_will = false;
                clean_disconnect = true;
                stop_session = true;
                break;
            }
        }
        if (s->read_pos > 0) {
            s->read_buf.erase(s->read_buf.begin(),
                              s->read_buf.begin() + static_cast<std::ptrdiff_t>(s->read_pos));
            s->read_pos = 0;
        }
        if (stop_session) schedule_reactor_teardown(s, clean_disconnect);
    }

    // Tears down a reactor session. ALWAYS defers the actual work via reactor_io_.post(), even when
    // already called from the reactor loop thread — deliberate, not an oversight: some callers (e.g.
    // try_drain_reactor_send on a hard write error) are invoked WHILE still holding s->io_mu; running
    // teardown inline there would self-deadlock the moment teardown_session() below takes io_mu again
    // (the close_fd race fix). Deferring unconditionally means teardown always runs on a fresh call
    // stack, after any caller's lock_guard has released — no per-call-site special-casing needed.
    // `teardown_scheduled`'s exchange ensures only the FIRST of possibly-several concurrent triggers
    // (hard write error, EOF, parse error, keep-alive expiry, session takeover) actually posts the task —
    // without this, two racing triggers would each successfully weak_ptr::lock() and run
    // teardown_session() twice (double Will delivery, double close_fd on a possibly-already-recycled fd
    // number). weak_ptr-captured so a session already destroyed by the time this runs is a safe no-op.
    void schedule_reactor_teardown(const std::shared_ptr<Session>& s, bool clean_disconnect) {
        if (s->teardown_scheduled.exchange(true, std::memory_order_acq_rel)) return;
        std::weak_ptr<Session> weak = s;
        reactor_io_.post([this, weak, clean_disconnect] {
            auto sp = weak.lock();
            if (!sp) return;
            {
                // Best-effort final flush attempt (never blocks) — gives whatever's already queued (e.g.
                // a DISCONNECT reason code enqueued by the same call that triggered this teardown) a
                // chance to actually go out before the socket closes, matching legacy sessions' own
                // best-effort posture for send_disconnect(). Whatever doesn't fit is simply not sent.
                std::lock_guard<std::mutex> g(sp->io_mu);
                try_drain_reactor_send(sp);
            }
            reactor_io_.del_fd(sp->fd);
            teardown_session(sp, clean_disconnect);
        });
    }

    // Precondition: caller holds s->io_mu. Arms EPOLLOUT interest for `s` if not already armed —
    // marshaled via reactor_io_.post() when the calling thread isn't the reactor loop thread itself
    // (mod_fd() is documented loop-thread-only, not internally synchronized). Fire-and-forget is
    // sufficient: the caller doesn't need to know synchronously when interest actually gets armed.
    void arm_write_interest(const std::shared_ptr<Session>& s) {
        if (s->write_interest_armed) return;
        s->write_interest_armed = true;
        if (is_on_reactor_thread()) {
            reactor_io_.mod_fd(s->fd, EPOLLIN | EPOLLOUT);
            return;
        }
        std::weak_ptr<Session> weak = s;
        reactor_io_.post([this, weak] {
            auto sp = weak.lock();
            if (sp) reactor_io_.mod_fd(sp->fd, EPOLLIN | EPOLLOUT);
        });
    }

    // route_publish()'s fan-out loop calls this for a reactor-managed recipient, from ANY thread. Never
    // blocks. Queues the LOGICAL message (see QueuedOutboundPublish's own banner) so choke points
    // re-run at actual send time, not enqueue time.
    void enqueue_reactor_publish(const std::shared_ptr<Session>& s, const std::string& topic,
                                 const std::vector<std::byte>& payload, std::uint8_t qos, bool retain,
                                 const PublishExtras& extras) {
        std::lock_guard<std::mutex> g(s->io_mu);
        s->out_queue.push_back(
            QueuedOutbound{std::nullopt, QueuedOutboundPublish{topic, payload, qos, retain, extras}});
        try_drain_reactor_send(s);
    }

    // Precondition: caller holds s->io_mu. Serializes+sends as much of the outbound queue as possible
    // WITHOUT EVER BLOCKING the calling thread — on a genuine would-block it arms EPOLLOUT and returns;
    // the reactor's own later EPOLLOUT dispatch resumes this same function. This is the mechanism that
    // makes 017 Phase 7 actually different from Phase 6: Phase 6's item-count cap bounded how many items
    // a flush drained, not how long any ONE blocking send could take. Here, no send ever blocks at all —
    // a would-block returns control to the reactor loop instead of waiting on it.
    void try_drain_reactor_send(const std::shared_ptr<Session>& s) {
        for (;;) {
            while (s->out_current.empty() && !s->out_queue.empty()) {
                QueuedOutbound item = std::move(s->out_queue.front());
                s->out_queue.pop_front();
                std::byte type_flags{};
                std::vector<std::byte> body;
                bool ok;
                if (item.raw) {
                    type_flags = item.raw->type_flags;
                    body = std::move(item.raw->body);
                    ok = true;
                } else {
                    const QueuedOutboundPublish& p = *item.publish;
                    std::uint8_t flags = static_cast<std::uint8_t>(p.qos << 1);
                    if (p.retain) flags |= 0x01;
                    type_flags = static_cast<std::byte>(0x30 | flags);
                    ok = build_publish_variable_header_and_payload(*s, p.topic, p.payload, p.qos, p.extras,
                                                                    body);
                }
                if (!ok) continue;  // choke point said skip this item — try the next queued one
                s->out_current = aero::transport::mqtt::serialize_packet(type_flags, body);
                s->out_sent = 0;
            }
            if (s->out_current.empty()) {  // queue fully drained
                if (s->write_interest_armed) {
                    s->write_interest_armed = false;
                    if (is_on_reactor_thread()) {
                        reactor_io_.mod_fd(s->fd, EPOLLIN);
                    } else {
                        std::weak_ptr<Session> weak = s;
                        reactor_io_.post([this, weak] {
                            auto sp = weak.lock();
                            if (sp) reactor_io_.mod_fd(sp->fd, EPOLLIN);
                        });
                    }
                }
                return;
            }
            auto w = std::visit(
                [&](auto& ch) {
                    return ch.send_some(s->out_current.data() + s->out_sent,
                                        s->out_current.size() - s->out_sent);
                },
                s->channel);
            if (w) {
                s->out_sent += *w;
                if (s->out_sent >= s->out_current.size()) {
                    s->out_current.clear();
                    s->out_sent = 0;
                }
                continue;  // more of this packet to send, or move on to the next queued item
            }
            if (w.error() != quark::pal::would_block()) {
                // Hard I/O error — discard the rest of the queue and tear this session down
                // asynchronously; never call back into teardown machinery from inside a non-blocking
                // send attempt (schedule_reactor_teardown() defers for exactly this reason).
                s->out_queue.clear();
                s->out_current.clear();
                s->out_sent = 0;
                schedule_reactor_teardown(s, /*clean_disconnect=*/false);
                return;
            }
            arm_write_interest(s);
            return;
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

    // 017 Phase 3: buffered read (redesign doc §2.4 Experiment A / §3.1) — one recv_some() burst here can
    // hand over many packets' worth of bytes at once (or a partial one, split across TCP segments); the
    // inner drain loop below carves every complete packet currently in `s->read_buf` via
    // try_parse_packet() before this outer loop polls again, replacing the old one-outer-iteration-per-
    // packet call into read_packet() (which did its own internal per-byte recv_some()-or-poll cycle: 3+
    // syscalls per packet, unconditionally). `read_packet()`/`read_n()` themselves are UNCHANGED — still
    // used by MqttClientTransport, bridge.hpp, and every test's hand-rolled client.
    void session_loop(const std::shared_ptr<Session>& s) {
        bool clean_disconnect = false;  // true only for an explicit DISCONNECT (0xE0) — gates the Will
        while (running_.load(std::memory_order_acquire)) {
            // Checked every outer iteration (not just on a read timeout): a session-takeover CONNECT
            // (3.1.4) must end this session promptly even if it's mid-read of something else. Also
            // re-checked inside the inner drain loop below (see its own comment) — a single recv_some()
            // burst can hand over several buffered packets before this outer check would run again.
            if (s->kicked.load(std::memory_order_acquire)) {
                send_disconnect(*s, 0x8E);  // Session taken over (M7.1)
                break;
            }

            // Explicit poll-then-read (rather than letting recv_some's would-block handling loop
            // internally) so a silent-but-still-open connection gets its keep-alive checked every ~200ms
            // too, not just when data actually shows up (mirrors tcp_transport.hpp's accept_loop
            // poll-timeout-as-heartbeat idiom — unchanged from before this Phase 3 change).
            const auto ready = aero::pal::wait_readable(s->fd, 200);
            if (!ready) break;  // poll itself failed
            if (!*ready) {
                if (keep_alive_expired(*s)) {  // 3.1.1 §3.1.2.10 — silent longer than 1.5x keep-alive
                    send_disconnect(*s, 0x8D);  // Keep Alive timeout (M7.1)
                    break;
                }
                continue;
            }

            std::byte recv_scratch[4096];
            auto got = std::visit(
                [&](auto& ch) { return ch.recv_some(recv_scratch, sizeof(recv_scratch)); }, s->channel);
            if (!got) {
                // A spurious would-block right after wait_readable said ready is a legitimate TOCTOU —
                // just poll again, exactly like read_n's own would-block handling did before this change.
                if (got.error() != quark::pal::would_block()) break;  // real error — same exit as before
                continue;
            }
            if (*got == 0) break;  // peer closed — same as read_packet()'s Ok(0)/EOF contract
            s->read_buf.insert(s->read_buf.end(), recv_scratch, recv_scratch + *got);

            bool stop_session = false;
            while (true) {
                // Re-checked every packet drained from the buffer, not just once per outer iteration —
                // see this function's own banner: a buffered read can hand several packets to this inner
                // loop at once, so checking `kicked` only at the top of the outer loop would honor a
                // session takeover less promptly than the pre-Phase-3 one-packet-per-iteration contract.
                if (s->kicked.load(std::memory_order_acquire)) {
                    send_disconnect(*s, 0x8E);
                    stop_session = true;
                    break;
                }
                auto pkt = aero::transport::mqtt::try_parse_packet(s->read_buf, s->read_pos);
                if (!pkt) {
                    // Incomplete: not an error, just nothing more to dispatch until more bytes arrive —
                    // fall through to compact the buffer and let the outer loop poll again. Malformed
                    // (a remaining-length varint past MQTT's 4-byte cap): fatal framing error, same
                    // "close the connection" exit read_packet() returning nullopt used to trigger.
                    if (pkt.error() == aero::transport::mqtt::ParseStatus::Malformed) stop_session = true;
                    break;
                }
                s->last_activity = std::chrono::steady_clock::now();  // ANY inbound packet refreshes it
                const std::uint8_t type = pkt->type_flags & 0xF0;
                if (type == 0x10) {          // CONNECT
                    if (!handle_connect(s, *pkt)) { stop_session = true; break; }
                } else if (type == 0x80) {   // SUBSCRIBE
                    if (!handle_subscribe(s, *pkt)) { stop_session = true; break; }
                } else if (type == 0x30) {   // PUBLISH
                    if (!handle_publish(*s, *pkt)) { stop_session = true; break; }
                } else if (type == 0x60) {   // PUBREL (4.3.3 step 3; low nibble MUST be 0x2, not checked —
                                              // mirrors this file's existing tolerance of e.g. SUBSCRIBE's flags)
                    if (!handle_pubrel(*s, *pkt)) { stop_session = true; break; }
                } else if (type == 0xC0) {   // PINGREQ
                    if (!send_packet(*s, std::byte{0xD0}, {})) { stop_session = true; break; }  // PINGRESP
                } else if (type == 0xE0) {   // DISCONNECT (3.1.1 §3.14): graceful — no Will on a clean
                                              // disconnect, so discard it BEFORE teardown runs.
                    s->has_will = false;
                    clean_disconnect = true;
                    stop_session = true;
                    break;
                }
                // 0x40 PUBACK from the peer (ack of a QoS-1 delivery we sent): no retry state kept in v1
                // (mirrors MqttClientTransport's own honest "no QoS-1 sender-side retransmit" scope) —
                // parsed and discarded, nothing to dispatch it to.
            }

            // Compact the already-dispatched prefix out of the buffer so it doesn't grow unbounded across
            // many read cycles — read_pos always sits at "everything before this has been consumed".
            if (s->read_pos > 0) {
                s->read_buf.erase(s->read_buf.begin(),
                                  s->read_buf.begin() + static_cast<std::ptrdiff_t>(s->read_pos));
                s->read_pos = 0;
            }
            if (stop_session) break;
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

        // 017 Phase 7: close_fd() used to run with NO io_mu lock — a fan-out that already snapshotted
        // this shared_ptr<Session> (topic_index_candidates() releases topic_index_mu_ before any write
        // happens) could race a concurrent teardown's close here, a classic fd-reuse hazard: the OS can
        // hand this exact fd number to a brand-new accept()'d connection before the racing writer's
        // send_some() call runs. Pre-existing (independent of this phase), but a persistent per-session
        // outbound queue (items can sit queued across multiple readiness events, not just one in-flight
        // call) widens the window significantly, so this phase closes it rather than inheriting it: hold
        // io_mu around close_fd — every writer (send_packet(), try_drain_reactor_send(), the legacy
        // hand-off pool) already takes io_mu before touching `channel`/`fd`, so this establishes a real
        // happens-before between "last possible write" and "fd closed". Reactor sessions additionally
        // discard any still-queued outbound state here — defensive: by the time teardown_session() runs
        // for a reactor session, schedule_reactor_teardown() has already del_fd()'d it, so no further
        // readiness event can invoke try_drain_reactor_send() on it, but a foreign-thread enqueue that
        // raced the teardown post (e.g. a fan-out publish that snapshotted this session moments before)
        // could still have pushed one more item onto out_queue right before this runs.
        {
            std::lock_guard<std::mutex> g(s->io_mu);
            if (s->is_reactor_session) {
                s->out_queue.clear();
                s->out_current.clear();
                s->out_sent = 0;
            }
            quark::pal::close_fd(s->fd);
        }
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
            (void)send_packet(*s, std::byte{0x20}, {std::byte{0x00}, std::byte{0x01}});
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
            // properties in it (Will Delay Interval/Payload Format/etc.) still aren't acted on in v1/M7.2;
            // malformed -> treated exactly like any other malformed CONNECT field. M7.2 PR A: Message
            // Expiry Interval (0x02) IS now captured into will_extras so a Will PUBLISH's TTL isn't
            // silently dropped while regular PUBLISHes honor it (see Session::will_extras's own comment —
            // this file documents Will/regular PUBLISH as one shared delivery path so they can't drift).
            // M7.2 PR B: Response Topic/Correlation Data/User Properties are captured the same way, for
            // the same reason — a Will's request/response fields must not silently drop while regular
            // PUBLISHes honor them.
            if (is_v5) {
                auto will_props = aero::transport::mqtt::read_properties(b, pos);
                if (!will_props) return false;
                if (will_props->message_expiry_interval)
                    will_extras.expiry_deadline = std::chrono::steady_clock::now() +
                                                  std::chrono::seconds(*will_props->message_expiry_interval);
                will_extras.response_topic = will_props->response_topic;
                will_extras.correlation_data = will_props->correlation_data;
                will_extras.user_properties = will_props->user_properties;
                // Same Topic Name Invalid rule as regular PUBLISH's Response Topic (§3.3.2.3.5). No
                // CONNACK/session exists yet at this point in CONNECT parsing to carry a reason code, so
                // this is a silent connection drop — matches the sibling `if (!will_props) return false;`
                // a few lines above.
                if (will_extras.response_topic &&
                    aero::broker::topic_name_has_wildcard(*will_extras.response_topic))
                    return false;
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
                    (void)send_packet(*s, std::byte{0x20}, body);
                } else {
                    (void)send_packet(*s, std::byte{0x20}, {std::byte{0x00}, std::byte{0x04}});
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
            //
            // 017 Phase 7: a LEGACY session notices `kicked` within ~200ms via session_loop's own
            // unconditional poll, even while otherwise idle. A reactor session has no equivalent
            // always-running poll — its handler only runs on genuine fd readiness or an armed timer — so
            // an idle, kicked reactor session would otherwise sit connected indefinitely instead of being
            // torn down promptly (a real behavior regression a Plan-agent critique of this phase's draft
            // caught before any code shipped). Fix: actively schedule its teardown here instead of
            // relying on passive discovery — safe to call from this (a different session's) thread,
            // exactly what schedule_reactor_teardown()'s weak_ptr-captured post() design is for.
            {
                std::lock_guard<std::mutex> g(client_sessions_mu_);
                const auto it = client_sessions_.find(client_id);
                if (it != client_sessions_.end() && it->second != s) {
                    it->second->kicked.store(true, std::memory_order_release);
                    if (it->second->is_reactor_session) {
                        // Legacy sessions send this from their own session_loop() the moment they notice
                        // `kicked` (within ~200ms). A reactor session has no equivalent poll to notice it
                        // passively (the promptness gap a Plan-agent critique caught), so send it here,
                        // from this (the NEW session's) thread — send_packet() is safe from any thread for
                        // a reactor recipient — then actively schedule teardown; schedule_reactor_teardown()
                        // makes a best-effort final drain attempt before closing, so this enqueued
                        // DISCONNECT gets a real chance to go out before the socket closes.
                        send_disconnect(*it->second, 0x8E);  // Session taken over (M7.1)
                        schedule_reactor_teardown(it->second, false);
                    }
                }
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
            // Topic index (post-benchmark addition, see route_publish()'s own banner): index BEFORE the
            // move below consumes restored_subs — this is the OTHER place (besides handle_subscribe) a
            // session's subs can grow, since a persistent-session reconnect restores its prior filters
            // wholesale without going through SUBSCRIBE again.
            for (const Subscription& sub : restored_subs) index_subscription(s, sub.filter);
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
        if (!send_packet(*s, std::byte{0x20}, body)) return false;  // CONNACK

        // Flush anything that arrived while this persistent session was offline — AFTER CONNACK, per
        // 3.1.1 (the client only knows to expect them once it's seen session-present=1).
        for (const QueuedMessage& m : to_flush)
            if (!publish_to(*s, m.topic, m.payload, m.qos, /*retain=*/false, m.extras)) return false;
        return true;
    }

    bool handle_subscribe(const std::shared_ptr<Session>& s, const aero::transport::mqtt::Packet& pkt) {
        const std::vector<std::byte>& b = pkt.body;
        if (b.size() < 2) return false;
        const std::uint16_t packet_id =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        std::size_t pos = 2;

        // M7 v5: SUBSCRIBE Properties (MQTT 5 §3.8.2.1) sit here, before the per-filter payload loop.
        // Discarded — Subscription Identifier/User Property aren't acted on in v1 — but must still be
        // walked past correctly or the filter loop below would misparse the first filter's length as
        // properties bytes. Malformed -> treated like any other malformed SUBSCRIBE (return false).
        if (s->protocol_version == 5) {
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
                !cfg_.authorizer || cfg_.authorizer->allow(s->principal, filter, AclAction::Subscribe);
            if (allowed) {
                added.push_back(Subscription{filter, qos});
                granted.push_back(static_cast<std::byte>(qos));
            } else {
                // 3.9.3 (v4): 0x80 SUBACK failure. MQTT 5 §3.9.3: 0x87 Not Authorized is the v5-specific
                // reason code for the same denial — v4 sessions keep 0x80 unchanged.
                granted.push_back(s->protocol_version == 5 ? std::byte{0x87} : std::byte{0x80});
            }
        }
        {
            // Copy, not move — `added` is still read below for the retained-message replay.
            std::lock_guard<std::mutex> g(s->subs_mu);
            for (const Subscription& sub : added) s->subs.push_back(sub);
        }
        // Topic index (post-benchmark addition, see route_publish()'s own banner): mirrors `added` into
        // exact_topic_index_/wildcard_subscribers_ so route_publish() never has to scan this session for a
        // topic none of its filters could possibly match. Done AFTER releasing subs_mu (index_subscription
        // takes its own topic_index_mu_ — never nest the two, matches this file's existing per-resource
        // lock discipline) and BEFORE the SUBACK is sent, so the index is never behind what the client was
        // just told is subscribed.
        for (const Subscription& sub : added) index_subscription(s, sub.filter);

        // SUBACK variable header (MQTT 5 §3.9.2): Packet Identifier, then Properties, THEN the payload's
        // reason-code list — verified against the spec rather than assumed (Properties precede the
        // reason codes, mirroring PUBACK/PUBREC's own Packet-Id-then-Properties shape elsewhere in v5).
        std::vector<std::byte> vh;
        aero::transport::mqtt::put_u16_be(vh, packet_id);
        if (s->protocol_version == 5) aero::transport::mqtt::put_empty_properties(vh);
        vh.insert(vh.end(), granted.begin(), granted.end());
        if (!send_packet(*s, std::byte{0x90}, vh)) return false;  // SUBACK

        // Retained-message replay (3.1.1 §3.8.4): a new matching SUBSCRIBE gets the retained payload
        // immediately, at the subscription's granted QoS. M7.2 PR A: msg.extras carries the retained
        // message's own Message Expiry through — publish_to()'s choke point re-checks it at replay time,
        // so a long-stale retained message is silently dropped instead of replayed as if fresh.
        std::lock_guard<std::mutex> rg(retained_mu_);
        for (const Subscription& sub : added) {
            for (const auto& [topic, msg] : retained_) {
                if (topic_matches(sub.filter, topic)) {
                    if (!publish_to(*s, topic, msg.payload, sub.qos, /*retain=*/true, msg.extras)) return false;
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
        // Message Expiry Interval (0x02) is now captured too, turned into an absolute deadline below. M7.2
        // PR B: Response Topic (0x08), Correlation Data (0x09), and User Properties (0x26) are captured
        // too, threaded onto `extras` for deliver_publish()'s on_publish_/publish_to() call sites.
        std::optional<std::uint16_t> topic_alias;
        PublishExtras extras;
        if (s.protocol_version == 5) {
            auto props = aero::transport::mqtt::read_properties(b, pos);
            if (!props) return false;
            topic_alias = props->topic_alias;
            if (props->message_expiry_interval)
                extras.expiry_deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(*props->message_expiry_interval);
            extras.response_topic = props->response_topic;
            extras.correlation_data = props->correlation_data;
            extras.user_properties = props->user_properties;
            // Response Topic must be a valid Topic Name — no wildcards (MQTT 5 §3.3.2.3.5). This is a
            // semantic/protocol-error check, not a decode-shape one, so it belongs here rather than in
            // mqtt_codec.hpp — mirrors how Topic Alias's 0-or-too-large check already lives in
            // handle_publish(), not in the codec. 0x90 is the spec's dedicated Topic Name Invalid reason
            // code (also used by CONNACK/PUBACK/PUBREC/SUBACK for the same condition).
            if (extras.response_topic && aero::broker::topic_name_has_wildcard(*extras.response_topic)) {
                send_disconnect(s, 0x90);  // Topic Name Invalid
                return false;
            }
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
            return send_packet(s, std::byte{0x50}, ack);  // PUBREC
        }

        // QoS 0/1: act BEFORE acking — a publisher that has received its PUBACK must be able to assume a
        // subsequent SUBSCRIBE/retained-replay from anyone will already see this value/delivery (no
        // ack-before-visible race).
        if (allowed) deliver_publish(resolved_topic, payload, qos, retain, extras);
        if (qos == 1) {
            if (!allowed) return true;  // silent drop: no PUBACK
            std::vector<std::byte> ack;
            aero::transport::mqtt::put_u16_be(ack, packet_id);
            if (!send_packet(s, std::byte{0x40}, ack)) return false;  // PUBACK
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
        return send_packet(s, std::byte{0x70}, ack);  // PUBCOMP
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
        if (on_publish_) on_publish_(topic, payload, qos, to_publish_properties(extras));
        route_publish(topic, payload, qos, extras);
        // M6 (017 §4): AFTER local delivery, so a peer's relayed copy can never arrive before this node's
        // own subscribers see it. No-op single-node default (peer_forwarder_ unset) — see set_peer_forwarder().
        if (peer_forwarder_) peer_forwarder_(topic, payload, qos);
    }

    // Fan out to every connected session with a matching subscription, at min(publish qos, granted
    // qos). Also queues into any OFFLINE persistent (clean_session=0) session whose stored subscriptions
    // match — QoS ≥1 only, since there is nothing to durably queue a QoS-0 "at most once" message as.
    // M7.2 PR A: `extras` (Message Expiry, primarily) is threaded through to both the live fan-out
    // (publish_to's own choke point re-checks it at actual send time) and the offline queue
    // (QueuedMessage::extras, re-checked when flushed on reconnect).
    //
    // Post-benchmark addition (see bench/broker/broker_bench.cpp): this used to snapshot ALL of
    // sessions_ and scan every one of them for a match, live-fanout throughput measured at ~500-1200ns
    // PER SESSION regardless of whether it matched — at a few thousand connected-but-unrelated sessions
    // (e.g. many devices each on their own topic) that scan alone became the dominant per-publish cost.
    // topic_index_candidates() below narrows the snapshot to sessions that could ACTUALLY match `topic` —
    // see its own banner (next to topic_index_mu_) for why that's provably safe (no missed matches).
    // Every candidate is still scanned exactly as before (full subs_mu-locked walk, topic_matches() per
    // filter), so delivery semantics — including per-session duplicate delivery for overlapping
    // subscriptions — are byte-for-byte unchanged; only which sessions get scanned at all has changed.
    //
    // 017 Phase 7b: bounds how many recipients a single reactor-thread call processes inline before
    // yielding the reactor loop to other ready fds via a chained reactor_io_.post() continuation — mirrors
    // quark::net::voice_channel.hpp's own cited rule ("bounded, chained IoContext::post() continuation —
    // never inline", proven there by ADR-030's negative control). A real measurement this round (a
    // throwaway scratch experiment, not part of the shipped suite) showed an UNBOUNDED inline fan-out loop
    // causes a genuine latency spike for an unrelated reactor session sharing the thread once fan-out size
    // reaches the thousands (max ping latency ~16ms baseline -> ~60-80ms at 3,000-8,000 recipients) — each
    // item's own work (a mutex lock + one non-blocking syscall attempt) is cheap, but thousands of them in
    // one uninterrupted call add up to tens of milliseconds the reactor loop can't service anyone else
    // during. 256 keeps a worst-case inline batch in the same tens-of-microseconds range this round's
    // measurement put a single item at, times a couple hundred. Only applies when the CALLING thread is
    // the reactor thread — a legacy thread's own inline loop only ever blocks its own thread, unaffected.
    static constexpr std::size_t kMaxFanoutInlinePerCall = 256;

    // 017 Phase 7: the fan-out dispatch is a 3-way split by (recipient kind, calling thread) — the 4th
    // cell of the design doc's dispatch table ("any non-reactor-loop thread" -> legacy recipient) IS this
    // function's original unchanged inline publish_to() call for that case. The 3 kinds:
    //   - reactor-managed recipient (any calling thread): enqueue_reactor_publish() — never blocks.
    //   - legacy recipient, caller IS the reactor thread: MUST NOT call blocking publish_to() inline (would
    //     freeze every other reactor session) — hands off to the bounded, persistent worker pool instead
    //     (enqueue_legacy_handoff(), Critical fix #2 — bounded queue + send deadline, unlike Phase 6's
    //     unbounded item-count-only cap).
    //   - legacy recipient, caller is any other thread (a legacy session's own thread, or the
    //     BrokerRelayActor cluster-relay worker thread — both grouped here since neither is the reactor
    //     loop thread): today's unchanged inline blocking publish_to() call, exactly as before this phase.
    //
    // When the calling thread IS the reactor thread, the delivery list is built once (same
    // topic_index_candidates() + subs_mu scan as before — every candidate still scanned exactly as
    // before, so delivery semantics are unchanged) and then handed to route_publish_reactor_batch() below
    // for the bounded/chained dispatch (7b). A non-reactor-thread caller's own inline loop is left
    // completely unbounded/unchained, exactly as in 7a — it only ever blocks its own dedicated thread.
    void route_publish(const std::string& topic, const std::vector<std::byte>& payload, std::uint8_t qos,
                       const PublishExtras& extras = {}) {
        const bool caller_on_reactor_thread = is_on_reactor_thread();
        if (caller_on_reactor_thread) {
            std::vector<std::pair<std::shared_ptr<Session>, std::uint8_t>> deliveries;
            for (const auto& session : topic_index_candidates(topic)) {
                std::lock_guard<std::mutex> g(session->subs_mu);
                for (const Subscription& sub : session->subs)
                    if (topic_matches(sub.filter, topic))
                        deliveries.emplace_back(session, qos < sub.qos ? qos : sub.qos);
            }
            route_publish_reactor_batch(topic, payload, extras, std::move(deliveries), /*offset=*/0);
        } else {
            for (const auto& session : topic_index_candidates(topic)) {
                std::vector<Subscription> matches;
                {
                    std::lock_guard<std::mutex> g(session->subs_mu);
                    for (const Subscription& sub : session->subs)
                        if (topic_matches(sub.filter, topic)) matches.push_back(sub);
                }
                for (const Subscription& sub : matches) {
                    const std::uint8_t deliver_qos = qos < sub.qos ? qos : sub.qos;
                    if (session->is_reactor_session) {
                        enqueue_reactor_publish(session, topic, payload, deliver_qos, /*retain=*/false,
                                                extras);
                    } else {
                        (void)publish_to(*session, topic, payload, deliver_qos, /*retain=*/false, extras);
                    }
                }
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

    // 017 Phase 7b: processes up to kMaxFanoutInlinePerCall deliveries starting at `offset`, then — if
    // more remain — chains via reactor_io_.post() rather than continuing the loop inline, so a large
    // fan-out interleaves with other reactor sessions' readiness events instead of hogging the loop for
    // its entire duration. Always runs on the reactor thread (the only caller, route_publish() above,
    // only reaches here when already on it; each chained continuation is itself a posted task, which also
    // always runs on the reactor thread) — so `enqueue_reactor_publish()`'s and `enqueue_legacy_handoff()`'s
    // own thread-safety doesn't even need marshaling here specifically, though both remain safe to call
    // from any thread regardless (unchanged from 7a). `deliveries` is captured by value / moved through
    // the chain (shared_ptr<Session> entries — the same "snapshot, hold for the fan-out's duration" idiom
    // topic_index_candidates() itself already uses, just spanning possibly more than one reactor loop
    // iteration here); a session torn down mid-chain is safe, not a race — enqueue_reactor_publish() and
    // the legacy hand-off path both tolerate a closed fd/already-torn-down session as an ordinary send
    // error, never a crash (see try_drain_reactor_send()'s hard-error handling).
    void route_publish_reactor_batch(const std::string& topic, const std::vector<std::byte>& payload,
                                     const PublishExtras& extras,
                                     std::vector<std::pair<std::shared_ptr<Session>, std::uint8_t>> deliveries,
                                     std::size_t offset) {
        const std::size_t end = std::min(offset + kMaxFanoutInlinePerCall, deliveries.size());
        for (std::size_t i = offset; i < end; ++i) {
            const auto& [session, deliver_qos] = deliveries[i];
            if (session->is_reactor_session) {
                enqueue_reactor_publish(session, topic, payload, deliver_qos, /*retain=*/false, extras);
            } else {
                enqueue_legacy_handoff(session, topic, payload, deliver_qos, /*retain=*/false, extras);
            }
        }
        if (end >= deliveries.size()) return;  // done
        reactor_io_.post([this, topic, payload, extras, deliveries = std::move(deliveries), end]() mutable {
            route_publish_reactor_batch(topic, payload, extras, std::move(deliveries), end);
        });
    }

    // Best-effort server->client DISCONNECT (MQTT 5 §3.14) — v5 only; a v4 session has no such packet, so
    // this is a silent no-op for it (every call site's socket-close behavior is unchanged for v4).
    void send_disconnect(Session& s, std::uint8_t reason_code) {
        if (s.protocol_version != 5) return;
        std::vector<std::byte> body{static_cast<std::byte>(reason_code)};
        aero::transport::mqtt::put_empty_properties(body);
        (void)send_packet(s, std::byte{0xE0}, body);
    }

    // 017 Phase 7 (Critical fix #1): the single choke point EVERY outbound write on a Session goes
    // through — CONNACK/SUBACK/PINGRESP/PUBACK/PUBREC/PUBCOMP/DISCONNECT directly (every call site above
    // and below this file mechanically renamed from `s->send_packet(...)`/`s.send_packet(...)` to
    // `send_packet(*s, ...)`/`send_packet(s, ...)` — no business logic touched), and PUBLISH indirectly
    // via publish_to() below. A Plan-agent critique of this phase's draft caught that routing ONLY
    // route_publish()'s fan-out through the new non-blocking path (and leaving this one alone) would
    // still let a single unresponsive reactor client freeze the entire reactor thread the moment it sent
    // anything needing an immediate reply (a bare PINGREQ is enough) — exactly Phase 6's disproven "looks
    // non-blocking but isn't, once you trace every call path" failure shape, just through a different
    // door. Legacy (TLS) sessions keep today's unconditional blocking write_packet() call, byte-for-byte
    // unchanged, still reporting real synchronous success/failure. Reactor sessions NEVER block here,
    // regardless of which thread calls this — the write is queued (see QueuedOutboundRaw), not
    // necessarily sent yet, so this always reports success; a hard I/O error discovered later during the
    // actual non-blocking send tears the session down asynchronously via schedule_reactor_teardown()
    // instead of through this function's return value — the reactor has no per-session read loop to
    // `break` out of the way session_loop() does, so there is nothing for a synchronous failure here to
    // usefully signal for a reactor session.
    bool send_packet(Session& s, std::byte type_flags, const std::vector<std::byte>& body) {
        if (!s.is_reactor_session) {
            std::lock_guard<std::mutex> g(s.io_mu);
            return std::visit(
                [&](auto& ch) { return aero::transport::mqtt::write_packet(ch, type_flags, body); },
                s.channel);
        }
        std::shared_ptr<Session> sp = s.shared_from_this();
        std::lock_guard<std::mutex> g(s.io_mu);
        s.out_queue.push_back(QueuedOutbound{QueuedOutboundRaw{type_flags, body}, std::nullopt});
        try_drain_reactor_send(sp);
        return true;
    }

    // 017 Phase 5/Phase 7: builds [PUBLISH variable header + v5 Properties + payload] into `out` (does
    // NOT include the fixed header byte or remaining-length prefix — callers add framing separately via
    // aero::transport::mqtt::serialize_packet()/write_packet()/write_packet_bounded()). Extracted
    // (Phase 7) from publish_to() below so the reactor outbound-queue drain path (which must build this
    // into a per-Session buffer that outlives one call, not publish_to()'s thread_local scratch) and the
    // reactor->legacy hand-off pool (Critical fix #2) can reuse the exact same choke-point + serialization
    // logic instead of re-deriving it — publish_to() itself is UNCHANGED in behavior, only its body is now
    // split between this function and the framing/send it still does directly (thread_local `vh` reuse,
    // Phase 5's shipped pooling win, is fully preserved since publish_to() still owns and passes its own
    // thread_local buffer as `out`). Returns false if a choke point (Message Expiry / Maximum Packet Size)
    // says this message must not be sent — callers must treat that as "skip, not an error", the same
    // silent-drop idiom this file has always used (mirrors the ACL-denial idiom: one message not going out
    // must never tear a session down).
    [[nodiscard]] bool build_publish_variable_header_and_payload(
            Session& s, const std::string& topic, const std::vector<std::byte>& payload, std::uint8_t qos,
            const PublishExtras& extras, std::vector<std::byte>& out) {
        // Choke point 1 — Message Expiry: check first, before building anything, so an already-stale
        // message (offline queue flush, retained replay, plain fanout, or a backed-up reactor recipient's
        // queued item) never gets serialized.
        if (extras.expiry_deadline && std::chrono::steady_clock::now() >= *extras.expiry_deadline) {
            return false;
        }

        out.clear();
        aero::transport::mqtt::put_str(out, topic);
        if (qos > 0) aero::transport::mqtt::put_u16_be(out, next_packet_id());

        if (s.protocol_version == 5) {
            aero::transport::mqtt::PropertyWriter pw;
            if (extras.expiry_deadline) {
                const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                    *extras.expiry_deadline - std::chrono::steady_clock::now())
                                          .count();
                // remaining > 0 here — choke point 1 above already returned on <= 0.
                pw.put_u32(0x02, static_cast<std::uint32_t>(remaining));
            }
            // 017 M7.2 PR B: Response Topic / Correlation Data / User Properties, each independently
            // optional — User Property is repeatable (§3.3.2.3.7), so every entry in the vector gets its
            // own record, duplicates and all.
            if (extras.response_topic) pw.put_str(0x08, *extras.response_topic);
            if (extras.correlation_data) pw.put_binary(0x09, *extras.correlation_data);
            for (const auto& [k, v] : extras.user_properties) pw.put_str_pair(0x26, k, v);
            std::vector<std::byte> props;
            pw.write(props);  // valid Properties block even when pw is empty (one 0x00 byte) — the §3.3.1 fix
            out.insert(out.end(), props.begin(), props.end());
        }
        out.insert(out.end(), payload.begin(), payload.end());

        // Choke point 2 — Maximum Packet Size: after Properties are written (so the check covers the real
        // wire size, not an underestimate), before send.
        if (s.max_packet_size) {
            std::vector<std::byte> len_scratch;
            aero::transport::mqtt::put_remaining_length(len_scratch, static_cast<std::uint32_t>(out.size()));
            const std::size_t total = 1 + len_scratch.size() + out.size();  // fixed-header + rem-len + out
            if (total > *s.max_packet_size) return false;  // silent drop, same idiom as choke point 1
        }
        return true;
    }

    // 017 M7.2 PR A: this function used to write ZERO Properties bytes for outbound PUBLISH, for EVERY
    // protocol version — not even an empty MQTT 5 Properties field, which is actually a latent correctness
    // bug (§3.3.1 requires one, mandatory-even-empty, for v5; see this file's M7/M7.1 banners' own honest
    // "outbound PUBLISH framing is untouched" scope notes, now superseded). This rewrite fixes that AS A
    // DOCUMENTED BYPRODUCT while adding the two PR A choke points below. v4 sessions never enter the
    // `protocol_version == 5` branch — wire shape is byte-for-byte unchanged for them, a hard invariant.
    bool publish_to(Session& s, const std::string& topic, const std::vector<std::byte>& payload,
                    std::uint8_t qos, bool retain, const PublishExtras& extras = {}) {
        // 017 Phase 5 (redesign doc §4.1/plan): thread_local instead of a fresh vector on every one of
        // this function's 3 call sites (route_publish's fan-out loop, offline-queue flush, retained
        // replay) - safe because write_packet() (mqtt_codec.hpp) fully copies its `body` parameter (vh)
        // into its own owned buffer BEFORE the potentially-blocking send/retry loop starts, so a stalled
        // recipient socket can never extend vh's "live" window past this function's return.
        static thread_local std::vector<std::byte> vh;
        if (!build_publish_variable_header_and_payload(s, topic, payload, qos, extras, vh)) return true;

        std::uint8_t flags = static_cast<std::uint8_t>(qos << 1);
        if (retain) flags |= 0x01;
        return send_packet(s, static_cast<std::byte>(0x30 | flags), vh);
    }

    std::uint16_t next_packet_id() {
        const std::uint16_t id = packet_id_.fetch_add(1, std::memory_order_relaxed);
        return id == 0 ? packet_id_.fetch_add(1, std::memory_order_relaxed) : id;  // MQTT packet id != 0
    }

    void remove_session(const std::shared_ptr<Session>& s) {
        {
            std::lock_guard<std::mutex> g(sessions_mu_);
            std::erase(sessions_, s);
        }

        // Undo every index_subscription(s, ...) call this session's subs ever triggered. Re-derived from
        // s->subs itself rather than tracked separately: this broker has no UNSUBSCRIBE (see
        // handle_subscribe's own scope), so a session's subs only ever grow (SUBSCRIBE, or a persistent-
        // session reconnect's wholesale restore) until the whole session tears down right here — so
        // s->subs is always exactly "every filter this session was ever indexed under".
        std::vector<Subscription> subs_copy;
        {
            std::lock_guard<std::mutex> g(s->subs_mu);
            subs_copy = s->subs;
        }
        if (subs_copy.empty()) return;

        bool had_wildcard = false;
        std::lock_guard<std::mutex> g(topic_index_mu_);
        for (const Subscription& sub : subs_copy) {
            if (filter_has_wildcard(sub.filter)) {
                had_wildcard = true;
                continue;
            }
            auto it = exact_topic_index_.find(sub.filter);
            if (it == exact_topic_index_.end()) continue;
            std::erase(it->second, s);
            if (it->second.empty()) exact_topic_index_.erase(it);
        }
        if (had_wildcard) std::erase(wildcard_subscribers_, s);
    }

    // Topic index (post-benchmark addition — see route_publish()'s own banner for why this exists).
    //
    // A subscription filter either contains no `+`/`#` (in which case topic_matches() degenerates to
    // plain string equality — see topic_match.hpp) or it does. That split gives two lookup paths instead
    // of one linear scan:
    //   - exact_topic_index_[topic] -> every session with a NON-wildcard filter == topic exactly.
    //   - wildcard_subscribers_ -> every session with AT LEAST ONE '+'/'#'-containing filter — still
    //     scanned linearly (MQTT wildcard matching can't be reduced to a hash lookup without a full topic
    //     trie, deliberately out of scope here; this list is typically far smaller than the total live-
    //     session count in a real deployment, mirroring acl.hpp's own "cold path, low cardinality"
    //     reasoning for wildcard-shaped rules).
    // A session with zero subscriptions (freshly accepted, not yet SUBSCRIBEd) is in neither bucket.
    //
    // Invariant this relies on: index entries can only be ADDED (index_subscription(), called from
    // handle_subscribe/handle_connect's session-restore path) and are only ever removed ALL AT ONCE, when
    // the owning session tears down (remove_session() above) — because this broker has no UNSUBSCRIBE
    // (handle_subscribe's own scope note). So an index entry for (filter, session) existing always implies
    // session->subs still contains that exact filter; the index can never point at a session whose actual
    // subs no longer back the entry.
    std::mutex topic_index_mu_;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Session>>> exact_topic_index_;
    std::vector<std::shared_ptr<Session>> wildcard_subscribers_;

    [[nodiscard]] static bool filter_has_wildcard(std::string_view filter) noexcept {
        return filter.find('+') != std::string_view::npos || filter.find('#') != std::string_view::npos;
    }

    // Called once per newly-added Subscription (handle_subscribe for a fresh SUBSCRIBE, handle_connect for
    // a persistent-session reconnect's restored subs) — never from route_publish()'s hot path itself, so
    // index maintenance cost is paid at subscribe time, not publish time.
    void index_subscription(const std::shared_ptr<Session>& s, const std::string& filter) {
        std::lock_guard<std::mutex> g(topic_index_mu_);
        if (filter_has_wildcard(filter))
            wildcard_subscribers_.push_back(s);
        else
            exact_topic_index_[filter].push_back(s);
    }

    // The deduplicated candidate session set for `topic` — see this group's banner above for why every
    // session NOT in this set provably has no subscription that could match. Snapshot-then-unlock (same
    // pattern as the old sessions_mu_ snapshot this replaced): topic_index_mu_ is held only long enough to
    // copy shared_ptrs, never while route_publish()'s callers do the actual (possibly blocking) socket
    // writes.
    // 017 Phase 5 (redesign doc §4.1/plan): thread_local instead of a fresh vector every publish — safe
    // because NativeBroker is thread-per-connection (session reader threads) plus the cluster relay path
    // (BrokerRelayActor, a quark::Sequential actor - confirmed single-in-flight, never reentrant on its own
    // worker thread), so no thread can call back into this function while a previous call's `out` on that
    // SAME thread is still being iterated. `out.clear()` MUST run unconditionally on every call, not just
    // when exact_topic_index_ has a match - a fresh local was always implicitly "cleared" by construction,
    // but a persistent thread_local isn't, and skipping this would leak a previous call's sessions into a
    // topic that has none today.
    [[nodiscard]] const std::vector<std::shared_ptr<Session>>& topic_index_candidates(const std::string& topic) {
        static thread_local std::vector<std::shared_ptr<Session>> out;
        out.clear();
        {
            std::lock_guard<std::mutex> g(topic_index_mu_);
            if (auto it = exact_topic_index_.find(topic); it != exact_topic_index_.end())
                out.insert(out.end(), it->second.begin(), it->second.end());
            out.insert(out.end(), wildcard_subscribers_.begin(), wildcard_subscribers_.end());
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
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
    std::vector<std::thread> session_threads_;  // legacy (TLS) sessions only — reactor sessions never
                                                 // get an entry here (see accept_loop()'s Phase 7 banner)

    // 017 Phase 7: the shared reactor (single shard, per the design doc's §2.2 resolution — sharding
    // stays a future, separately-validated option) plaintext sessions register with. `reactor_thread_id_`
    // is read via std::thread::get_id() on the constructing thread immediately after reactor_thread_'s
    // construction (available without waiting for the thread body to start) — is_on_reactor_thread()
    // compares against it to decide whether an IoContext loop-thread-only call (add_fd/mod_fd/del_fd/
    // post_after) can be made directly or must be marshaled via reactor_io_.post() instead.
    quark::pal::IoContext reactor_io_;
    std::thread reactor_thread_;
    std::thread::id reactor_thread_id_;

    // 017 Phase 7 (Critical fix #2): the bounded, persistent worker pool for "a reactor-originated
    // PUBLISH fanning out to a legacy (TLS) recipient" — reuses BrokerCluster's own "dedicated persistent
    // worker, not ad-hoc pooled dispatch" precedent (broker_cluster.hpp's 1-worker Quark engine for
    // BrokerRelayActor), implemented here as a plain fixed-size std::thread pool rather than pulling in
    // Quark's full actor/message-pool machinery, which solves a different problem (cross-actor routing)
    // than "drain a bounded queue with N persistent workers" needs. Bounded queue depth (drop-oldest, the
    // same idiom as kQueuedMessageCap's offline-message cap) + a per-send deadline
    // (write_packet_bounded(), mqtt_codec.hpp) together ensure one persistently slow legacy recipient can
    // occupy a worker for at most kLegacyHandoffSendDeadline, never forever — the exact bound Phase 6's
    // item-count-only cap failed to provide, applied here to the one edge of the mixed-mode dispatch
    // table that still uses a worker pool rather than genuinely non-blocking per-item sends.
    struct LegacyHandoffItem {
        std::shared_ptr<Session> target;
        std::string topic;
        std::vector<std::byte> payload;
        std::uint8_t qos = 0;
        bool retain = false;
        PublishExtras extras;
    };
    static constexpr std::size_t kLegacyHandoffQueueCap = 100;
    static constexpr std::chrono::seconds kLegacyHandoffSendDeadline{5};
    static constexpr int kLegacyHandoffWorkerCount = 2;

    std::mutex handoff_mu_;
    std::condition_variable handoff_cv_;
    std::deque<LegacyHandoffItem> handoff_queue_;
    bool handoff_stop_ = false;
    std::vector<std::thread> handoff_workers_;

    void enqueue_legacy_handoff(const std::shared_ptr<Session>& target, const std::string& topic,
                                const std::vector<std::byte>& payload, std::uint8_t qos, bool retain,
                                const PublishExtras& extras) {
        std::lock_guard<std::mutex> g(handoff_mu_);
        if (handoff_queue_.size() >= kLegacyHandoffQueueCap) handoff_queue_.pop_front();  // drop-oldest
        handoff_queue_.push_back(LegacyHandoffItem{target, topic, payload, qos, retain, extras});
        handoff_cv_.notify_one();
    }

    void handoff_worker_loop() {
        for (;;) {
            LegacyHandoffItem item;
            {
                std::unique_lock<std::mutex> lk(handoff_mu_);
                handoff_cv_.wait(lk, [this] { return handoff_stop_ || !handoff_queue_.empty(); });
                if (handoff_stop_ && handoff_queue_.empty()) return;
                item = std::move(handoff_queue_.front());
                handoff_queue_.pop_front();
            }
            std::vector<std::byte> body;
            if (!build_publish_variable_header_and_payload(*item.target, item.topic, item.payload,
                                                            item.qos, item.extras, body))
                continue;  // choke point said skip
            std::uint8_t flags = static_cast<std::uint8_t>(item.qos << 1);
            if (item.retain) flags |= 0x01;
            const auto deadline = std::chrono::steady_clock::now() + kLegacyHandoffSendDeadline;
            std::lock_guard<std::mutex> g(item.target->io_mu);
            // A false return (deadline exceeded or a hard error) is treated the same as every other
            // best-effort fan-out delivery in this file — route_publish() never reports per-recipient
            // failures back to the publisher. The recipient's own reader thread notices a genuinely dead
            // connection on its next read attempt and tears itself down through the normal path.
            (void)std::visit(
                [&](auto& ch) {
                    return aero::transport::mqtt::write_packet_bounded(
                        ch, static_cast<std::byte>(0x30 | flags), body, deadline);
                },
                item.target->channel);
        }
    }

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
    std::function<void(std::string_view, std::span<const std::byte>, std::uint8_t,
                       const PublishProperties&)> on_publish_;

    // M6 (017 §4): unset by default (single-node — 100% of Phase 1 through M3 behavior unchanged). Set by
    // BrokerCluster via set_peer_forwarder() to broadcast every locally-originated PUBLISH to peer nodes.
    std::function<void(std::string_view, std::span<const std::byte>, std::uint8_t)> peer_forwarder_;
};

}  // namespace aero::broker
