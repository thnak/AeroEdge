// AeroEdge Broker — `BrokerCluster`, cross-node PUBLISH broadcast for `NativeBroker` (017 §4, M6).
//
// WHAT M6 CLOSES: `NativeBroker` (native_broker.hpp) is Phase-1-through-M3 single-node — a PUBLISH only
// ever reaches sessions connected to the SAME broker instance. Spec 017 §4 says cross-node routing should
// be "just" `quark::DistributedRouter` — proven working, in-process, by tests/transport/cross_node_message.cpp.
// But that test rides `quark::InProcessMembership` + `quark::LoopbackTransport`, both explicitly documented
// (cluster.hpp's banner) as IN-PROCESS TEST DOUBLES — there is no real (cross-process) membership or wiring
// for it anywhere in this codebase yet. This file is that wiring, deliberately scoped down:
//
//   * MEMBERSHIP — `StaticBrokerMembership` below: a FIXED, config-provided NodeId roster for the process's
//     lifetime. No gossip, no failure detection, no dynamic join/leave — mirrors `quark::InProcessMembership`'s
//     internal snapshot-publish shape (membership.hpp), just without the join()/leave() mutators. A real SWIM-
//     backed membership (021) is a distinct, later seam this can be swapped behind without touching NativeBroker.
//   * TRANSPORT — `aero::transport::TcpTransport` (tcp_transport.hpp), the real cross-platform TCP wire
//     already proven in tests/transport/tcp_transport.cpp. ONE MULTIPLEXED CONNECTION PER PEER on a port
//     SEPARATE from the MQTT client-facing port (`cluster_port` below) — devices dial `NativeBroker::Config`'s
//     port; peer brokers dial each other on this one.
//   * FANOUT MODEL — BROADCAST, not selective/HRW-topic-owner routing (017 §4's original text). MQTT
//     wildcard subscriptions mean "which node owns topic T" does not tell you which nodes have a subscriber
//     whose FILTER matches T — answering that without a distributed subscription registry needs exactly the
//     expensive machinery 017 exists to avoid rebuilding (017 §1). So: every LOCALLY-received PUBLISH is
//     broadcast to EVERY peer, and each peer runs its own ordinary local wildcard match against its own
//     sessions. Less efficient at scale than selective forwarding, correct and simple for v1 — the natural
//     slot a future optimization (past M6) would fill.
//   * LOOP PREVENTION — mandatory: a relay-delivered PUBLISH must reach this node's local subscribers but
//     must NEVER be broadcast onward (that would forward-loop across the whole cluster forever). This is why
//     `NativeBroker` grew TWO distinct entry points (native_broker.hpp's M6 banner): `deliver_publish()`
//     (locally-originated — matches locally AND calls the peer_forwarder_ hook) and the new public
//     `deliver_remote_publish()` (relay-delivered — matches locally ONLY, no forwarder call, ever).
//   * RETAINED MESSAGES — deliberately NOT synchronized cross-node in M6: `deliver_remote_publish()`'s
//     signature (topic/payload/qos, no retain flag) mirrors this file's own relay message, so a peer's
//     retained_ store only ever reflects PUBLISHes ITS OWN directly-connected clients sent with retain=1.
//     A device that connects to node B won't see a retained value a device set on node A. Known v1 gap,
//     consistent with 017's existing "cross-node session continuity" Open Question — not addressed here.
//
// M5.1 (017 §10) adds OPT-IN mTLS over the inter-broker link itself: `BrokerClusterSecurityConfig`
// (broker_cluster_security.hpp, default-disabled) wraps `transport_` in QuarkCpp's real ADR-040
// `SecureTransport` — see `start()`'s own banner and `prime_handshake()` for the wiring; every plain-
// TcpTransport call site above is unaffected when security isn't configured.
//
// SURPRISE WORTH FLAGGING (017 §4 undersells this): `quark::DistributedRouter::tell()` is PLACEMENT-routed
// — `place(ActorId, membership.view())` picks ONE owner node for a given (ActorId, membership) pair, and
// EVERY caller across the whole cluster computes the SAME owner. There is no `tell`-shaped way to address
// an explicit, caller-chosen peer — a fixed `BrokerRelayActor` key does not turn `dist_router.get<A>(key).tell(m)`
// into a per-peer send; every node's `tell()` to that key would land on the SAME single HRW-chosen node,
// not "this call reaches peer P". `broadcast()` below works around this by using `DistributedRouter` only
// for the RECEIVE side (`register_remote` + its constructor's automatic `transport.on_receive(deliver)`
// wiring) and hand-building one `MessageFrame` per peer for the SEND side, handed directly to
// `TcpTransport::send(NodeId, MessageFrame)` — bypassing placement entirely, which is exactly what an
// explicit "send to every peer, individually" broadcast needs.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aero/broker/native_broker.hpp"
#include "aero/broker/broker_cluster_security.hpp"
#include "aero/transport/tcp_transport.hpp"

#include "quark/core/actor.hpp"      // Actor<>, Protocol<> (CRTP actor base)
#include "quark/core/actor_ref.hpp"  // LocalRouter
#include "quark/core/activation.hpp" // Activation, detail::MessagePool
#include "quark/core/distribution.hpp"  // DistributedRouter
#include "quark/core/engine.hpp"        // Engine<>
#include "quark/core/engine_config.hpp" // EngineConfig
#include "quark/core/ids.hpp"           // NodeId
#include "quark/core/membership.hpp"    // Membership, MembershipView
#include "quark/core/metadata.hpp"      // actor_id_of<A>, type_key_of<M>
#include "quark/core/spawn.hpp"         // register_actor<A>
#include "quark/core/transport.hpp"     // MessageFrame, WireMode
#include "quark/core/wire.hpp"          // encode_tagless, tagless_size

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
#include <array>
#include <random>

#include "aero/pal/tls.hpp"  // ensure_threading_registered() — see BrokerCluster::start()'s banner
#include "quark/adapters/mbedtls/mbedtls_aead.hpp"
#include "quark/adapters/mbedtls/mbedtls_handshake.hpp"
#include "quark/core/secure_transport.hpp"
#endif

namespace aero::broker {

// A peer node's identity + its inter-broker dial address (host:port for `cluster_port` below — NOT the
// MQTT client-facing port). One per OTHER node in the static cluster snapshot.
struct PeerSpec {
    quark::NodeId id{};
    std::string host;
    std::uint16_t port = 0;
};

// The optional second half of `Runtime::configure_broker()` (runtime.hpp, 017 M6): if `peers` is empty
// (the default), no BrokerCluster is ever constructed — single-node behavior, unchanged, zero cost.
struct BrokerClusterConfig {
    quark::NodeId self{};
    std::uint16_t cluster_port = 0;  // this node's OWN inter-broker TCP listener (0 == ephemeral)
    std::vector<PeerSpec> peers;
};

// A static, config-fixed membership snapshot for the process's lifetime (017 M6 brief: no gossip, no
// failure detection, no dynamic join/leave — a real SWIM-backed `Membership` is a distinct, later seam).
// Mirrors `quark::InProcessMembership`'s internal shape (membership.hpp: a sorted, de-duplicated,
// shared_ptr-pinned node vector) minus the mutators — the roster this constructs is the roster for good.
class StaticBrokerMembership final : public quark::Membership {
public:
    StaticBrokerMembership(quark::NodeId self, std::vector<quark::NodeId> nodes) : self_(self) {
        std::sort(nodes.begin(), nodes.end(), [](quark::NodeId a, quark::NodeId b) { return a.value < b.value; });
        nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
        snapshot_ = std::make_shared<const std::vector<quark::NodeId>>(std::move(nodes));
    }

    [[nodiscard]] quark::NodeId self() const noexcept override { return self_; }
    [[nodiscard]] quark::MembershipView view() const noexcept override {
        return quark::MembershipView{snapshot_, /*epoch=*/1};  // never republished — the roster never changes
    }

private:
    quark::NodeId self_;
    std::shared_ptr<const std::vector<quark::NodeId>> snapshot_;
};

// 017 M7.2 PR E: a single MQTT 5 User Property (key, value) — QUARK_SERIALIZE's describe()-based codec
// (quark/core/wire.hpp) has no native `std::pair` support (only scalars/enums/std::string/std::vector/
// nested Described types — see wire.hpp's own tagged_value_size()/tagless_value_size() dispatch), so
// `PublishProperties::user_properties` (a `vector<pair<string,string>>`) is wire-shaped as a
// `vector<RelayUserProperty>` instead — a nested Described type is exactly the shape wire.hpp's own
// vector<T> support recurses into for non-trivial elements.
struct RelayUserProperty {
    std::string key;
    std::string value;
};
QUARK_SERIALIZE(RelayUserProperty, (1, key), (2, value));

// The cross-node relay message (016): one broadcast PUBLISH, from its originating node to a peer, carrying
// exactly what `NativeBroker::deliver_remote_publish()` needs to re-run local match-and-deliver there.
//
// 017 M7.2 PR E: gained the PUBLISH extras `deliver_remote_publish()`'s OLD banner documented as a known
// v1 gap (Message Expiry, Response Topic, Correlation Data, User Properties never survived a relay hop).
// Same `std::pair`-avoidance reasoning as RelayUserProperty above applies to `std::optional<T>` — also
// unsupported by wire.hpp's describe() codec — so "field present" is threaded as an explicit `has_*` bool
// alongside its own always-present value/default, rather than `std::optional<T>` itself.
// `message_expiry_remaining_s` is RELATIVE (seconds remaining as of the moment the ORIGINATING node
// forwarded it) — an absolute deadline would be meaningless once decoded on a peer with a different
// `steady_clock` epoch; `NativeBroker::deliver_remote_publish()` re-derives its OWN absolute deadline from
// this, the same way `handle_publish()` does from an inbound MQTT Message Expiry Interval property.
// Field DECLARATION order here is a pure sizeof/padding optimization (grouping the heavy
// string/vector members together, then the small scalars) — it does NOT affect wire behavior, which is
// driven entirely by QUARK_SERIALIZE's own (tag, member) list below (quark/core/describe.hpp: the
// tagless fast path bulk-copies fields in the MACRO's listed order, one `ar.field()` call per entry,
// regardless of the struct's actual physical layout). Kept this way because sizeof(BrokerRelayMsg) is
// load-bearing: quark::detail::MessagePool::kMaxPayload (192 bytes) is a hard per-cell ceiling shared by
// every actor message type in the process, not something to raise for one broker message — the field
// order below is what keeps this struct fitting under that ceiling with the full PUBLISH extras added.
struct BrokerRelayMsg {
    std::string topic;
    std::vector<std::byte> payload;
    std::string response_topic;
    std::vector<std::byte> correlation_data;
    std::vector<RelayUserProperty> user_properties;
    std::uint32_t message_expiry_remaining_s = 0;
    std::uint8_t qos = 0;
    bool has_message_expiry = false;
    bool has_response_topic = false;
    bool has_correlation_data = false;
};
QUARK_SERIALIZE(BrokerRelayMsg, (1, topic), (2, payload), (3, qos), (4, has_message_expiry),
                (5, message_expiry_remaining_s), (6, has_response_topic), (7, response_topic),
                (8, has_correlation_data), (9, correlation_data), (10, user_properties));

// The FIXED, well-known ActorId key for the ONE BrokerRelayActor per node (017 M6 brief: not per-topic —
// the broadcast fanout model means every peer's relay actor is the same target regardless of topic).
inline constexpr std::uint64_t kBrokerRelayKey = 1;

// One per node: receives a relayed PUBLISH over `DistributedRouter` and hands it to THIS node's
// NativeBroker via `deliver_remote_publish()` — local match-and-deliver ONLY, never re-broadcast (that
// invariant lives in native_broker.hpp: deliver_remote_publish() never touches peer_forwarder_).
struct BrokerRelayActor : quark::Actor<BrokerRelayActor, quark::Sequential> {
    using protocol = quark::Protocol<BrokerRelayMsg>;
    NativeBroker* broker = nullptr;

    void handle(const BrokerRelayMsg& m) noexcept {
        if (!broker) return;
        std::optional<std::chrono::seconds> remaining;
        if (m.has_message_expiry) remaining = std::chrono::seconds{m.message_expiry_remaining_s};
        PublishProperties props;
        if (m.has_response_topic) props.response_topic = m.response_topic;
        if (m.has_correlation_data) props.correlation_data = m.correlation_data;
        props.user_properties.reserve(m.user_properties.size());
        for (const RelayUserProperty& p : m.user_properties) props.user_properties.emplace_back(p.key, p.value);
        broker->deliver_remote_publish(m.topic, m.payload, m.qos, remaining, props);
    }
};

// BrokerCluster (017 §4, M6): wires ONE `NativeBroker` into cross-node PUBLISH broadcast over a real TCP
// inter-broker link + a Quark engine hosting one `BrokerRelayActor`. See this file's banner for the
// membership/transport/fanout/loop-prevention design and the DistributedRouter::tell() placement surprise.
//
// Construction does no I/O (mirrors NativeBroker/TcpTransport's own split); call start() to bind the
// inter-broker listener, dial peers, bring up the engine, and wire NativeBroker::set_peer_forwarder().
class BrokerCluster {
public:
    // `security` (default-constructed = disabled, see broker_cluster_security.hpp) opts into mTLS over
    // the inter-broker link (M5.1, 017 §10) — additive; the existing 4-arg call shape keeps working
    // unchanged (plain TcpTransport, no encryption/authentication, exactly as before M5.1).
    BrokerCluster(quark::NodeId self, std::uint16_t cluster_port, std::vector<PeerSpec> peers,
                  NativeBroker& broker, BrokerClusterSecurityConfig security = {})
        : self_(self),
          peers_(std::move(peers)),
          broker_(&broker),
          membership_(self_, all_node_ids(self_, peers_)),
          pool_(1024),
          transport_(aero::transport::TcpTransport::Config{self_, "0.0.0.0", cluster_port, {}})
#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
          ,
          security_(std::move(security))
#endif
    {
#if !defined(AERO_TLS_ENABLED) || !AERO_TLS_ENABLED
        (void)security;  // mTLS not compiled in this build — see broker_cluster_security.hpp's stub gate
#endif
    }

    ~BrokerCluster() { stop(); }

    BrokerCluster(const BrokerCluster&) = delete;
    BrokerCluster& operator=(const BrokerCluster&) = delete;

    // Bind the inter-broker listener, dial every configured peer, bring up the relay actor's engine
    // (MessagePool → Activation → Engine → register_actor → LocalRouter, `Runtime::configure_mes()`'s
    // exact recipe, runtime.hpp), and wire `broker.set_peer_forwarder()`. Order matters: DistributedRouter's
    // constructor registers `wire().on_receive(...)` (M5.1: `wire()` is `secure_transport_` when mTLS is
    // configured, chaining through to `transport_.on_receive()` itself — SecureTransport::on_receive()
    // is exactly that pass-through) — TcpTransport's own contract requires that be set BEFORE start()
    // (tcp_transport.hpp), so dist_router_ is built first (which is also why secure_transport_, above,
    // must be constructed before dist_router_ too) and transport_.start() comes last.
    [[nodiscard]] std::expected<void, std::string> start() {
#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
        // M5.1: opt-in mTLS over the inter-broker link. MUST run before any other mbedTLS call this
        // process makes — QuarkCpp's mbedTLS security adapter shares this project's own vendored
        // mbedTLS build (M5.1's own CMakeLists.txt banner), which is compiled with
        // MBEDTLS_THREADING_ALT (017 M5's cmake/patch_mbedtls.cmake, patch 3): that requires
        // mbedtls_threading_set_alt() before ANY OTHER mbedTLS call, full stop, or CTR_DRBG seeding
        // fails outright — the exact hazard M9.4's OpcUaSecurityConfig::apply_security_config()
        // discovered and fixed the same way (opcua_security.hpp's own banner has the full writeup).
        if (!security_.certificate_chain_file.empty()) {
            aero::pal::tls::detail::ensure_threading_registered();
            if (!load_cluster_identity(security_, identity_, trust_)) {
                return std::unexpected("BrokerCluster: failed to load cluster TLS identity/trust material");
            }
            std::array<std::byte, quark::adapters::MbedtlsAeadGcm::kKeyBytes> placeholder_key{};
            std::random_device rd;
            for (auto& b : placeholder_key) b = static_cast<std::byte>(rd() & 0xFF);
            placeholder_cipher_ = std::make_unique<quark::adapters::MbedtlsAeadGcm>(
                std::span<const std::byte, quark::adapters::MbedtlsAeadGcm::kKeyBytes>(placeholder_key));
            secure_transport_ =
                std::make_unique<quark::SecureTransport>(transport_, *placeholder_cipher_, self_);
            secure_transport_->enable_handshake(handshake_factory_, security_.cluster_id, *identity_, *trust_);
        }
#endif

        relay_.broker = broker_;
        activation_ = std::make_unique<quark::Activation>(&relay_, BrokerRelayActor::dispatch_table(), pool_.sink());
        engine_ = std::make_unique<quark::Engine<>>(quark::EngineConfig{/*workers*/ 1, /*shards*/ 1,
                                                                         /*budget*/ 64, 64});
        quark::register_actor<BrokerRelayActor>(*engine_, kBrokerRelayKey, *activation_);
        local_router_ = std::make_unique<quark::LocalRouter>(engine_->post_courier(), pool_);

        dist_router_ = std::make_unique<quark::DistributedRouter>(membership_, *local_router_, wire());
        dist_router_->register_remote<BrokerRelayActor, BrokerRelayMsg>();

        engine_->start();

        auto r = transport_.start();
        if (!r) return std::unexpected(r.error());
        for (const PeerSpec& p : peers_) {
            transport_.add_peer(p.id, p.host, p.port);
            // A port==0 placeholder (ephemeral-port peer, corrected later via add_peer() — this file's
            // own documented two-phase pattern) can't be dialed yet; priming against it now would only
            // burn SecureTransport's own "already pending" dedup window (ensure_handshake()) on a dial
            // that's guaranteed to fail, blocking the LATER add_peer()-triggered retry with the real
            // port from actually resending anything until that window elapses. Skip it here — add_peer()
            // primes once the real address is known.
            if (p.port != 0) prime_handshake(p.id);
        }

        broker_->set_peer_forwarder([this](std::string_view topic, std::span<const std::byte> payload,
                                           std::uint8_t qos,
                                           std::optional<std::chrono::seconds> message_expiry_remaining,
                                           const PublishProperties& props) {
            broadcast(topic, payload, qos, message_expiry_remaining, props);
        });
        return {};
    }

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
    // M5.1: forwards straight to SecureTransport::set_audit_sink() — tamper/replay drops and handshake
    // failures (see its own banner). No-op if security is disabled or before start() constructs
    // secure_transport_ (call after start() to actually observe anything).
    void set_cluster_link_audit_sink(quark::AuditSink sink) noexcept {
        if (secure_transport_) secure_transport_->set_audit_sink(sink);
    }
#endif

    // M5.1 diagnostics: proves the cluster link is ACTUALLY sealing/opening frames when mTLS is
    // configured (not just "PUBLISH still worked", which would also be true over the plain, unsealed
    // path) — both 0 when security is disabled or no traffic has crossed the link yet.
    [[nodiscard]] std::uint64_t cluster_link_frames_sealed() const noexcept {
#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
        return secure_transport_ ? secure_transport_->sealed() : 0;
#else
        return 0;
#endif
    }
    [[nodiscard]] std::uint64_t cluster_link_frames_opened() const noexcept {
#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
        return secure_transport_ ? secure_transport_->opened() : 0;
#else
        return 0;
#endif
    }
    // The port this node's inter-broker listener actually bound to (useful when cluster_port==0/ephemeral,
    // e.g. tests) — mirrors NativeBroker::listen_port()/TcpTransport::listen_port().
    [[nodiscard]] std::uint16_t cluster_listen_port() const noexcept { return transport_.listen_port(); }

    // Re-advertise a peer's inter-broker dial address, callable any time (before or after start()) — needed
    // when peers bind ephemeral cluster_port==0 and only learn each other's resolved port() post-start
    // (mirrors TcpTransport::add_peer()'s own documented pattern, tcp_transport.hpp). Forwards straight
    // through; `peers_` passed to the constructor already seeded the membership NodeId set either way.
    // M5.1: also (re-)primes a handshake with `peer` — the common ephemeral-port case corrects the
    // dial address via THIS call, after start()'s own priming attempt (which ran against the
    // constructor's placeholder port, e.g. 0, and so could not actually reach the peer yet).
    void add_peer(quark::NodeId peer, std::string host, std::uint16_t port) {
        transport_.add_peer(peer, std::move(host), port);
        prime_handshake(peer);
    }

    // Clean shutdown, in an order a TSan run actually exercises (a prior ordering — engine before
    // transport — raced: transport_'s reader thread can still be mid-delivery into dist_router_/engine_,
    // e.g. schedule_and_wake touching engine_'s Worker array, at the exact moment the main thread
    // destroys that Engine, a real concurrent use-after-free-shaped race, not a false positive):
    //   1. Defang the forwarder FIRST — no NEW outbound send from a NativeBroker session thread can start
    //      touching transport_/engine_ after this.
    //   2. Stop transport_ SECOND — TcpTransport::stop() synchronously JOINS its accept/reader threads
    //      (tcp_transport.hpp), so once this call returns, no thread can still be mid-delivery into
    //      dist_router_ (and so into engine_) — the inbound direction is now provably quiescent too.
    //   3. Only THEN stop/destroy engine_ — nothing can still be touching it.
    // Idempotent (engine_ nulled after stop; TcpTransport::stop() is itself idempotent).
    void stop() {
        if (broker_) broker_->set_peer_forwarder(nullptr);
        transport_.stop();
        if (engine_) {
            engine_->stop();
            engine_.reset();
        }
    }

private:
    // The active Transport DistributedRouter/broadcast() should use: secure_transport_ when mTLS is
    // configured (M5.1), the plain transport_ otherwise — identical to every pre-M5.1 deployment.
    quark::Transport& wire() noexcept {
#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
        if (secure_transport_) return *secure_transport_;
#endif
        return transport_;
    }

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
    // M5.1: SecureTransport's mTLS handshake is glare-free by NodeId ordering (secure_transport.hpp:
    // "only the lower NodeId initiates") — its OWN trigger for that is ensure_handshake(), called ONLY
    // from inside send() when no session yet exists for a peer, and DROPS whatever frame triggered it
    // (S2 "no session -> no delivery"). broadcast() only calls send() when THIS node has a locally-
    // originated PUBLISH to relay — with the broadcast fanout model (this file's banner), that can
    // easily never happen from the lower-NodeId side first (e.g. only the HIGHER NodeId's clients ever
    // publish), leaving BOTH sides permanently stuck: the higher NodeId keeps hitting the passive
    // "server waits for the peer's opening message" branch, and the lower NodeId never has a reason to
    // call send() at all. Proactively priming a handshake whenever THIS node learns/re-learns a peer's
    // dial address (start()'s own peer loop, and add_peer() for the common ephemeral-port-correction
    // case) closes that gap — the primed frame itself is dropped by design (S2), but the handshake it
    // kicks off completes in the background, so a real PUBLISH relayed moments later already has a
    // session waiting. A no-op if security is disabled, `peer` isn't THIS node's client-role
    // responsibility (glare-free rule), or a handshake for `peer` is already in flight/complete
    // (SecureTransport::send()'s own idempotent "already pending" check).
    void prime_handshake(quark::NodeId peer) {
        if (!secure_transport_ || self_.value >= peer.value) return;
        const BrokerRelayMsg empty{};
        quark::MessageFrame f;
        f.from = self_;
        f.to = peer;
        f.target = quark::actor_id_of<BrokerRelayActor>(kBrokerRelayKey);
        f.msg_type = quark::type_key_of<BrokerRelayMsg>();
        f.mode = quark::WireMode::Tagless;
        f.payload.resize(quark::tagless_size(empty));
        quark::encode_tagless(empty, f.payload.data());
        secure_transport_->send(peer, std::move(f));
    }
#else
    void prime_handshake(quark::NodeId) noexcept {}
#endif

    static std::vector<quark::NodeId> all_node_ids(quark::NodeId self, const std::vector<PeerSpec>& peers) {
        std::vector<quark::NodeId> ids{self};
        for (const PeerSpec& p : peers) ids.push_back(p.id);
        return ids;
    }

    // Broadcast one locally-originated PUBLISH to every OTHER node in the static membership snapshot
    // (017 M6 fanout model, this file's banner). Deliberately bypasses DistRef<A>::tell() — see the
    // banner's "surprise" note — and instead builds one MessageFrame per peer, handed straight to
    // `transport_.send()`. `WireMode::Tagless` is safe here: every node in a static-config cluster runs
    // the same AeroEdge binary, exactly the "matched peer" assumption `DistributedRouter`'s own default
    // peer schema makes (distribution.hpp's `default_peer_schema`).
    // 017 M7.2 PR E: `message_expiry_remaining`/`props` flatten into BrokerRelayMsg's own has_*-flagged
    // fields (see that struct's banner for why — no std::optional/std::pair support in the wire codec).
    void broadcast(std::string_view topic, std::span<const std::byte> payload, std::uint8_t qos,
                   std::optional<std::chrono::seconds> message_expiry_remaining,
                   const PublishProperties& props) {
        BrokerRelayMsg m;
        m.topic = std::string(topic);
        m.payload.assign(payload.begin(), payload.end());
        m.qos = qos;
        if (message_expiry_remaining) {
            m.has_message_expiry = true;
            m.message_expiry_remaining_s = static_cast<std::uint32_t>(
                std::max<std::chrono::seconds::rep>(0, message_expiry_remaining->count()));
        }
        if (props.response_topic) {
            m.has_response_topic = true;
            m.response_topic = *props.response_topic;
        }
        if (props.correlation_data) {
            m.has_correlation_data = true;
            m.correlation_data = *props.correlation_data;
        }
        m.user_properties.reserve(props.user_properties.size());
        for (const auto& [k, v] : props.user_properties) m.user_properties.push_back(RelayUserProperty{k, v});

        const quark::MembershipView v = membership_.view();
        for (quark::NodeId n : v.nodes()) {
            if (n == self_) continue;  // never relay to self — loop prevention starts here too
            quark::MessageFrame f;
            f.from = self_;
            f.to = n;
            f.target = quark::actor_id_of<BrokerRelayActor>(kBrokerRelayKey);
            f.msg_type = quark::type_key_of<BrokerRelayMsg>();
            f.mode = quark::WireMode::Tagless;
            f.payload.resize(quark::tagless_size(m));
            quark::encode_tagless(m, f.payload.data());
            wire().send(n, std::move(f));
        }
    }

    quark::NodeId self_;
    std::vector<PeerSpec> peers_;
    NativeBroker* broker_;
    StaticBrokerMembership membership_;
    quark::detail::MessagePool pool_;
    aero::transport::TcpTransport transport_;

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
    // M5.1 (017 §10): declared AFTER transport_ (so secure_transport_ is destroyed before transport_,
    // which it wraps by reference — reverse-declaration-order destruction) and BEFORE dist_router_
    // (which holds a Transport& into whichever of transport_/secure_transport_ wire() picked — that
    // target must outlive dist_router_, and it does, being declared earlier). identity_/trust_/
    // handshake_factory_/placeholder_cipher_ are all declared before secure_transport_ for the same
    // reason: it holds non-owning pointers into them (SecureTransport::enable_handshake()'s own
    // contract), so they must outlive it.
    BrokerClusterSecurityConfig security_;
    quark::adapters::MbedtlsHandshakeEngineFactory handshake_factory_;
    // SecureTransport's constructor requires a live Aead reference even in handshake mode, where it is
    // provably NEVER actually used (every PeerSession gets its own directional cipher pair from the
    // handshake instead, see secure_transport.hpp's send()/deliver()) — a random, never-reused key,
    // never a fixed/zero one, purely so this doesn't read as suspicious in review.
    std::unique_ptr<quark::adapters::MbedtlsAeadGcm> placeholder_cipher_;
    std::unique_ptr<quark::IdentityMaterial> identity_;
    std::unique_ptr<quark::TrustStore> trust_;
    std::unique_ptr<quark::SecureTransport> secure_transport_;
#endif

    BrokerRelayActor relay_;
    std::unique_ptr<quark::Activation> activation_;
    std::unique_ptr<quark::Engine<>> engine_;
    std::unique_ptr<quark::LocalRouter> local_router_;
    std::unique_ptr<quark::DistributedRouter> dist_router_;
};

}  // namespace aero::broker
