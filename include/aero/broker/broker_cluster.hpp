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
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aero/broker/native_broker.hpp"
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

// The cross-node relay message (016): one broadcast PUBLISH, from its originating node to a peer, carrying
// exactly what `NativeBroker::deliver_remote_publish()` needs to re-run local match-and-deliver there.
struct BrokerRelayMsg {
    std::string topic;
    std::vector<std::byte> payload;
    std::uint8_t qos = 0;
};
QUARK_SERIALIZE(BrokerRelayMsg, (1, topic), (2, payload), (3, qos));

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
        if (broker) broker->deliver_remote_publish(m.topic, m.payload, m.qos);
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
    BrokerCluster(quark::NodeId self, std::uint16_t cluster_port, std::vector<PeerSpec> peers,
                  NativeBroker& broker)
        : self_(self),
          peers_(std::move(peers)),
          broker_(&broker),
          membership_(self_, all_node_ids(self_, peers_)),
          pool_(1024),
          transport_(aero::transport::TcpTransport::Config{self_, "0.0.0.0", cluster_port, {}}) {}

    ~BrokerCluster() { stop(); }

    BrokerCluster(const BrokerCluster&) = delete;
    BrokerCluster& operator=(const BrokerCluster&) = delete;

    // Bind the inter-broker listener, dial every configured peer, bring up the relay actor's engine
    // (MessagePool → Activation → Engine → register_actor → LocalRouter, `Runtime::configure_mes()`'s
    // exact recipe, runtime.hpp), and wire `broker.set_peer_forwarder()`. Order matters: DistributedRouter's
    // constructor registers `transport_.on_receive(...)` — TcpTransport's own contract requires that be set
    // BEFORE start() (tcp_transport.hpp), so dist_router_ is built first and transport_.start() comes last.
    [[nodiscard]] std::expected<void, std::string> start() {
        relay_.broker = broker_;
        activation_ = std::make_unique<quark::Activation>(&relay_, BrokerRelayActor::dispatch_table(), pool_.sink());
        engine_ = std::make_unique<quark::Engine<>>(quark::EngineConfig{/*workers*/ 1, /*shards*/ 1,
                                                                         /*budget*/ 64, 64});
        quark::register_actor<BrokerRelayActor>(*engine_, kBrokerRelayKey, *activation_);
        local_router_ = std::make_unique<quark::LocalRouter>(engine_->post_courier(), pool_);

        dist_router_ = std::make_unique<quark::DistributedRouter>(membership_, *local_router_, transport_);
        dist_router_->register_remote<BrokerRelayActor, BrokerRelayMsg>();

        engine_->start();

        auto r = transport_.start();
        if (!r) return std::unexpected(r.error());
        for (const PeerSpec& p : peers_) transport_.add_peer(p.id, p.host, p.port);

        broker_->set_peer_forwarder([this](std::string_view topic, std::span<const std::byte> payload,
                                           std::uint8_t qos) { broadcast(topic, payload, qos); });
        return {};
    }

    // The port this node's inter-broker listener actually bound to (useful when cluster_port==0/ephemeral,
    // e.g. tests) — mirrors NativeBroker::listen_port()/TcpTransport::listen_port().
    [[nodiscard]] std::uint16_t cluster_listen_port() const noexcept { return transport_.listen_port(); }

    // Re-advertise a peer's inter-broker dial address, callable any time (before or after start()) — needed
    // when peers bind ephemeral cluster_port==0 and only learn each other's resolved port() post-start
    // (mirrors TcpTransport::add_peer()'s own documented pattern, tcp_transport.hpp). Forwards straight
    // through; `peers_` passed to the constructor already seeded the membership NodeId set either way.
    void add_peer(quark::NodeId peer, std::string host, std::uint16_t port) {
        transport_.add_peer(peer, std::move(host), port);
    }

    // Clean shutdown: defang the forwarder FIRST (so no in-flight PUBLISH on a NativeBroker session thread
    // can call into a transport/engine mid-teardown), then stop the engine, then the transport. Idempotent
    // (engine_ nulled after stop; TcpTransport::stop() is itself idempotent).
    void stop() {
        if (broker_) broker_->set_peer_forwarder(nullptr);
        if (engine_) {
            engine_->stop();
            engine_.reset();
        }
        transport_.stop();
    }

private:
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
    void broadcast(std::string_view topic, std::span<const std::byte> payload, std::uint8_t qos) {
        const BrokerRelayMsg m{std::string(topic), std::vector<std::byte>(payload.begin(), payload.end()), qos};
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
            transport_.send(n, std::move(f));
        }
    }

    quark::NodeId self_;
    std::vector<PeerSpec> peers_;
    NativeBroker* broker_;
    StaticBrokerMembership membership_;
    quark::detail::MessagePool pool_;
    aero::transport::TcpTransport transport_;

    BrokerRelayActor relay_;
    std::unique_ptr<quark::Activation> activation_;
    std::unique_ptr<quark::Engine<>> engine_;
    std::unique_ptr<quark::LocalRouter> local_router_;
    std::unique_ptr<quark::DistributedRouter> dist_router_;
};

}  // namespace aero::broker
