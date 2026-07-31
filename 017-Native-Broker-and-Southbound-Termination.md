# 017 — Native Broker and Southbound Termination

> Draft v0.2. AeroEdge provides its own embedded MQTT broker — a **fully-distributed plugin**
> behind the 014 transport seam (014 §4 B3), not a fork of an existing broker and not a
> dependency on one. It exists to (a) remove EMQX's Business Source License as a blocker to
> shipping AeroEdge as a product, and (b) terminate device-facing ("southbound") MQTT at the
> edge node itself instead of requiring separate broker infrastructure per deployment. The goal
> is EMQX **capability** parity for the pieces that matter to AeroEdge's deployments — not a
> line-for-line reimplementation of EMQX's own architecture, and explicitly not its licensed-away
> multi-node clustering (§3/§4 cover how AeroEdge gets that property for free instead).

## Status (updated as milestones ship — keep this current, don't let it drift like v0.1's §6 did)

| Milestone | What | Status |
|---|---|---|
| Phase 1 | Single-node MQTT 3.1.1 core: CONNECT/SUBSCRIBE/PUBLISH/PUBACK/PINGREQ/DISCONNECT, wildcard routing, retained messages, QoS 0/1 | **Shipped** |
| M1 | QoS 2, Last Will & Testament, keep-alive enforcement, persistent/clean sessions + takeover | **Shipped** |
| M2 | `Runtime::configure_broker()`, `GET /broker/status`, CLI flags — `aero-broker` now linked into `aero-runtime` | **Shipped** |
| M3 | Bridge seam (`IBridgeSink`: MQTT-to-MQTT, HTTP webhook) + rule engine reusing `ExprRuleNode` (008 §6) | **Shipped** |
| M4 | Studio dashboard page for the broker | **Shipped** |
| M5 | TLS + per-topic ACL for the native MQTT broker | **Shipped** — southbound (device-facing) TLS + ACL; cluster-link TLS deferred, M5.1 |
| M6 | Cross-node topic routing | **Shipped** — v1 broadcast fanout, not HRW-selective (see §4 correction below) |
| M7 | MQTT 5 | **Shipped** — protocol negotiation + CONNECT/Will/SUBSCRIBE/PUBLISH properties parsing + v5 CONNACK/SUBACK reason codes; feature properties (Topic Alias, Shared Subs, Request/Response, Enhanced Auth) deferred, M7.1 |
| M8 | Kafka/Pulsar/RabbitMQ bridges (needs new third-party deps, native-extension-shaped) | **Shipped** — RabbitMQ only; Kafka/Pulsar deferred, M8.1/M8.2 |
| M9 | Multi-protocol southbound (OPC-UA/Modbus) | **Superseded by spec 018** — its own spec, as this row anticipated |

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

**Correction from M6 (this section's original framing was wrong on one point).** The paragraph
above assumed `DistributedRouter::tell` could be pointed at whichever node HRW-owns a given
topic, the same way any other actor `tell` works. It cannot: `tell()` is placement-routed — it
computes the HRW owner itself and sends there — there is no way to address an explicit,
caller-chosen peer through `tell()`/`DistRef`. That rules out "ask Quark who owns this topic and
forward to just that node" as a mechanism, at least without a lower-level primitive this codebase
doesn't have today. M6 ships v1 with the simpler, coordinator-free alternative discussed as a
fallback in §4's fanout note: **every local PUBLISH is broadcast to every peer node** (a
hand-built `MessageFrame`, `TcpTransport::send()`'d directly to each peer — see
`broker_cluster.hpp`), and each peer does its own local wildcard match. `DistributedRouter` is
still used, but only for the *receive* side (peers `tell` a well-known `BrokerRelayActor`, which
is itself HRW-placed like any actor — the sender just also happens to be every node, since a
broadcast reaches all of them). Selective, ownership-aware forwarding (M6.1, Open Questions)
would need either a real per-topic subscription registry or a lower-level "send to this specific
`NodeId`" primitive in Quark; neither exists yet.

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

## 6. MQTT scope — capability parity where it matters, not a line-for-line EMQX clone

EMQX's BSL-restricted surface (from the survey: 30+ bridge connectors, a schema registry, a SQL
rule engine, multi-protocol gateways for CoAP/LwM2M/OCPP/etc., a dashboard) is real
functionality AeroEdge deployments need — the revised direction (see Status above) is to match
that *capability* by **reusing what AeroEdge already has** (the existing `ExprRuleNode`
expression engine for rules, `httplib`/`RestMesAdapter`'s pattern for HTTP bridging,
`MqttClientTransport`'s codec for MQTT bridging, Studio's existing page pattern for a
dashboard) rather than copying EMQX's own from-scratch implementation of each. What's still
explicitly declined is EMQX's *long tail* (Kafka/Pulsar/RabbitMQ bridges needing new
dependencies, MQTT 5, CoAP/LwM2M/OCPP gateways) — those are backlog milestones (M7-M9), not
principled exclusions the way v0.1 framed the whole breadth:

| Capability | Status | Rationale / where |
|---|---|---|
| MQTT 3.1.1: CONNECT/SUBSCRIBE/PUBLISH/PUBACK/PUBREC/PUBREL/PUBCOMP/PINGREQ/DISCONNECT | **shipped** | Phase 1 + M1 |
| QoS 0, 1, 2 | **shipped** | QoS 2 (PUBREC/PUBREL/PUBCOMP, §4.3.3) landed in M1 |
| Topic wildcards (`+`, `#`) on SUBSCRIBE | **shipped** | Phase 1 |
| Retained messages | **shipped** | Phase 1 — last-known-value fits Tag semantics (002/003) |
| Last Will & Testament | **shipped** | M1 |
| Keep-alive enforcement (1.5× timeout) | **shipped** | M1 |
| Persistent/clean sessions + takeover | **shipped**, in-memory, single-node | M1; cluster-wide session sync is M6 |
| Bridging (MQTT-to-MQTT, HTTP webhook) | **shipped** | M3, `IBridgeSink`/`MqttBridgeSink`/`HttpWebhookBridgeSink` |
| Rule engine (topic filter + expression gate → sink) | **shipped**, reuses `ExprRuleNode` | M3, not a new DSL |
| Dashboard | **shipped** | M4, `studio/src/pages/BrokerPage.tsx` |
| TLS | **shipped** | M5, `aero/pal/tls.hpp` (mbedTLS-backed `TlsServerContext`/`TlsSession`) — server-auth and optional mTLS (client-cert-required) southbound listener |
| Per-topic ACL / authorization | **shipped** | M5, `aero/broker/acl.hpp` (`Authorizer`/`TopicAclAuthorizer`) — broker-local seam, not literal Quark 020 reuse (N6 correction below) |
| Cross-node topic routing | **shipped**, v1 broadcast fanout (not HRW-selective) | M6, see §4 correction; `broker_cluster.hpp` |
| MQTT 5 | **shipped**, protocol negotiation + properties parsing + a v1 slice of feature properties | M7, `aero/transport/mqtt_codec.hpp`'s bounded Properties codec (`read_varint`/`read_properties`/`put_empty_properties`/`put_topic_alias_max_properties`) + `native_broker.hpp`'s `Session::protocol_version` branch in CONNECT/Will/PUBLISH/SUBSCRIBE parsing and CONNACK/SUBACK reason codes; M7.1 adds Session Expiry Interval TTL enforcement, server DISCONNECT reason codes (keep-alive timeout, session takeover), and inbound Topic Alias — outbound Topic Alias (compression), Shared Subs, Request/Response, User Properties end-to-end, Message Expiry/Max Packet Size enforcement, and Enhanced Auth remain deferred |
| RabbitMQ bridge | **shipped** | M8, `RabbitMqBridgeSink` (`broker/rabbitmq_bridge_sink.hpp`) over rabbitmq-c (AMQP 0-9-1); PUBLISH only, no TLS/SASL-EXTERNAL, no publisher confirms |
| Kafka/Pulsar bridges | backlog | M8.1/M8.2 — needs new third-party deps (librdkafka / pulsar-client-cpp), native-extension-shaped (008) |
| Multi-protocol gateways (CoAP/LwM2M/OCPP), OPC-UA/Modbus southbound | backlog, likely a separate spec | M9 |
| Shared subscriptions | not yet scheduled | no current AeroEdge/AeroMes use case; revisit on demand |

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

- **C5 (014), southbound leg — shipped, M5**: a second, TLS-speaking listener
  (`aero::pal::tls::TlsServerContext`/`TlsSession`, `aero/pal/tls.hpp`, mbedTLS-backed) runs
  alongside the plaintext one, additive not a replacement — `NativeBroker::Config::tls` (unset by
  default: no behavior change for a Config that doesn't opt in). Server-authenticated TLS is the
  common case (the device verifies the broker); setting `ServerConfig::ca_file` additionally
  requires and verifies a client certificate (mutual TLS) — a client presenting none, or one that
  doesn't chain to `ca_file`, is rejected at the handshake, never silently downgraded to
  plaintext-equivalent trust. **C5's cluster-link leg remains open** — see M5.1 in Open Questions.
- **CONNECT-time authentication — shipped, M5**: `NativeBroker::Config::authenticate`
  (`aero::broker::Authenticator`, `aero/broker/acl.hpp`) is a pluggable
  `(username, password) -> optional<principal>` callback consulted before any other CONNECT-time
  session-state mutation; `nullopt` rejects the CONNECT (CONNACK rc=0x04) with zero broker-state
  side effects, a returned principal becomes the session's identity for `authorizer` below. Unset
  by default — every CONNECT accepted, principal `"anonymous"`, matching pre-M5 behavior exactly.
- **Per-topic authorization — shipped, M5, but NOT literal Quark 020 principal/Authorizer reuse**:
  `NativeBroker::Config::authorizer` (`aero::broker::Authorizer`, concretely `TopicAclAuthorizer`
  or a custom implementation) gates PUBLISH/SUBSCRIBE per `(principal, topic, action)` at the
  CONNECT/SUBSCRIBE/PUBLISH boundary, before any topic state (retained store, session subscription
  list, `route_publish` fan-out) is touched — a denied request has zero side effects. This is a
  DELIBERATELY INDEPENDENT, purpose-built seam styled after Quark 020's boundary-enforcement/
  default-deny pattern (same shape: a virtual `allow()` interface + an ordered-rule-table
  implementation + an explicit default posture), not a reuse of `quark::Authorizer`/
  `quark::Principal` themselves — see the N6 correction below for why, and
  `aero/broker/acl.hpp`'s own banner comment for the canonical version of this reasoning (both
  places are meant to say the same thing).

## 9. Invariants (normative)

- **N1** — the native broker is an opt-in-to-*configure* plugin (`aero-broker`): `aero-runtime`
  links it and exposes `Runtime::configure_broker()` (M2), the same posture as `aero-mes`/
  `aero-ota` (daemon-lifetime subsystems, inert until explicitly configured) — **not** the
  `aero-transport` posture (excluded from `aero-runtime` entirely, swapped in per-deployment
  behind the Transport seam instead). `aero-core` still never depends on it. (Revises v0.1's
  wording, which conflated "opt-in" with "never linked" — see 014 B3's actual requirement:
  `aero-core`/`aero-runtime` never *require* it, which configure-gating satisfies without
  needing exclusion from the link graph.)
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
  ACL store. **CORRECTION (M5)**: the shipped ACL (`aero/broker/acl.hpp`) is a broker-LOCAL
  `Authorizer` seam styled after Quark 020's boundary-enforcement/default-deny pattern — NOT a
  literal reuse of `quark::Principal`/`quark::Authorizer`. Reason: Quark's `Authorizer::allow` is
  keyed on `(Principal, ActorId, TypeKey)` — actor-addressing types that exist because Quark
  routes messages between actors. An MQTT broker has no `ActorId`/`TypeKey` on its hot path: what
  it routes on is topic STRINGS with `+`/`#` wildcard semantics (§4.7 of the MQTT spec), which
  don't map onto Quark's addressing model at all. Bolting topic strings onto `ActorId`/`TypeKey`
  would either lose wildcard matching or force a fake actor-space just to satisfy a type signature
  that doesn't fit the problem — so this is a deliberately independent, purpose-built seam (same
  shape as Quark 020's — a boundary-enforcement virtual interface, a concrete ordered-rule-table
  implementation, an explicit default posture — just not the same classes). This paragraph and
  `aero/broker/acl.hpp`'s own banner comment are meant to state the same reasoning consistently;
  treat that file as canonical if the two ever drift.
- **N7** — QoS ≥ 1 delivery from device → AeroEdge is at-least-once with dedup at the
  consuming actor (mirrors 014's C3 posture for inter-actor MQTT); QoS 0 is an explicit,
  opt-in, lossy choice per topic.
- **N8** — QoS 2 is exactly-once *per session*: a message is only routed/delivered after PUBCOMP
  completes the handshake (§4.3.3), never on PUBREC alone; a retransmitted PUBLISH under the
  same packet id is deduped, not re-routed.
- **N9** — Last Will delivery is keyed on HOW a session ended: an ungraceful end (socket error/
  reset, or losing a session-takeover race) fires the will; a clean DISCONNECT never does
  (§3.14) — the broker must distinguish these, not treat every disconnect identically.
- **N10** — persistent-session state (subscriptions + queued QoS≥1 messages) is bounded (a
  capped, drop-oldest queue) and in-memory only — it does not survive a broker process restart
  in the current milestone (that would be a durability feature, tracked separately, not implied
  by "persistent" here).
- **N11** — bridge sinks (`IBridgeSink`) and the rule engine never block the broker's session
  threads on slow downstream I/O in a way that stalls PUBLISH processing for unrelated topics —
  each sink call is expected to be fire-and-forget or bounded, mirroring 012's outbox posture
  (M3 review should confirm this holds as sinks grow more sophisticated, e.g. Kafka in M8).
- **N12** — the rule engine reuses `ExprRuleNode`'s grammar/evaluator verbatim; it does not fork
  or extend the expression language for broker-specific needs — if broker rules eventually need
  something the flow-rule grammar can't express, that is a change to the shared grammar (008),
  not a second one.

## 10. Open questions

- **M6.1 — selective (HRW-owner) cross-node forwarding.** M6 ships broadcast fanout: every
  PUBLISH is relayed to every peer node regardless of whether that peer has a matching
  subscriber. Correct and coordinator-free, but O(nodes) messages per PUBLISH regardless of
  actual interest — fine at the node counts seen so far, not something to assume scales
  unbounded. A selective version needs either (a) a distributed per-topic subscription registry
  (the expensive machinery this spec explicitly avoided reimplementing in §4), or (b) a
  lower-level "send to this specific `NodeId`" primitive in Quark that `DistributedRouter::tell`
  doesn't provide today (see §4's correction). Revisit if/when node counts or PUBLISH volume
  make broadcast fanout a measured bottleneck, not preemptively.
- **Cross-node session continuity** — if a device reconnects to a *different* AeroEdge node
  (mobile gateway, failover), does its persistent session/retained state need to follow it?
  v1 treats sessions as local-node; revisit once a real multi-node southbound deployment
  exists.
- **AeroMes contract co-design** — the concrete `AeroMesAdapter` request/response shape,
  auth (beyond today's `X-Api-Key`), and replay protection (issue #454) need to be defined
  jointly with AeroMes, not inferred from its current webhook alone.
- **Retained-message durability** — whether a retained value survives a node restart (ties to
  007 State/Persistence) or is memory-only per session in v1.
- **M7.1 — MQTT 5 feature properties.** M7 ships protocol negotiation (CONNECT protocol-level
  0x04/0x05, reject-if-neither with a 3.1.1-shaped CONNACK) and Properties PARSING for every
  packet type that carries them in this broker's supported set (CONNECT, CONNECT Will, PUBLISH,
  SUBSCRIBE) via a bounded, skip-unknown codec (`mqtt_codec.hpp`'s `read_properties`), plus the
  v5-shaped CONNACK/SUBACK reason codes (0x00/0x84/0x86 CONNACK; 0x87 SUBACK).

  **Shipped this pass (v1 slice):**
  - **Session Expiry Interval TTL enforcement** — `Session::session_expiry_interval` (parsed from
    CONNECT Properties) is now stored, and `StoredSession::expires_at` (set in `teardown_session`
    when that property was present) is checked when a persistent session's owner reconnects:
    an elapsed TTL discards the stored subs/queued messages instead of restoring them
    (`session_present` stays 0). A v4 session, or a v5 session that never sent the property,
    behaves exactly as before — `expires_at` stays `nullopt`, meaning "never expires".
  - **Server-initiated DISCONNECT with a reason code** — `NativeBroker::send_disconnect()` sends a
    v5-only `0xE0` before the socket closes, for the two cases that actually need one: keep-alive
    timeout (reason `0x8D`) and session takeover (reason `0x8E`, MQTT 3.1.4). (**Correction from
    the prior version of this bullet:** it previously named protocol-version mismatch and auth
    failure as the motivating cases for this feature — that was wrong. Both of those happen
    *before* a successful CONNACK, and MQTT 5 §3.14 forbids the server from sending DISCONNECT
    before CONNACK succeeds; they correctly carry their reason in the CONNACK's own reason code
    instead, unchanged by this pass.)
  - **Inbound (client→broker) Topic Alias** — a v5 PUBLISH may establish an alias (topic name +
    Topic Alias property together) or reuse one (empty topic name + Topic Alias only);
    `NativeBroker` resolves it per-session (`Session::topic_aliases`) *before* the ACL gate, so an
    alias can't be used to bypass per-topic authorization. The broker advertises Topic Alias
    Maximum = 16 (`kTopicAliasMax`) in CONNACK; alias 0, or a value above the max, or an
    unestablished alias, gets `0xE0`/`0x94` (Topic Alias invalid) instead of being silently
    dropped or misrouted.

  **Still deferred** — none of it is acted on yet even though the bytes are correctly
  parsed-and-skipped or not yet parsed at all:
  - **Topic Alias** (compression, i.e. the *outbound* broker→subscriber direction) — the broker
    never assigns/uses an alias of its own when delivering a PUBLISH; `publish_to()`'s wire shape
    is unchanged.
  - **Shared Subscriptions** (`$share/...`) — no special-cased SUBSCRIBE filter handling.
  - **Request/Response pattern** — Response Topic (0x08) and Correlation Data (0x09) are parsed-
    and-skipped, never actually round-tripped to a responder.
  - **Enhanced/SASL Authentication** — the `0xF0` AUTH packet is not implemented; Authentication
    Method/Data (0x15/0x16) are parsed-and-skipped on CONNECT.
  - **User Properties end-to-end** — parsed-and-skipped everywhere; not surfaced through
    `NativeBroker::on_publish()`'s callback (would change that public signature).
  - **Message Expiry Interval / Maximum Packet Size enforcement** — parsed-and-skipped, no TTL or
    size-limit behavior wired to either.
  Revisit any of these once a real device/integration actually needs it — same "don't build
  ahead of demand" posture M7's own predecessor entry held, now narrowed to what's left.
- **M8.1/M8.2 — Kafka and Pulsar bridges.** M8 ships `RabbitMqBridgeSink` (rabbitmq-c, AMQP 0-9-1)
  as the one bridge in the Kafka/Pulsar/RabbitMQ family this milestone actually vendors — RabbitMQ
  was picked first because it needed no new build-system machinery beyond the FetchContent pattern
  mbedTLS/open62541 already established (plain C, no codegen step, MIT-licensed, no bundled
  copyleft deps). Kafka (`librdkafka`) and Pulsar (`pulsar-client-cpp`) remain explicitly deferred
  as separate future milestones, not folded into M8: both are materially heavier dependencies
  (librdkafka alone is a much larger C library with its own vendored zstd/lz4/ssl knobs; pulsar-
  client-cpp pulls in Boost + Protobuf) that deserve their own vendoring review rather than being
  rushed in alongside RabbitMQ's comparatively small footprint. Revisit each independently once a
  real deployment actually needs that specific broker, same "don't build ahead of demand" posture
  M7.1's own entry holds.
- **Shared secrets / device provisioning** — how a device gets its TLS identity/PSK in the
  first place (fleet-level concern, may tie to 011 OTA's existing device identity story).
- **Southbound OPC-UA/Modbus termination** — this spec is MQTT-only; the same "AeroEdge
  terminates the protocol locally instead of requiring external infra" posture now extends to
  OPC-UA/Modbus via **spec 018** (M9), as this entry anticipated.
- **M5.1 — cluster-link TLS** — M5 ships TLS for the southbound (device-facing) leg only
  (`aero/pal/tls.hpp`'s `TlsServerContext`, wired into `NativeBroker`). The broker's inter-node
  link, if/when cross-node topic routing ships (M6, `DistributedRouter`, §4), is a separate leg
  C5 (014) also covers and is NOT yet TLS-secured — deferred, blocked on Quark 020 shipping a
  real (non-mock) `Aead` cipher for its own `SecureTransport` seam. This is upstream QuarkCpp
  work, not something this project builds itself; revisit once that lands.
