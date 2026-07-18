# 010 — Distribution and Horizontal Scale

> Draft v0.1. How AeroEdge scales across many nodes. As with the actor runtime, the load
> is carried by QuarkCpp's distribution stack — AeroEdge adds a device-placement and
> topology layer on top, not a new clustering protocol.

## 1. Principle

Horizontal scale is a **first-class requirement**: an AeroEdge deployment is a *cluster of
edge nodes*, and device actors are distributed and rebalanced across them. AeroEdge does
**not** build clustering — Quark already proved it:

| Need | QuarkCpp mechanism | Spec / ADR |
|---|---|---|
| Place an actor on a node | Weighted-HRW / VirtualBins O(1) placement | 010, 025, 026, ADR-013 |
| Membership / failure detection | In-house SWIM gossip | 010 |
| Cluster formation, seeds, dial dedup | Trust-root bootstrap + discovery | 021 |
| Elastic scale up/down, drain, hand-off | Fenced hand-off + rolling upgrade | 021 |
| Route a message to a remote actor | Transport + DHT-relay (≤⌈log₂N⌉ hops) | 010, 026, ADR-011 |
| FIFO preserved across a path change | Path-pinning + drain-boundary promotion | ADR-011 |
| 10³–10⁴ node topology | Bounded partial-view + VirtualBins | 026 |
| Stateless worker pools (fan-out compute) | `Stateless<N>` pool, exactly-once | 025, ADR-011 |

**AeroEdge's job** is to decide *what* to place and *where it should prefer to live*, then
express that as Quark placement policy. It never re-implements HRW, SWIM, or the relay.

## 2. The AeroEdge placement model

### 2.1 Device affinity

An `EdgeActor` manages a physical device reachable over a specific network. Placement is
not arbitrary — the actor should run on the node with **network line-of-sight** to its
device (same VLAN / gateway / plant segment). AeroEdge expresses this with Quark's
**capability/affinity placement modifiers** (025):

- each node advertises **capabilities** (e.g. `plant=hanoi`, `segment=line-3`,
  `has=opcua-gateway`);
- an `EdgeActor` declares a **required/preferred capability** derived from its device
  config;
- Quark's Weighted-HRW placement (ADR-013) picks among the eligible nodes, balancing load.

### 2.2 Actor tiers and how they scale

| Actor | Cardinality | Placement intent | Scales by |
|---|---|---|---|
| `EdgeActor` | one per device (many) | pinned near its device (capability affinity) | adding edge nodes with the right capability |
| `LineActor` | one per line (moderate) | co-located with most of its EdgeActors | HRW, rebalanced on membership change |
| Egress/compute workers | elastic | anywhere with spare capacity | Quark `Stateless<N>` pool (025) |

Heavy, stateless processing (FFT, vision inference, batch transforms) is a natural fit for
a Quark **stateless worker pool**: the flow's Output/compute step `tell`s a pooled worker
type; Quark distributes and load-balances instances across the cluster with exactly-once
delivery (ADR-011).

## 3. Rebalancing and failure

When a node joins/leaves (SWIM detects it, 021):

- Quark recomputes placement; affected device actors migrate with **fenced hand-off** (021)
  so no two nodes ever drive the same device concurrently — critical for industrial safety.
- In-flight Commands preserve per-`(sender, actor)` FIFO across the migration (ADR-011).
- AeroEdge adds one rule: **a device actor must re-establish its driver connection** on the
  new node before resuming its flows. This is a lifecycle hook (`on_activate` re-dials the
  driver), not new distribution machinery. See 006 (Drivers).

## 4. What AeroEdge must NOT do here

- No custom gossip, heartbeat, or membership — use SWIM (021).
- No custom hash ring — use Weighted-HRW / VirtualBins (ADR-013, 026).
- No custom cross-node RPC — use Quark transport + relay (010, 026).

If a requirement seems to need one of these, push it into Quark as a placement *policy*
(capabilities, weights, affinity), not a fork.

**Transport pluggability** (Local/TCP/MQTT/gRPC to reach remote actors) is its own concern —
adapters behind Quark's `Transport` seam, never a parallel interface, with brokers integrated as
clients rather than reimplemented. See [014](014-Transport-Interface-and-Pluggable-Transports.md).

## 5. Open questions

- **Device→capability mapping source of truth** — where the `plant/segment/gateway` tags
  come from (device registry? config file? MES?). Ties to 011 (OTA/device management) and
  012 (MES hook).
- **Split-brain safety for actuators** — fenced hand-off (021) prevents dual activation, but
  actuator commands may need an extra lease/quorum guard beyond Quark's default; evaluate
  against Quark 020 (security) + 017 (delivery) before enabling write-capable drivers in a
  multi-node deployment.
- **Cross-plant vs single-plant topology** — whether one AeroEdge cluster spans plants
  (WAN, 026 bounded partial-view) or one cluster per plant with federation; deployment
  decision, deferred to 009.
