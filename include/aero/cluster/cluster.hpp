// AeroEdge cluster — the minimal multi-node DEPLOYMENT VIEW (spec 010 §2). THIN over Quark (R0): the
// live node set is Quark's `InProcessMembership` (Phase-7, the 021 SWIM seam's test double) BY DEFAULT,
// capability annotation is Quark's `CapabilityView` (025). AeroEdge adds ONLY the deployment-shaped
// wrapper + `place_actors`, which spreads a set of device actors across eligible nodes via the affinity
// policy (placement.hpp). NO membership protocol, NO gossip, NO hash ring live here (010 §4).
//
// Phase-8 follow-up (010 §5): `ClusterView` can now ALSO be backed by a real `quark::SwimMembership`
// (021, a genuine SWIM failure detector over a real Transport, shipped upstream since this banner was
// first written) — pass a live `quark::Membership&` to the second constructor below instead of letting
// ClusterView build its own in-process one. This makes the ALIVE/DEAD node set real; it does NOT make
// capability annotation real (that stays config-declared here, same as the single-node case — see
// runtime.hpp's `configure_fleet()`), and it does NOT provide cross-node actor state hand-off/migration
// (fenced migration, 010 §3, still needs a shared/networked durable store neither Quark nor AeroEdge
// ships today — see 010 §5's own writeup). This is OBSERVABILITY: an honest live view of who is
// actually reachable, nothing more.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aero/cluster/placement.hpp"

#include "quark/core/capabilities.hpp"   // NodeCapabilities, CapabilityView, make_capability_view (025)
#include "quark/core/ids.hpp"            // ActorId, NodeId
#include "quark/core/membership.hpp"     // Membership, InProcessMembership, MembershipView (021 seam)

namespace aero::cluster {

// One node in the deployment view: its Quark NodeId + its advertised static capabilities (025).
struct NodeSpec {
    quark::NodeId id{};
    quark::NodeCapabilities caps;
};

// A device actor to be placed: its Quark ActorId + the affinity requirement derived from its device
// config. `name` is diagnostics only (device label in logs / the returned plan).
struct DeviceActor {
    quark::ActorId id{};
    PlacementRequirement requirement;
    std::string name;
};

// The result of spreading a set of device actors across the cluster (010 §2.2). `assignments` maps each
// PLACED actor to its owning node; `unplaceable` lists actors whose REQUIRED capabilities no live node
// satisfies (a clear value, never a crash); `load` is the per-node actor count for spread assertions.
struct PlacementPlan {
    std::unordered_map<quark::ActorId, quark::NodeId> assignments;
    std::vector<quark::ActorId> unplaceable;
    std::unordered_map<std::uint64_t, int> load;  // NodeId::value → #actors placed there

    [[nodiscard]] int load_of(quark::NodeId n) const noexcept {
        const auto it = load.find(n.value);
        return it == load.end() ? 0 : it->second;
    }
};

// A minimal multi-node deployment view (010 §2). Owns the node specs and publishes a Quark
// `CapabilityView` (the annotated membership placement reads).
//
// TWO membership modes, chosen by which constructor is used:
//   - Default (single-arg): builds its own `quark::InProcessMembership` — a deterministic test double,
//     not a failure detector (Phase-7's original shape, still the right choice for offline
//     policy tests like tests/cluster/placement_affinity.cpp/cluster_place.cpp).
//   - Live (two-arg): wraps a CALLER-OWNED, already-constructed `quark::Membership&` — in production
//     this is a real `quark::SwimMembership` (021) ticking on its own thread elsewhere (see
//     runtime.hpp's `configure_fleet()`). ClusterView does not own or tick it; call `refresh()`
//     periodically to pull whatever `live_membership.view()` currently reports. `live_membership` must
//     outlive this ClusterView.
class ClusterView {
public:
    explicit ClusterView(std::vector<NodeSpec> nodes)
        : nodes_(std::move(nodes)),
          owned_membership_(std::make_unique<quark::InProcessMembership>(self_id(nodes_), node_ids(nodes_))),
          membership_(owned_membership_.get()) {
        refresh();
    }

    ClusterView(std::vector<NodeSpec> nodes, quark::Membership& live_membership)
        : nodes_(std::move(nodes)), membership_(&live_membership) {
        refresh();
    }

    [[nodiscard]] const quark::CapabilityView& capabilities() const noexcept { return view_; }
    [[nodiscard]] const quark::MembershipView& membership() const noexcept { return view_.membership(); }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    // Place ONE device actor via the affinity policy (placement.hpp): filter to eligible, then Quark HRW.
    [[nodiscard]] PlacementResult place(quark::ActorId id, const PlacementRequirement& req) const {
        return aero::cluster::place(id, req, view_);
    }

    // Re-pulls the underlying membership's CURRENT snapshot into a fresh CapabilityView — a no-op for
    // the offline/InProcessMembership mode (it never changes after construction here), the actual point
    // for the live-SwimMembership mode: the caller's own tick loop calls this after each tick() (or on
    // whatever cadence it prefers) so `membership()`/`place()` reflect real, current aliveness. NodeSpec
    // capabilities themselves stay config-declared/static either way (see this file's own banner).
    void refresh() {
        // Annotate the membership's live snapshot with the per-node capability map (025). The map is
        // keyed by NodeId::value, exactly as CapabilityView expects (capabilities.hpp).
        auto caps = std::make_shared<quark::CapabilityView::CapMap>();
        for (const auto& n : nodes_) caps->emplace(n.id.value, n.caps);
        view_ = quark::CapabilityView{membership_->view(), std::move(caps)};
    }

private:
    static quark::NodeId self_id(const std::vector<NodeSpec>& ns) {
        return ns.empty() ? quark::NodeId{1} : ns.front().id;  // a well-formed membership includes self
    }
    static std::vector<quark::NodeId> node_ids(const std::vector<NodeSpec>& ns) {
        std::vector<quark::NodeId> ids;
        ids.reserve(ns.size());
        for (const auto& n : ns) ids.push_back(n.id);
        return ids;
    }

    std::vector<NodeSpec> nodes_;
    std::unique_ptr<quark::InProcessMembership> owned_membership_;  // set only by the offline constructor
    quark::Membership* membership_;  // either owned_membership_.get() or the caller's live_membership
    quark::CapabilityView view_;
};

// Spread a set of device actors across the cluster (010 §2.2): apply the affinity policy to each,
// recording the assignment + per-node load, collecting the unplaceable ones. Deterministic (a pure
// function of the actors + the cluster view), so every node computes the same plan (coordinator-free).
[[nodiscard]] inline PlacementPlan place_actors(const std::vector<DeviceActor>& actors,
                                                const ClusterView& cluster) {
    PlacementPlan plan;
    for (const auto& a : actors) {
        const PlacementResult r = cluster.place(a.id, a.requirement);
        if (r.ok) {
            plan.assignments.emplace(a.id, r.node);
            ++plan.load[r.node.value];
        } else {
            plan.unplaceable.push_back(a.id);
        }
    }
    return plan;
}

}  // namespace aero::cluster
