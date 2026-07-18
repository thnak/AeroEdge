# 014 — Transport Interface and Pluggable Transports

> Draft v0.1. Multiple transport types (Local, TCP, MQTT, gRPC) for reaching remote actors —
> without replacing brokers or building one into the core. The load-bearing decision: this is
> **not a new interface**; it is adapters behind QuarkCpp's existing `Transport` seam, and every
> adapter must honor that seam's contract (above all per-sender FIFO) or the cluster correctness
> proofs break.

## 1. First, the distinction that prevents a design error

"Transport" means two different things in an edge platform. Conflating them is the trap:

| | **(A) Inter-actor transport** | **(B) Device / integration transport** |
|---|---|---|
| Moves | actor→actor messages across nodes | device data in, external-system data in/out |
| Owner | QuarkCpp `Transport` seam (010) | AeroEdge Drivers (006) + MES adapters (012) |
| Guarantees | per-`(from,target)` FIFO, delivery guarantees (017), deadline/trace/principal propagation | protocol-native; framed into the flow |
| Examples | reach a `LineActor` on another node | an MQTT **driver** ingesting sensor topics; a `BusMesAdapter` |

**The diagram in this request is (A)** — Actor Runtime → Transport Interface → {Local, MQTT,
gRPC} → Remote Actor. This spec is about (A). MQTT appears in **both** columns: as an
actor-transport *tunnel* here, and as a device *driver* in 006 — same broker possibly, different
topics, different purpose. Do not merge them.

## 2. The decision: adapters behind Quark's `Transport` seam — not a new one

Quark 010 already defines the exact interface this diagram asks for:

```cpp
// Quark's existing seam (transport.hpp) — AeroEdge does NOT redefine this.
struct Transport {
    void send(NodeId to, MessageFrame frame);     // fire-and-forget byte move
    void on_receive(/* sink */);                   // inbound frames → DistributedRouter re-post
};
// MessageFrame = { from, to, target ActorId, msg_type TypeKey, WireMode, payload, deadline, trace }
```

The `DistributedRouter` (010) routes placement-aware over three seams — `Membership`, placement,
`Transport` — and the **default** `Transport` is plain TCP + length-prefixed frames; gRPC/QUIC/RDMA
are already named as optional adapters behind it. **AeroEdge's "Transport Interface" IS this
seam.** Our job is to supply more adapters, not a parallel abstraction:

```text
              Quark DistributedRouter (placement + membership + delivery)
                                   │
                          Transport seam (010)          ← the "Transport Interface"
        ┌──────────────┬──────────────┬──────────────┬──────────────┐
   Local/Loopback   TCP (Quark      MqttTransport   GrpcTransport   (QUIC/RDMA…)
   (same node)      default)        (AeroEdge)      (AeroEdge)      (Quark/optional)
        └──────────────┴──────────────┴──────────────┴──────────────┘
                                   │
                            Remote Actor
```

- **Local Queue** = same-node delivery through Quark's `LocalRouter` — *no transport at all*, no
  serialization (the fast path). The loopback transport is its in-process test/relay double.
- **TCP** = Quark's coordinator-free default (SWIM membership + HRW placement + DHT-relay, 026).
  This is already a **distributed, brokerless** actor fabric — see §4.
- **MQTT / gRPC** = AeroEdge adapters implementing `Transport::send`/`on_receive` (§5, §6).

## 3. The Transport contract every adapter MUST honor (non-negotiable)

An adapter is free to move bytes however it likes, but it must present the seam's guarantees to
the router above it, or Quark's proofs (ADR-011 FIFO-under-relay, 017 delivery) are void:

- **C1 — per-`(from → target)` FIFO.** Frames from one sender to one destination arrive in send
  order. This is *the* property ADR-011 depends on. A transport whose substrate can reorder
  (MQTT across reconnect, any multi-path fabric) **must add a sequence number + resequencing
  shim** to restore FIFO before handing frames up. Non-negotiable.
- **C2 — frame integrity + header fidelity.** The full `MessageFrame` header survives: `target`
  ActorId, `msg_type` TypeKey, negotiated `WireMode` (016), propagated deadline (018), trace
  (009), and Principal (020). The adapter is a byte pipe for the payload and a faithful carrier
  for the header — it never reinterprets either.
- **C3 — delivery class respected.** Whatever Quark 017 asks for (at-most / at-least / effectively
  once) the adapter must not silently weaken. At-least-once + Quark's idempotency/dedup (017) is
  the workable target for broker transports; at-most-once (lossy) is only legal where 017 permits.
- **C4 — no lifecycle/ordering opinions.** Connection dial/dedup/keepalive/reconnect is Quark 021;
  the adapter plugs into that, it does not invent its own membership or placement.
- **C5 — security in-band.** node↔node authentication/encryption is Quark 020 (secure transport);
  a broker transport rides broker TLS **plus** Quark's principal propagation — the broker is not
  trusted to authorize actor messages.

If an adapter cannot meet C1–C3 for a given link, that link is **not** eligible for inter-actor
transport (it may still be fine as a device/integration transport, column B).

## 4. Broker posture — the explicit constraints

Three rules, matching the request ("support multiple transports, don't replace brokers, embedded
broker must be plugin + fully distributed"):

- **B1 — never replace or reimplement a broker.** The `MqttTransport` (and any AMQP/Kafka variant)
  is a **client** to an existing broker. We use the broker's strengths (fan-out, store-and-forward,
  NAT traversal) and add only the C1–C3 shims Quark needs on top. "Let them do their best."
- **B2 — the core needs no broker at all.** Quark's default TCP fabric is already **coordinator-free
  and distributed** (SWIM + HRW + DHT-relay, 026) — no broker, no central node, no SPOF. So a broker
  is a *connectivity/integration choice* (plant networks that only permit broker access, existing
  MQTT infrastructure), never a correctness dependency. The default build links no broker.
- **B3 — an embedded broker is a plugin, and must be fully distributed.** If a deployment wants
  broker semantics without external infrastructure, an embedded broker may be provided **only** as
  a plugin behind the transport seam, and it must be **coordinator-free / peer-to-peer with no
  single point of failure** (consistent with Quark's no-external-coordinator stance, 026). Even
  then, `aero-core`/`aero-runtime` never depend on it — it is an opt-in adapter like any other.

> Net: brokers are integrated, not embedded-by-default and never reinvented. The one embedded case
> is a distributed plugin, isolated behind the same seam.

## 5. `MqttTransport` — actor frames over an existing broker

For links where nodes cannot dial each other directly (NAT, plant firewalls, hub-and-spoke plant
networks) but can all reach a broker, actor frames tunnel through MQTT:

- **Topic scheme.** One inbound topic per destination node, e.g. `aero/xport/<toNodeId>`; the
  `MessageFrame` header carries the `target` ActorId, so one node-inbox topic suffices and keeps
  per-publisher ordering to that topic. A node subscribes to its own inbox topic.
- **QoS.** **QoS ≥ 1** always (QoS 0 is at-most-once → violates C3 for at-least-once actors). QoS 1
  duplicates are absorbed by Quark 017 idempotency/dedup; QoS 2 where the broker supports it and
  exactly-once is required.
- **Ordering shim (C1).** MQTT preserves order per-topic per-session but **not** guaranteed across a
  reconnect / clean-session. The adapter therefore stamps a per-`(from→to)` sequence number and
  **resequences** on receive, dropping duplicates — restoring the FIFO Quark demands. This shim is
  mandatory, not optional.
- **The broker is untrusted for authz.** TLS to the broker (C5) protects the hop; actor-level
  authorization is still Quark 020 principal propagation carried in the frame — a compromised broker
  can delay/replay (mitigated by seq + dedup) but cannot forge an authorized actor message.
- **Latency posture.** A broker hop is slower than direct TCP. MQTT transport is for **reachability
  and integration**, not the low-latency hot path. Latency-sensitive intra-plant links stay on TCP.

## 6. `GrpcTransport` — typed streaming over HTTP/2

For firewall-friendly, service-mesh, or cross-datacenter links:

- A bidirectional gRPC stream per peer carries `MessageFrame`s; HTTP/2 stream ordering gives C1 on a
  single stream (one stream per peer, mirroring Quark's one-connection-per-peer TCP model).
- mTLS satisfies C5; deadlines map naturally to gRPC deadlines (C2/018).
- Good where infrastructure already speaks gRPC or where HTTP/2 traverses firewalls that block raw
  TCP. Heavier than Quark's default TCP; chosen per deployment, not by default.

## 7. Streaming across transports — where end-to-end backpressure survives

Quark streams data across nodes (024), and it rides **this same pluggable seam**: the Transport
carries stream frames, and the destination actor ingests them through a local SPSC credit-ring
with **FIFO-per-stream preserved end-to-end** and **credit-based backpressure (producer stall,
not shedding — 022)**. The API even takes the endpoint explicitly:
`open_stream<F>(actor_id, transport_ep)`. A stream is a *distinct sender* from control-plane
`tell`s (no mutual global order), but each stream is FIFO end-to-end.

But backpressure is only **end-to-end** when the transport is **flow-controlled and
point-to-point**. A broker decouples producer from consumer and breaks the credit loop:

| Transport | Cross-node stream | Backpressure reaches the *original* producer? |
|---|---|---|
| Local | n/a (same node) | yes — direct credit loop |
| TCP (Quark default) | yes | **yes** — no credit → stop reading the socket → TCP flow control stalls the remote sender |
| gRPC (HTTP/2) | yes | **yes** — HTTP/2 / app-level stream credit stalls the sender |
| **MQTT (broker)** | yes, but | **no** — credit stalls at the *broker*, not the device/actor that produced the frames; the broker then buffers or drops per its own QoS/retention |

Consequence for AeroEdge:

- **High-rate streams that scale across nodes** (edge→gateway/cloud aggregation of normalized
  samples) → carry over a **flow-controlled transport (TCP/gRPC)** to keep Quark's end-to-end
  FIFO + stall-not-drop. This is the load-bearing reason a plant runs TCP internally.
- **MQTT is right for discrete control-plane messages** (with the §5 resequencing shim) and for
  device/integration (006/012), but is a **poor carrier for a backpressured stream**: over a
  broker you get store-and-forward/QoS, not producer stall. If a stream *must* cross an
  MQTT-only link, treat the **broker as the backpressure boundary** and size/shed explicitly
  there — never assume the device throttles.

This is another reason transport is a **per-link** policy (§8): one cluster streams TCP
intra-plant *and* ships discrete QoS-1 messages MQTT northbound, choosing per hop.

## 8. Transport selection and relay

- **Per-peer policy.** Which transport reaches which node is a deployment policy (Quark 013 config),
  informed by node **capabilities** (010 §2.1) — a node advertises which transports it is reachable
  on (`reachable=tcp,mqtt`). The router picks a transport both endpoints share.
- **No direct link? Relay.** When two nodes share no direct transport, Quark's DHT-relay (026,
  ADR-011 FIFO-under-relay) forwards through a mutually-reachable node — already proven to preserve
  FIFO across a variable-hop path. AeroEdge adds nothing; it just ensures at least one reachable
  transport exists per node.
- **Mixed fabric.** A cluster can run TCP intra-plant and MQTT plant→cloud simultaneously — the
  router treats each peer link independently. This is how a plant that only exposes MQTT northbound
  still joins one logical AeroEdge cluster.

## 9. Relationship to the Event bus and device transports

- **Event bus (002 §4)** rides inter-actor transport: a cross-node `publish` is a `tell` over
  whatever transport reaches the subscriber's node. No separate event transport.
- **MQTT driver (006)** and **`BusMesAdapter` (012)** are column-B (device/integration) — they speak
  MQTT to *devices/MES*, not to actors, and do **not** carry `MessageFrame`s or owe C1–C5. A single
  broker may serve both roles on different topics; keep the topic namespaces disjoint
  (`aero/xport/*` for transport vs `plant/#` for device data).

## 10. Invariants (normative)

- **X1** — inter-actor transport is Quark's `Transport` seam; AeroEdge adds adapters, never a
  parallel interface.
- **X2** — every inter-actor adapter honors C1–C5; one that cannot give per-sender FIFO adds a
  resequencing shim or is ineligible for actor transport.
- **X3** — brokers are integrated as clients, never reimplemented or replaced (B1); the default
  fabric (Quark TCP, coordinator-free) needs no broker (B2).
- **X4** — an embedded broker exists only as a **fully-distributed plugin** behind the seam; core
  never depends on it (B3).
- **X5** — transport choice is a per-peer deployment policy; missing direct links relay through
  Quark's FIFO-preserving DHT-relay, never through a bespoke AeroEdge router.
- **X6** — broker/transport security is TLS (C5) *plus* Quark 020 principal propagation; a broker is
  never trusted to authorize actor messages.
- **X7** — cross-node streams (024) keep end-to-end backpressure only over a flow-controlled
  transport (TCP/gRPC); over a broker the broker is the backpressure boundary and overflow policy
  must be explicit there (§7).

## 11. Open questions

- **MQTT topic granularity** — one inbox topic per node (simple, preserves per-publisher order) vs
  per-actor topics (finer fan-out, more subscriptions, weaker cross-actor ordering). Leaning
  per-node inbox + in-frame ActorId; revisit if fan-out patterns demand per-actor topics.
- **Resequencing shim bounds** — the reorder/dedup window size and how it interacts with Quark 017's
  existing dedup (avoid double-buffering). Co-design with 017 usage.
- **Embedded distributed broker** — whether to build one at all, or always require external broker
  infra; if built, which distributed design (its own SWIM-like membership? reuse Quark's?). Deferred
  until a deployment actually forbids external brokers.
- **Transport capability advertisement** — where `reachable=tcp,mqtt` lives (device/node registry,
  010 §5) and how it feeds placement so an actor isn't placed on a node its callers can't reach.
- **QUIC** — Quark lists QUIC as a transport adapter; it may subsume much of the MQTT-for-NAT use
  case with lower latency. Evaluate against MQTT once a real NAT-constrained deployment appears.
