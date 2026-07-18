// AeroEdge Transport — the adapter seam over Quark's `Transport` (014 §2, X1).
//
// THE LOAD-BEARING DECISION (014 §2 / X1): AeroEdge does NOT define a parallel inter-actor transport
// interface. Quark 010 already owns the exact seam this phase's diagram (Actor Runtime → Transport
// Interface → {Local, MQTT, gRPC} → Remote Actor) asks for: `quark::Transport` —
//     void send(NodeId to, MessageFrame frame);   // fire-and-forget byte move
//     void on_receive(std::function<void(MessageFrame)>);  // inbound → DistributedRouter::deliver
// So an AeroEdge transport adapter *IS-A* `quark::Transport`; it plugs straight into
// `quark::DistributedRouter`'s third seam. `ITransport` below only ADDS AeroEdge-side identity and a
// transport-class tag on top of that same interface — it never redefines send/on_receive. Every
// adapter must honor the seam's contract C1–C5 (014 §3), above all per-`(from → target)` FIFO (C1);
// a substrate that can reorder composes `Resequencer` (resequencer.hpp) to restore it.
//
// THIS HEADER SUPPLIES: (1) `ITransport` — the named AeroEdge adapter base (= quark::Transport + name +
// class); (2) `LoopbackTransportAdapter` — the "Local/Loopback" box in the diagram, wrapping Quark's
// in-process `LoopbackTransport` test double as an ITransport; (3) `TransportSelector` — the per-peer
// selection policy (014 §8, X5): pick a transport BOTH endpoints share, by advertised reachability.
//
// LAYERING: this is an optional adapter library (aero-transport → quark::quark). It is NOT pulled into
// aero-core/aero-sdk or the flow hot path (R0/CONVENTIONS "heavier backends are optional adapters
// behind the seam").
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/ids.hpp"        // NodeId
#include "quark/core/transport.hpp"  // quark::Transport, MessageFrame, LoopbackTransport/Fabric

namespace aero::transport {

using quark::MessageFrame;
using quark::NodeId;

// The transport classes AeroEdge can select among for a peer link (014 §2 diagram + §7 table). Order
// encodes the §7 latency/backpressure posture used as the selection preference below: flow-controlled,
// point-to-point transports (loopback/tcp/grpc) are preferred; a broker (mqtt) is the last resort —
// it is higher-latency AND breaks end-to-end stream backpressure (X7).
enum class TransportClass : std::uint8_t {
    Loopback = 0,  // same node / in-process (the Local Queue box) — no wire, no serialization
    Tcp = 1,       // Quark's coordinator-free default fabric (014 §4 B2) — flow-controlled
    Grpc = 2,      // HTTP/2 bidi stream; stream order gives C1, flow-controlled (014 §6)
    Mqtt = 3,      // broker tunnel for NAT/firewalled links; needs the resequencer (014 §5), no BP (X7)
};

// The named AeroEdge adapter base. IS-A quark::Transport (X1) — so `DistributedRouter dist(m, local,
// adapter)` accepts it directly. Adds only diagnostics/identity; send/on_receive stay Quark's seam.
class ITransport : public quark::Transport {
public:
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual TransportClass transport_class() const noexcept = 0;
};

// ============================================================================================
// Local/Loopback adapter — the "Local Queue" box (014 §2). Wraps Quark's in-process LoopbackTransport
// (the byte-mover test double that delivers a single sender's frames FIFO by construction) as an
// ITransport. This is the same-node relay/test path; it is the ONLY transport that needs no shim
// because Quark's fabric already delivers a single sender's frames in order.
// ============================================================================================
class LoopbackTransportAdapter final : public ITransport {
public:
    LoopbackTransportAdapter(quark::LoopbackFabric& fabric, NodeId self) noexcept
        : inner_(fabric, self) {}

    void send(NodeId to, MessageFrame frame) override { inner_.send(to, std::move(frame)); }
    void on_receive(std::function<void(MessageFrame)> cb) override { inner_.on_receive(std::move(cb)); }

    [[nodiscard]] std::string_view name() const noexcept override { return "loopback"; }
    [[nodiscard]] TransportClass transport_class() const noexcept override {
        return TransportClass::Loopback;
    }

private:
    quark::LoopbackTransport inner_;
};

// ============================================================================================
// TransportSelector — per-peer transport selection (014 §8, X5). "Which transport reaches which node
// is a deployment policy, informed by node capabilities: a node advertises which transports it is
// reachable on (reachable=tcp,mqtt); the router picks a transport BOTH endpoints share."
//
// The selector holds the transports THIS node can drive (register_transport) and each peer's advertised
// reachability (set_peer_reachability). select() returns the most-preferred transport whose class the
// peer advertises AND this node can drive. No shared transport → nullptr: the caller then relays through
// Quark's FIFO-preserving DHT-relay (026, 014 §8) — AeroEdge invents no bespoke router (X5).
// ============================================================================================
class TransportSelector {
public:
    // Register a transport this node can drive. Later registration of the same class wins (last-wins).
    void register_transport(ITransport& t) { local_[t.transport_class()] = &t; }

    // Advertise a peer's reachability (014 §8 node capability `reachable=...`).
    void set_peer_reachability(NodeId peer, std::vector<TransportClass> classes) {
        std::sort(classes.begin(), classes.end());  // canonical order → deterministic preference scan
        classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
        peers_[peer] = std::move(classes);
    }

    // The class chosen for `peer`: the most-preferred (lowest enum value = flow-controlled first, §7)
    // class that BOTH the peer advertises and this node can drive. nullopt ⇒ relay (X5).
    [[nodiscard]] std::optional<TransportClass> select_class(NodeId peer) const {
        const auto pit = peers_.find(peer);
        if (pit == peers_.end()) return std::nullopt;
        for (const TransportClass cls : pit->second) {  // ascending == preference order
            if (local_.find(cls) != local_.end()) return cls;
        }
        return std::nullopt;
    }

    // The transport instance chosen for `peer`, or nullptr ⇒ no shared transport → relay (X5).
    [[nodiscard]] ITransport* select(NodeId peer) const {
        const auto cls = select_class(peer);
        if (!cls) return nullptr;
        const auto it = local_.find(*cls);
        return it == local_.end() ? nullptr : it->second;
    }

private:
    std::unordered_map<TransportClass, ITransport*> local_;      // transports this node can drive
    std::unordered_map<NodeId, std::vector<TransportClass>> peers_;  // per-peer advertised reachability
};

}  // namespace aero::transport
