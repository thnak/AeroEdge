# 012 — MES Integration Hook

> Draft v0.1. AeroEdge's integration contract with an MES (Manufacturing Execution System).
> AeroEdge **defines this hook** — it is not a fixed connector to one MES product but a
> bidirectional seam any MES adapter implements. Keyword-mandated; AeroEdge-owned.

## 1. Why a hook, not a connector

Different factories run different MES (Takako, Tamatra, VaultForge, SAP, a REST service, a
message bus). AeroEdge must not bake in one. So MES integration is a **seam**: a stable
interface AeroEdge speaks, plus pluggable **MES adapters** that translate it to a specific
MES. This mirrors Quark's "seam + optional adapter" posture (transport, persistence, etc.).

```text
        AeroEdge flows/events                 MES adapter (pluggable)          MES
   ┌──────────────────────────┐          ┌────────────────────────┐      ┌─────────┐
   │  EdgeActor / LineActor    │          │  IMesAdapter impl:     │      │  Takako │
   │  emits ProductionFinished │──hook──▶ │  → POST /production    │─────▶│  Tamatra│
   │  consumes MESOrder        │◀─hook──  │  ← poll orders / sub   │◀─────│  SAP …  │
   └──────────────────────────┘          └────────────────────────┘      └─────────┘
```

## 2. The two directions

### 2.1 Outbound — AeroEdge → MES (report)

Edge events that the MES cares about are reported through the hook:

| AeroEdge Event/Output | MES meaning |
|---|---|
| `ProductionFinished` | report produced quantity / cycle complete |
| `AlarmRaised` | report a fault / downtime reason |
| `TagChanged` (selected) | report a measured process value / SPC point |
| `FirmwareUpdated` (011) | report device/asset state change |

Outbound reporting rides the **Output-node → EgressActor** pattern (004 §4, 005 §2): an
`MesOutputNode` stages an MES report into `ctx.output`; the flow `tell`s an **`MesGateway`
actor**; the gateway calls the configured `IMesAdapter` (which does the blocking HTTP/DB/bus
I/O on its own lane). Flows never block on the MES.

### 2.2 Inbound — MES → AeroEdge (command)

The MES drives production by sending orders/recipes into AeroEdge:

| MES input | AeroEdge Command |
|---|---|
| new work order | `MESOrderReceived` → `LineActor` |
| recipe / setpoints | `ApplyRecipe` → `EdgeActor`/`LineActor` |
| firmware rollout request | `DeployFirmware` (011) → `FleetActor` |
| start/stop line | `StartLine` / `StopLine` → `LineActor` |

Inbound flow: the `MesGateway` actor (or an adapter-owned ingress) receives from the MES
(webhook, poll, or subscription — the adapter's choice) and translates it into an AeroEdge
Command delivered by `tell`. From there it is an ordinary Command-triggered Flow (002/004).

## 3. The seam: `IMesAdapter`

```cpp
namespace aero::mes {

// AeroEdge defines this. An MES vendor/site implements it. The MesGateway actor owns
// the instance and is the ONLY caller — so the adapter may block on I/O (it runs on the
// gateway's lane / a BlockingHandler), never on a flow (I1).
class IMesAdapter {
public:
    virtual ~IMesAdapter() = default;

    // Outbound: AeroEdge → MES. Report an edge event. Returns delivery status;
    // the gateway applies retry/outbox semantics (Quark 017) around this call.
    virtual MesResult report(const MesReport& r) = 0;

    // Inbound: MES → AeroEdge. The adapter delivers MES-originated commands by invoking
    // this sink (poll loop, webhook handler, or subscription — adapter's choice).
    virtual void set_command_sink(MesCommandSink sink) = 0;

    // Lifecycle: connect/authenticate to the MES; called on gateway activation.
    virtual MesStatus connect(const MesConfig& cfg) = 0;

    virtual const MesAdapterDescriptor& descriptor() const noexcept = 0;
};

} // namespace aero::mes
```

- `MesReport` / `MesCommand` are **AeroEdge-canonical** DTOs (order, production count,
  alarm, tag sample, recipe). The adapter maps them to the MES's native schema. AeroEdge
  flows only ever see the canonical types — swapping MES swaps only the adapter.
- **Delivery guarantees.** Outbound reports go through a **transactional outbox** (Quark
  017): a `ProductionFinished` is durably staged before the flow commits, so an MES outage
  never loses a production count; the gateway drains the outbox with at-least-once + MES-side
  idempotency keys. This is the one place AeroEdge leans hard on Quark 017.

## 4. Where the hook attaches in a flow

The MES hook is exposed to flow authors as two node types, so low-code flows can wire MES
integration without touching gateway internals:

| Node | Category | Effect |
|---|---|---|
| `MesReportNode` | Output | stage a `MesReport` for the gateway (outbound) |
| `MesOrderSourceNode` | Source | inject a received `MESOrderReceived` payload into the flow |

Both are ordinary `INode`s (005). `MesReportNode::process` only *stages* (I1/N5); the actual
MES call is the gateway's, behind the adapter.

## 5. Adapters (initial targets)

| Adapter | Talks to | Transport |
|---|---|---|
| `RestMesAdapter` | any REST MES | HTTP + JSON |
| `TakakoMesAdapter` / `TamatraMesAdapter` | the sibling Blazor MES apps | REST/Mongo per their APIs |
| `BusMesAdapter` | MQTT/Kafka-based MES | message bus |

Adapters are Native extensions (008) loaded per deployment; the default build links only
`RestMesAdapter`.

## 6. Invariants (normative)

- **M1** — flows speak only AeroEdge-canonical MES DTOs; MES-specific schema lives in the
  adapter (swap MES = swap adapter, flows unchanged).
- **M2** — flows never block on MES I/O; all MES calls go through the `MesGateway` actor.
- **M3** — outbound production/quality reports use the durable outbox (Quark 017); an MES
  outage delays but never drops them.
- **M4** — inbound MES commands enter as ordinary Commands and obey the same FIFO/sequential
  and authorization rules as any other Command (002, Quark 020).
- **M5** — the adapter authenticates/authorizes via Quark 020; AeroEdge does not embed MES
  credentials in flows or nodes.

## 7. Open questions

- **Canonical DTO coverage** — the initial `MesReport`/`MesCommand` field set; grow from the
  Takako/Tamatra/VaultForge domains rather than guessing.
- **Ordering vs the MES** — whether production events must reach the MES in strict order or
  may be reconciled by timestamp/idempotency key; affects outbox drain policy.
- **Backfill/replay** — replaying edge events to the MES after a long outage (bounded outbox
  vs unbounded); ties to 007 (State) retention.
- **Authorization granularity** — which MES-originated commands (e.g. `DeployFirmware`, 011)
  require elevated authorization; align with Quark 020 principal propagation.
