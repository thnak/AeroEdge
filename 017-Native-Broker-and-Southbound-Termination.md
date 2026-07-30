# 017 — Native Broker and Southbound Termination

> Draft v0.1. AeroEdge provides its own embedded MQTT broker — a **fully-distributed plugin**
> behind the 014 transport seam (014 §4 B3), not a fork of an existing broker and not a
> dependency on one. It exists to (a) remove EMQX's Business Source License as a blocker to
> shipping AeroEdge as a product, and (b) terminate device-facing ("southbound") MQTT at the
> edge node itself instead of requiring separate broker infrastructure per deployment.

## 1. Why

**EMQX's license changed under us.** As of v5.9.0, EMQX moved from Apache 2.0 to the
[Business Source License 1.1](https://github.com/emqx/emqx/blob/master/LICENSE) and merged
its former Community/Enterprise split into one BSL-licensed codebase — there is no longer a
permissively-licensed edition. The Additional Use Grant permits production use of **a single
node only**, and explicitly excludes "offering the Licensed Work to third parties on a hosted
or embedded basis" — defined to include "integrating it into a product or solution offered to
third parties." EMQX's own README is direct about the practical consequence: **"deploying an
EMQX cluster (more than 1 node) requires a license file"** from EMQ. AeroEdge is exactly the
case BSL 1.1 excludes: a product, embedded at customer sites, that needs more than one node
for the same reason AeroEdge exists at all (010 — horizontal scale, no SPOF). Depending on
EMQX for anything beyond a single throwaway node means either paying EMQ per deployment or
being out of compliance. **We need a broker we own the license to.**

**AeroMes has nowhere to send device data today.** AeroMes (the sibling in-house MES) made a
deliberate, documented decision to *not* terminate MQTT/OPC-UA/Modbus itself — the adapters
were built and then removed (`docs/iot-adapters.md`, 2026-06-25, "Scope decision —
Webhook-only ingestion") because running always-on device protocol clients inside a web API
tier was the wrong shape. AeroMes's stated design is that **an external edge gateway** owns
device protocol termination and pushes normalized signals in. That is exactly AeroEdge's job
(006, 012) — but today AeroEdge has no way to *receive* an MQTT PUBLISH from a device unless
some other broker already exists to relay it (006's MQTT driver is a **client**, per §3
below). AeroMes's current webhook shape (`POST /api/v1/iot/ingest`) is a useful signal of
what's needed, not a frozen target — **AeroMes is unfinished, and the concrete integration
contract between the two projects is this project's to define, jointly, as both evolve** (see
§7 and Open Questions), not an external API AeroEdge adapts itself to.

## 2. Three distinct MQTT roles — do not conflate them

AeroEdge already touches MQTT in two places; this spec adds a third. Keeping them distinct
matters because only one of the three is new:

| # | Role | AeroEdge is the... | Spec | Status |
|---|---|---|---|---|
| 1 | Inter-actor transport over an existing broker | MQTT **client** (both ends) | 014 §5, `MqttClientTransport` | shipped |
| 2 | Device ingestion from an external broker | MQTT **client** (subscriber) | 006 §9 (built-in MQTT driver) | v0.1 target |
| 3 | **Southbound termination — devices publish directly to AeroEdge** | MQTT **broker** (server) | **this spec** | new |

Role 3 is what removes the external-broker dependency for roles 1 and 2 in the common
deployment (a plant with no existing MQTT infrastructure): devices dial an AeroEdge node
directly; AeroEdge terminates the session, and (§6) feeds the data into the same places role 2
would have. Roles 1 and 2 keep working unmodified against an external broker for sites that
already run one (BSL-compliant single-node EMQX, Mosquitto, HiveMQ, a cloud broker) — this
spec does not remove that option, it removes the *requirement*.

## 3. Reaffirming 014's broker posture (B1–B3)

This spec is the concrete instance of [014 §4](014-Transport-Interface-and-Pluggable-Transports.md#4-broker-posture--the-explicit-constraints)'s
open question ("Embedded distributed broker... Deferred until a deployment actually forbids
external broker infra" — §11). BSL 1.1 is exactly that trigger. The native broker must still
hold every 014 constraint:

- **B1 (unaffected)** — `MqttClientTransport`/the MQTT driver remain clients to *any* broker,
  including this one. Nothing about being a client changes.
- **B2 (unaffected)** — the native broker is never required. Quark's default TCP fabric still
  needs no broker; a deployment with no southbound MQTT devices links none of this.
- **B3 (this spec's contract)** — the native broker is a **plugin** behind the transport seam
  (aero-core/aero-runtime never depend on it, §5), and it must be **coordinator-free /
  peer-to-peer with no single point of failure**. §4 explains how it gets this for free
  instead of building it.

## 4. Architecture — no new distributed system

A from-scratch distributed broker means reimplementing what EMQX's `ekka`/`mria` (Mnesia) and
newer Raft-based durable storage do — exactly the expensive, hard-to-get-right machinery
that's licensed away in EMQX's Enterprise tier, and exactly what AeroEdge/Quark do not need to
rebuild:

- **Quark already has coordinator-free cluster membership and placement** — SWIM gossip + HRW
  rendezvous placement + DHT-relay for nodes that share no direct link (010 §3,
  [ADR-006/026](../QuarkCpp/decisions/ADR-006-large-scale-cluster-topology.md) at the Quark
  layer). This is the same machinery every distributed AeroEdge actor already rides.
- **A topic subscription is placement, not a new concept.** Each MQTT topic (or topic's owning
  actor — the natural unit is a `Tag`/`EdgeActor`, 001/005) is HRW-placed on exactly one node
  today, the same as any other actor. A device's PUBLISH on topic `T` needs to reach whichever
  node owns `T` — that is **the exact routing problem Quark's DistributedRouter already
  solves** for a `tell` (010, 014 §8). The native broker does not gossip its own topic table;
  it asks Quark "who owns this" and forwards, or handles it locally if it already owns it.
- **No SPOF follows by construction, not by engineering it separately.** Because topic
  ownership *is* actor placement, node failure/rebalancing is handled exactly as 010 §3
  already specifies for any actor — nothing broker-specific to fail over.
- **What IS new and local to each node:** the MQTT wire protocol termination itself — CONNECT/
  SUBSCRIBE/PUBLISH/PUBACK/PINGREQ/DISCONNECT parsing and session state for locally-dialed
  device connections. This is a **local, per-node, stateless-between-nodes** concern — no
  cluster-wide session table, no Mnesia-equivalent. A device that reconnects to a different
  node re-subscribes; MQTT's own semantics (clean/persistent session, QoS redelivery) are
  honored per-connection, not synchronized cluster-wide in v1 (see Open Questions).
- **Reuse, don't rewrite, the wire codec.** `mqtt_client_transport.hpp` (014, cross-platform as
  of the recent PAL work) already hand-rolls a correct MQTT 3.1.1 codec — fixed header,
  remaining-length varint, CONNECT/SUBSCRIBE/PUBLISH/PUBACK framing (`Packet`, `read_packet`/
  `write_packet`, the `put_*`/varint helpers). The broker's server-side state machine (accept
  CONNECT → CONNACK, accept SUBSCRIBE → SUBACK + register interest, PUBLISH → route/fan-out,
  PUBACK on QoS 1) is new; the byte-level framing is not — factor the codec into a shared
  header both the client and broker link, rather than a second copy.

## 5. Where it attaches (seam, not a fork of anything)

```text
   Device (PLC/sensor)                    AeroEdge node                      other AeroEdge nodes
  ┌──────────────────┐   MQTT/TCP    ┌───────────────────────┐   tell (existing     ┌──────────────┐
  │  publishes to a   │──────────────▶│ NativeBroker (new,     │   DistributedRouter, │  owns the    │
  │  topic (no broker │               │  aero-broker, plugin)  │──10/014 routing)────▶│  topic's Tag/│
  │  elsewhere needed)│               │  MQTT server session   │                      │  EdgeActor   │
  └──────────────────┘               └───────────┬────────────┘                      └──────────────┘
                                                    │ local topic → normalize as a
                                                    │ Driver/Source frame (006 pattern,
                                                    │ server-side instead of client-side)
                                                    ▼
                                          local Flow ingestion (004) → MesReportNode (012)
                                          → MesGateway → IMesAdapter → MES (outbox, at-least-once)
```

- **Layering (CONVENTIONS.md).** `aero-broker` is a new optional library, same tier as
  `aero-transport`: it depends on `aero-core`/Quark's router, nothing depends on it. Linked
  only when a deployment enables southbound MQTT termination (B3, opt-in).
- **Device data enters exactly like 006's MQTT driver today**, except the driver is now
  server-side (accepting the connection) instead of client-side (dialing out to a broker). The
  `IDriver`/Source-node contract (006 §5, §8) is unchanged — a NativeBroker session that owns
  a topic hands frames to a `Frame`-producing Source exactly as a polled/pushed driver does
  today. This is additive to 006, not a new ingestion mechanism.
- **Device data leaves toward the MES exactly through 012** — `MesReportNode` stages a report,
  `MesGateway` drains the durable outbox to whatever `IMesAdapter` is configured
  (`RestMesAdapter` today). This spec adds no second path to the MES; it is one more producer
  into the same seam 012 already defines (M1–M3 unaffected).

## 6. MQTT scope (v1) — the floor, not EMQX's ceiling

EMQX's BSL-restricted surface (from the survey: 30+ bridge connectors, schema registry, SQL
rule engine, multi-protocol gateways for CoAP/LwM2M/OCPP/etc., a dashboard) is enterprise
integration breadth AeroEdge does not need to match — that breadth is exactly what the license
is gatekeeping, and duplicating it would recreate the problem this spec exists to avoid. The
native broker's floor is the one industrial edge actually needs:

| Capability | v1 | Rationale |
|---|---|---|
| MQTT 3.1.1: CONNECT/SUBSCRIBE/PUBLISH/PUBACK/PINGREQ/DISCONNECT | yes | matches the existing client codec (§4); covers the vast majority of PLC/sensor MQTT stacks |
| QoS 0, QoS 1 | yes | QoS 1 is AeroEdge's own floor for at-least-once (014 §5); QoS 0 for genuinely lossy telemetry |
| QoS 2 | no (v1) | rare in device firmware; revisit if a real device needs it |
| Topic wildcards (`+`, `#`) on SUBSCRIBE | yes | needed for any non-trivial topic tree |
| Retained messages | yes | last-known-value is a natural fit for Tag semantics (002/003) |
| Persistent sessions across reconnect | local-node only, v1 | see §4 — no cluster-wide session sync yet |
| TLS | yes | C5 (014) — required for any non-loopback deployment |
| Per-topic ACL / authorization | via Quark 020 principal, not a bespoke ACL engine | reuse, don't rebuild (thin-over-Quark) |
| MQTT 5 | no (v1) | 3.1.1 covers the device population we've seen; revisit on real demand |
| Bridging to Kafka/other brokers, rule engine, dashboard | out of scope | EMQX's own paid-tier breadth; not this spec's problem to solve |
| Shared subscriptions, multi-protocol gateways (CoAP/LwM2M/OCPP) | out of scope (v1) | no current AeroEdge/AeroMes use case |

## 7. MES integration — defined here, not borrowed from AeroMes as-is

AeroMes's present `POST /api/v1/iot/ingest` (machineCode/tagKey/value/unit/timestamp,
`X-Api-Key`) is real, current, and the closest thing to a target contract that exists — but
AeroMes is an actively-changing, incomplete project, not a stable external system. Per the
project's direction: **this spec (and 012) define the canonical shape AeroEdge reports in
(`MesReport`, 012 §3); the AeroMes-side ingestion contract is expected to be co-designed
against that canonical shape as both projects move**, not treated as a fixed target AeroEdge
adapts to unilaterally. Concretely:

- The native broker never talks to AeroMes directly. It normalizes device PUBLISHes into
  AeroEdge's own Tag/Frame model (§5) and, where a report is MES-relevant, stages a canonical
  `MesReport` (012 §2.1) the same way any other flow does.
- A dedicated `AeroMesAdapter` (implementing `IMesAdapter`, 012 §5's adapter table) is the
  right place for whatever AeroMes's ingestion contract turns out to be — HTTP/webhook today,
  something else later — kept out of `aero-core`/the broker entirely (M1: swap MES, flows
  unchanged).
- Today's AeroMes webhook lacks HMAC/replay protection (AeroMes issue #454) — the adapter must
  not assume the far side is safe against replay; the outbox's own idempotency key (012 §3)
  covers AeroEdge's side, but transport-level integrity toward AeroMes is a joint open item.

## 8. Security

- **C5 (014) applies unchanged**: southbound connections should be TLS where the link isn't
  physically trusted loopback/plant-local; device certs or PSK, not a shared broker password.
- **Authorization is Quark 020 principal propagation**, not a broker-native ACL table — a
  device's MQTT session maps to a Principal, and topic-level publish/subscribe rights are
  checked the same way any other actor-to-actor authorization is (mirrors 014 §5's "broker is
  untrusted for authz" stance, now applied to *this* broker too: it terminates transport
  security, it does not become a second source of truth for authorization).

## 9. Invariants (normative)

- **N1** — the native broker is an opt-in plugin (`aero-broker`); `aero-core`/`aero-runtime`
  never depend on it (014 B3).
- **N2** — no cluster-wide broker state (session table, topic tree) is invented; topic
  ownership and cross-node delivery ride Quark's existing coordinator-free placement/routing
  (010, 026) unchanged.
- **N3** — the wire codec (packet framing) is shared with `MqttClientTransport`, not
  duplicated; a fix to one applies to both.
- **N4** — device data enters AeroEdge through the same Driver/Source contract as any other
  ingestion path (006 D1–D6); the broker is a transport for existing Sources, not a new
  ingestion primitive.
- **N5** — MES reporting rides 012 unchanged (M1–M3); the broker never talks to an MES
  directly.
- **N6** — authorization is Quark 020 principal propagation; the broker holds no independent
  ACL store.
- **N7** — QoS ≥ 1 delivery from device → AeroEdge is at-least-once with dedup at the
  consuming actor (mirrors 014's C3 posture for inter-actor MQTT); QoS 0 is an explicit,
  opt-in, lossy choice per topic.

## 10. Open questions

- **Cross-node session continuity** — if a device reconnects to a *different* AeroEdge node
  (mobile gateway, failover), does its persistent session/retained state need to follow it?
  v1 treats sessions as local-node; revisit once a real multi-node southbound deployment
  exists.
- **AeroMes contract co-design** — the concrete `AeroMesAdapter` request/response shape,
  auth (beyond today's `X-Api-Key`), and replay protection (issue #454) need to be defined
  jointly with AeroMes, not inferred from its current webhook alone.
- **Retained-message durability** — whether a retained value survives a node restart (ties to
  007 State/Persistence) or is memory-only per session in v1.
- **QoS 2 and MQTT 5** — deferred until a real device/integration needs them; don't build
  ahead of demand.
- **Shared secrets / device provisioning** — how a device gets its TLS identity/PSK in the
  first place (fleet-level concern, may tie to 011 OTA's existing device identity story).
- **Southbound OPC-UA/Modbus termination** — this spec is MQTT-only; whether the same
  "AeroEdge terminates the protocol locally instead of requiring external infra" posture
  extends to OPC-UA/Modbus (AeroMes descoped those too) is a separate, later spec.
