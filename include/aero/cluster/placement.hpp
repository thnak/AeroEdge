// AeroEdge cluster — the device-AFFINITY placement POLICY (spec 010 §2). THIN over Quark (R0): this
// header adds ONLY the AeroEdge policy — which node a device actor should PREFER to live on — and
// delegates the actual node pick to Quark's real Weighted-HRW (`place_weighted`, ADR-013). It builds
// NO hash ring, NO gossip, NO membership (010 §4 — those are Quark 021/025/026).
//
// THE MODEL (010 §2.1). A node advertises static CAPABILITIES (Quark 025 `NodeCapabilities`:
// `plant=hanoi`, `segment=line-3`, `has=opcua-gateway`). A device actor declares a
// `PlacementRequirement` derived from its device config: REQUIRED capabilities (a hard filter — the
// actor MUST run where it has network line-of-sight to its device) and PREFERRED capabilities (a soft
// rank). AeroEdge computes the ELIGIBLE node subset (the policy), then hands that subset to Quark's
// HRW so load spreads across eligible nodes.
//
// WHY RUNTIME, not Quark's compile-time `Require<HasFlag<"...">>`. Quark 025 expresses affinity as
// COMPILE-TIME policy tags on the actor type (`Placement<HashById, Require<Gpu>>`). A device actor's
// required capability comes from DEVICE CONFIG (device registry / JSON, 010 §5 open question), which is
// only known at RUNTIME — a compile-time `Require<>` cannot encode "this instance needs
// segment=line-3". So AeroEdge mirrors Quark's `Require`/`Prefer` semantics as a RUNTIME predicate over
// `NodeCapabilities`, then calls Quark's EXACT `place_weighted` over the surviving subset. The hash,
// the tie-break, the proportional log-WRH weighting are all Quark's (same splitmix64 mixer) — only the
// eligibility DOMAIN is narrowed. This is the "filter-then-HRW" the phase spec sanctions.
#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "quark/core/capabilities.hpp"          // NodeCapabilities, CapabilityView (025)
#include "quark/core/ids.hpp"                     // ActorId, NodeId
#include "quark/core/placement_policies.hpp"      // place_weighted — Quark's real Weighted-HRW (ADR-013)

namespace aero::cluster {

// A single RUNTIME capability constraint derived from a device's config (010 §2.1). Mirrors Quark's
// compile-time predicates (`HasFlag` / `HasLabel`, 025) but evaluated at runtime because a device's
// required/preferred capability is config-driven, not a compile-time type.
struct CapabilityConstraint {
    enum class Kind { Flag, Label };
    Kind kind = Kind::Flag;
    std::string name;   // flag name, or label KEY
    std::string value;  // label VALUE (unused for Flag)

    [[nodiscard]] static CapabilityConstraint flag(std::string n) {
        return CapabilityConstraint{Kind::Flag, std::move(n), {}};
    }
    [[nodiscard]] static CapabilityConstraint label(std::string k, std::string v) {
        return CapabilityConstraint{Kind::Label, std::move(k), std::move(v)};
    }

    // Does node capability-set `c` satisfy this constraint? (Same test Quark's HasFlag/HasLabel run.)
    [[nodiscard]] bool satisfied_by(const quark::NodeCapabilities& c) const noexcept {
        switch (kind) {
            case Kind::Flag:  return c.has_flag(name);
            case Kind::Label: return c.label(name) == value;
        }
        return false;
    }
};

// The AeroEdge device-affinity requirement (010 §2.1), derived from device config. `required` is the
// HARD filter (== Quark `Require<>`): a node must satisfy ALL of them to be eligible. `preferred` is
// the SOFT rank (== Quark `Prefer<>`): among eligible nodes, prefer those satisfying ALL preferred
// constraints, but fall back to the full eligible set if none qualify.
struct PlacementRequirement {
    std::vector<CapabilityConstraint> required;
    std::vector<CapabilityConstraint> preferred;

    // All REQUIRED constraints hold (the hard eligibility filter). Empty required ⇒ any node eligible.
    [[nodiscard]] bool node_eligible(const quark::NodeCapabilities& c) const noexcept {
        for (const auto& r : required)
            if (!r.satisfied_by(c)) return false;
        return true;
    }
    // All PREFERRED constraints hold (the soft rank). Empty preferred ⇒ vacuously preferred.
    [[nodiscard]] bool node_preferred(const quark::NodeCapabilities& c) const noexcept {
        for (const auto& p : preferred)
            if (!p.satisfied_by(c)) return false;
        return true;
    }
};

// Why a placement could not be made (a value, never UB — mirrors Quark's `result<NodeId>` taxonomy).
enum class PlacementError {
    None,
    NoMembership,    // the cluster view is empty — no node to place onto
    NoEligibleNode,  // no live node satisfies the REQUIRED capabilities (unplaceable device)
};

struct PlacementResult {
    bool ok = false;
    quark::NodeId node{};
    PlacementError error = PlacementError::None;
};

// The AeroEdge device-affinity placement (010 §2). FILTER (the policy) → HRW (Quark's).
//
//   1. eligible = { n : all REQUIRED capabilities of `req` hold for n }   — AeroEdge policy
//   2. narrow to nodes satisfying ALL PREFERRED capabilities, if any do   — AeroEdge policy (soft)
//   3. winner = quark::place_weighted(id.hash(), candidates, view)        — QUARK's Weighted-HRW
//
// Step 3 is Quark's EXACT ADR-013 proportional log-WRH over the restricted candidate span — the same
// hash/tie-break/weighting Quark uses cluster-wide, just over the eligible domain. No hash ring (R0).
[[nodiscard]] inline PlacementResult place(quark::ActorId id, const PlacementRequirement& req,
                                           const quark::CapabilityView& view) {
    if (view.empty()) return {false, {}, PlacementError::NoMembership};

    // (1) Hard eligibility filter — the device-affinity POLICY (010 §2.1). Control plane; a vector
    // alloc off the drain hot path is fine (mirrors Quark's own resolver, placement_policies.hpp).
    std::vector<quark::NodeId> eligible;
    eligible.reserve(view.size());
    for (quark::NodeId n : view.nodes())
        if (req.node_eligible(view.capabilities_of(n))) eligible.push_back(n);
    if (eligible.empty()) return {false, {}, PlacementError::NoEligibleNode};

    // (2) Soft PREFER rank — restrict to nodes satisfying all preferred caps IF any qualify (Quark
    // `Prefer<>` semantics); otherwise the full eligible set stays the candidate domain.
    std::vector<quark::NodeId> preferred;
    for (quark::NodeId n : eligible)
        if (req.node_preferred(view.capabilities_of(n))) preferred.push_back(n);
    const std::span<const quark::NodeId> cand =
        preferred.empty() ? std::span<const quark::NodeId>(eligible)
                          : std::span<const quark::NodeId>(preferred);

    // (3) Delegate the pick to Quark's real Weighted-HRW (ADR-013). Capacity `weight` balances load;
    // the winner is a pure function of (ActorId, membership+caps) — coordinator-free, same on every node.
    const auto winner = quark::place_weighted(id.hash(), cand, view);
    if (!winner) return {false, {}, PlacementError::NoEligibleNode};
    return {true, *winner, PlacementError::None};
}

}  // namespace aero::cluster
