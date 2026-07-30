// AeroEdge Phase-8 gate (spec 010 §2.2): spreading MANY device actors across MANY nodes by capability.
//
// A 6-node, 3-segment deployment. 90 device actors in three capability classes are placed by the
// affinity policy (cluster.hpp → Quark Weighted-HRW). Asserts:
//   * AFFINITY per class — each class's actors land ONLY on the nodes eligible for it, never elsewhere.
//   * NO ELIGIBLE NODE STARVED — every node that is eligible for some class carries actors (> 0).
//   * NO NODE OVERLOADED BEYOND POLICY — within an eligible pair the load is SHARED (neither node
//     absorbs the whole class), i.e. HRW spreads rather than pinning all to one.
//   * total placed == 90, none unplaceable.
//
// Exit code 0 = OK (ctest gate). Prints "FAIL" on any mismatch.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "aero/cluster/cluster.hpp"

using namespace quark;
using aero::cluster::CapabilityConstraint;
using aero::cluster::ClusterView;
using aero::cluster::DeviceActor;
using aero::cluster::NodeSpec;
using aero::cluster::PlacementPlan;
using aero::cluster::PlacementRequirement;

namespace {

constexpr TypeKey kEdge{0xED9E};
ActorId edge_id(std::uint64_t key) noexcept { return ActorId{kEdge, key}; }

PlacementRequirement segment_req(const char* seg, const char* gw_flag) {
    PlacementRequirement r;
    r.required.push_back(CapabilityConstraint::label("segment", seg));
    r.required.push_back(CapabilityConstraint::flag(gw_flag));
    return r;
}

// Append `n` device actors (keys starting at `base`) with requirement `req` to `out`.
void add_class(std::vector<DeviceActor>& out, std::uint64_t base, int n,
               const PlacementRequirement& req, const char* name) {
    for (int i = 0; i < n; ++i)
        out.push_back(DeviceActor{edge_id(base + static_cast<std::uint64_t>(i)), req, name});
}

}  // namespace

int main() {
    bool ok = true;

    // 6 nodes: three segment-pairs, each pair the eligible set for its class (equal weight ⇒ HRW split).
    ClusterView cluster(std::vector<NodeSpec>{
        {NodeId{1}, NodeCapabilities{Label{"segment", "line-3"}, Flag{"opcua-gateway"}}},
        {NodeId{2}, NodeCapabilities{Label{"segment", "line-3"}, Flag{"opcua-gateway"}}},
        {NodeId{3}, NodeCapabilities{Label{"segment", "line-5"}, Flag{"opcua-gateway"}}},
        {NodeId{4}, NodeCapabilities{Label{"segment", "line-5"}, Flag{"opcua-gateway"}}},
        {NodeId{5}, NodeCapabilities{Label{"segment", "line-1"}, Flag{"modbus"}}},
        {NodeId{6}, NodeCapabilities{Label{"segment", "line-1"}, Flag{"modbus"}}},
    });

    std::vector<DeviceActor> actors;
    add_class(actors, /*base*/ 1,   30, segment_req("line-3", "opcua-gateway"), "l3");   // eligible {1,2}
    add_class(actors, /*base*/ 101, 30, segment_req("line-5", "opcua-gateway"), "l5");   // eligible {3,4}
    add_class(actors, /*base*/ 201, 30, segment_req("line-1", "modbus"),        "l1");   // eligible {5,6}

    const PlacementPlan plan = aero::cluster::place_actors(actors, cluster);

    ok &= plan.unplaceable.empty();
    ok &= plan.assignments.size() == 90;

    // AFFINITY per class: a line-3 actor must be on {1,2}, a line-5 on {3,4}, a line-1 on {5,6}.
    auto owner_in = [&](std::uint64_t key, NodeId x, NodeId y) {
        const auto it = plan.assignments.find(edge_id(key));
        return it != plan.assignments.end() && (it->second == x || it->second == y);
    };
    bool affinity = true;
    for (std::uint64_t k = 1;   k <= 30;  ++k) affinity &= owner_in(k,       NodeId{1}, NodeId{2});
    for (std::uint64_t k = 101; k <= 130; ++k) affinity &= owner_in(k,       NodeId{3}, NodeId{4});
    for (std::uint64_t k = 201; k <= 230; ++k) affinity &= owner_in(k,       NodeId{5}, NodeId{6});
    ok &= affinity;

    // NO ELIGIBLE NODE STARVED + NO NODE OVERLOADED BEYOND POLICY: every node > 0, and within a pair
    // neither node absorbs the entire 30-actor class (HRW shares the load).
    int l[7];
    for (int i = 1; i <= 6; ++i) l[i] = plan.load_of(NodeId{static_cast<std::uint64_t>(i)});
    for (int i = 1; i <= 6; ++i) ok &= (l[i] > 0);                 // none starved
    ok &= (l[1] + l[2] == 30 && l[3] + l[4] == 30 && l[5] + l[6] == 30);  // class totals conserved
    ok &= (l[1] < 30 && l[2] < 30 && l[3] < 30 && l[4] < 30 && l[5] < 30 && l[6] < 30);  // shared, not pinned

    std::printf("[cluster] 90 actors / 3 classes → loads n1=%d n2=%d | n3=%d n4=%d | n5=%d n6=%d "
                "(affinity=%s starved=%s)\n",
                l[1], l[2], l[3], l[4], l[5], l[6], affinity ? "ok" : "VIOLATED",
                (l[1] && l[2] && l[3] && l[4] && l[5] && l[6]) ? "none" : "SOME");
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
