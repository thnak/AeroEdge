// AeroEdge Phase-8 gate (spec 010 §2): the device-AFFINITY placement policy over Quark's Weighted-HRW.
//
// Proves the three properties 010 §2.1 requires of AeroEdge's placement POLICY (Quark owns the actual
// pick — this test asserts the eligibility FILTER AeroEdge adds is honoured, and that the pick still
// spreads):
//   (i)   AFFINITY RESPECTED — a device actor is placed on a node that satisfies its REQUIRED
//         capabilities (network line-of-sight to its device), never on an ineligible node.
//   (ii)  UNPLACEABLE IS A CLEAR VALUE — a device requiring a capability no live node advertises yields
//         a `NoEligibleNode` error, not a crash / not an arbitrary placement.
//   (iii) SPREAD — many actors sharing one requirement (same eligible subset) spread across the eligible
//         nodes via Quark's HRW; they do NOT all pile onto one node.
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
using aero::cluster::PlacementError;
using aero::cluster::PlacementRequirement;

namespace {

// The device-actor type key (all device instances share it; the instance KEY differentiates them).
constexpr TypeKey kEdge{0xED9E};
ActorId edge_id(std::uint64_t key) noexcept { return ActorId{kEdge, key}; }

// A requirement for an OPC-UA device on line-3 (the common case: many such actors, eligible on the two
// line-3 opcua-gateway nodes).
PlacementRequirement line3_opcua() {
    PlacementRequirement r;
    r.required.push_back(CapabilityConstraint::label("segment", "line-3"));
    r.required.push_back(CapabilityConstraint::flag("opcua-gateway"));
    return r;
}

}  // namespace

int main() {
    bool ok = true;

    // Four edge nodes with DIFFERING capabilities (025). Nodes 1 & 2 are the line-3 OPC-UA gateways
    // (equal capacity ⇒ uniform HRW split); node 3 is line-5; node 4 is a different plant.
    ClusterView cluster(std::vector<NodeSpec>{
        {NodeId{1}, NodeCapabilities{Label{"plant", "hanoi"}, Label{"segment", "line-3"},
                                     Flag{"opcua-gateway"}, Scalar{"weight", 1.0}}},
        {NodeId{2}, NodeCapabilities{Label{"plant", "hanoi"}, Label{"segment", "line-3"},
                                     Flag{"opcua-gateway"}, Scalar{"weight", 1.0}}},
        {NodeId{3}, NodeCapabilities{Label{"plant", "hanoi"}, Label{"segment", "line-5"},
                                     Flag{"opcua-gateway"}, Scalar{"weight", 1.0}}},
        {NodeId{4}, NodeCapabilities{Label{"plant", "saigon"}, Label{"segment", "line-1"},
                                     Flag{"modbus"}, Scalar{"weight", 1.0}}},
    });

    // ===== (i) AFFINITY RESPECTED + (iii) SPREAD ===================================================
    // 64 OPC-UA/line-3 device actors. Eligible subset = {node 1, node 2}. Every placement must land on
    // an eligible node; across the 64 the load must spread over BOTH (not all on one).
    const PlacementRequirement req = line3_opcua();
    std::vector<DeviceActor> actors;
    for (std::uint64_t k = 1; k <= 64; ++k)
        actors.push_back(DeviceActor{edge_id(k), req, "opcua-dev"});

    const auto plan = aero::cluster::place_actors(actors, cluster);

    ok &= plan.unplaceable.empty();                 // all 64 are placeable (2 eligible nodes)
    ok &= plan.assignments.size() == 64;

    // Affinity: every placed actor is on node 1 or 2, and that node truly satisfies the requirement.
    bool all_eligible = true;
    for (const auto& [id, node] : plan.assignments) {
        const bool on_gateway = (node == NodeId{1} || node == NodeId{2});
        const bool node_ok = req.node_eligible(cluster.capabilities().capabilities_of(node));
        if (!on_gateway || !node_ok) all_eligible = false;
    }
    ok &= all_eligible;

    // Spread: BOTH eligible gateways carry actors; the ineligible nodes 3 & 4 carry NONE.
    const int l1 = plan.load_of(NodeId{1});
    const int l2 = plan.load_of(NodeId{2});
    ok &= (l1 > 0 && l2 > 0);
    ok &= (plan.load_of(NodeId{3}) == 0 && plan.load_of(NodeId{4}) == 0);
    ok &= (l1 + l2 == 64);

    std::printf("[affinity] 64 opcua/line-3 actors → node1=%d node2=%d (node3=%d node4=%d) "
                "eligible-only=%s spread=%s\n",
                l1, l2, plan.load_of(NodeId{3}), plan.load_of(NodeId{4}),
                all_eligible ? "yes" : "NO", (l1 > 0 && l2 > 0) ? "yes" : "NO");

    // ===== (ii) UNPLACEABLE IS A CLEAR VALUE =======================================================
    // A device on a PROFINET network — no live node advertises `profinet`. Must be a NoEligibleNode
    // value, never a fallback placement onto an ineligible node.
    PlacementRequirement profinet;
    profinet.required.push_back(CapabilityConstraint::flag("profinet"));
    const auto pr = cluster.place(edge_id(999), profinet);
    ok &= (!pr.ok && pr.error == PlacementError::NoEligibleNode);
    std::printf("[unplaceable] profinet device → ok=%d error=%d (expected ok=0 NoEligibleNode)\n",
                pr.ok, static_cast<int>(pr.error));

    // ===== affinity to a UNIQUE eligible node ======================================================
    // A saigon/line-1 device is eligible ONLY on node 4 — it must land there, every time (deterministic).
    PlacementRequirement saigon;
    saigon.required.push_back(CapabilityConstraint::label("plant", "saigon"));
    const auto sr = cluster.place(edge_id(7), saigon);
    ok &= (sr.ok && sr.node == NodeId{4});
    std::printf("[unique] saigon device → node=%llu ok=%d (expected node=4)\n",
                (unsigned long long)sr.node.value, sr.ok);

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
