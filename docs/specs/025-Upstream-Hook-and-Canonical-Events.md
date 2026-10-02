# 025 — Upstream Hook and Canonical Machine Events

> Draft v0.2. Generalizes the MES hook (012) into an **upstream hook**: one seam for every
> system above AeroEdge (AeroOEE, AeroMes, ERP, a historian, a third party). It also defines
> the **canonical events** AeroEdge publishes and what consumers must do with them. 012
> remains valid as the MES *profile* of this hook (§8).
>
> v0.2 incorporates the red-team and use-case review:
> - producer-side transactional outbox;
> - event identity `{source}:{type}:{epoch}:{seq}`;
> - critical vs best-effort subscriptions;
> - a live lane ahead of the backfill;
> - liveness leases;
> - clock policy;
> - snapshot/resync;
> - consumer obligations;
> - MQTT topic delivery downgraded to a live view.

## 1. Why generalize

012 assumed one upstream: an MES. The suite now has at least two consumers of the same machine
data (AeroOEE for Machine OEE, AeroMes for Order OEE and production counting), plus third
parties. 012's ingredients are right (outbox, at-least-once, idempotency, pluggable adapter). They
need to stop being MES-specific, and v0.1's single shared outbox coupled consumers to each
other.

## 2. The seam

```text
MachineActor / EdgeActor
   └─ commit(state) + stage(events)  ── one transaction (Quark 012/017) ──▶ producer outbox
                                                                              │
UpstreamGateway (one per node) ── drains local producer outboxes ─────────────┘
   ├─▶ IUpstreamAdapter: AeroOEE   (critical)
   ├─▶ IUpstreamAdapter: AeroMes   (critical, 012 profile)
   ├─▶ IUpstreamAdapter: webhook   (best-effort)
   └─▶ live view: MQTT topics      (at-most-once, §4)
```

- **Producer outbox.** Events are staged in the **producer's own transactional outbox** in the
  same commit as the state change that caused them (012 M3, Quark 017). That covers, for
  example, a window close. There's no separate `tell` to a gateway that a crash could lose.
- **`UpstreamGateway`.** One per node. It drains the outboxes of producers on that node and
  delivers to adapters. It keeps a **cursor per subscription** and deduplicates by event
  identity (§3).
- **`IUpstreamAdapter`**: `connect`, `report(batch)` → per-event ack, `set_command_sink`,
  `descriptor`. `IMesAdapter` is an `IUpstreamAdapter` with the MES command set (§8).
- Flows never block on upstream I/O (012 M2).

> **Implementation gate.** Quark's durable ack-cursor in `delivery.hpp` is still listed as
> deferred. Per-subscription durable cursors depend on it, or on an AeroEdge-local cursor table
> in the Quark 012 store. Decide before implementing (§11).

### 2.1 Subscription classes

| Class | For | Holds the outbox? | On overflow |
|---|---|---|---|
| `critical` | systems of record (AeroMes, AeroOEE) | **yes**. Events are retained until every critical subscription has acked | outbox pressure → 026 §3 (alarm, then ingest throttle) |
| `best_effort` | third-party webhooks, dashboards, analytics | no. Has a **byte quota** of retained backlog | subscription **suspended** with `SubscriptionSuspended` alarm. On resume it continues from the oldest retained event and receives a `DeliveryGap` marker |

A dead best-effort consumer can't block counting for the systems of record (UH2).

### 2.2 Ordering and the live lane

- Delivery is **at-least-once**, ordered **per `(source, producer_epoch)` by `seq`**. Across
  epochs (restart, failover, split brain; 024 §5.1), order is by epoch start, then `edge_ts`.
- After an outage, a backlog can take hours to drain (e.g. 7 days × 200 machines ≈ 9 M events).
  The gateway therefore runs two lanes per subscription:
  - **live lane**: new events, delivered immediately;
  - **backfill lane**: the retained backlog, drained in parallel at a configurable rate.

  Consumers order by `(producer_epoch, seq)`, not by arrival (§7), so dashboards are current
  while history catches up.

### 2.3 Liveness lease

Each subscription holds a **liveness lease** renewed by successful delivery or adapter
heartbeat (default lease 60 s). Lease expiry is the definition of **upstream loss** for that
subscription. It drives output fallback (024 §6) and the `UpstreamLost` alarm.

## 3. Canonical events

### 3.1 Common fields

| Field | Meaning |
|---|---|
| `event_id` | `{source}:{event_type}:{producer_epoch}:{seq}`. `source` = `machine_key` or `device_id`. **This is the idempotency key** |
| `schema_version` | major.minor. Additive changes only within a major (UH5) |
| `producer_epoch`, `seq` | ordering (§2.2). `seq` is monotonic per producer epoch |
| `edge_ts` | event time, node clock (UTC) |
| `ingest_ts` | when the node received the underlying reading |
| `clock_quality` | `synced` \| `unsynced` \| `device_ts_replaced` (§3.3) |
| `site`, `node_id` | origin |
| `test` | from a `test` binding or a device in Maintenance (024 §2.3) |

Window times (`window_from`, `window_to`) are **data**, never identity. Count and reject events
for the same window therefore have different `event_id`s and can't collide.

### 3.2 Event catalog

| Event | Payload | Emitted by |
|---|---|---|
| `MachineStateChanged` | `from`, `to` (incl. `Unknown`), `cause` | `MachineActor` (024 §5.3) |
| `CountReported` | `count`, `unit`, `kind` (total/good), `channel`, `pair_group`, window, `product`, `cycle_ms {min,avg,max,last,n}` | `MachineActor` |
| `RejectReported` | `count`, `unit`, `channel`, `code`, window | `MachineActor` |
| `CycleReported` *(opt-in)* | `channel`, `cycle_ms`, `ts` | `MachineActor` (024 §4.2) |
| `CountGap` | `from`, `to` (or unknown), `reason`, `recoverable` | `MachineActor` (024 §5.1) |
| `AlarmRaised` / `AlarmCleared` | `code`, `text`, `severity` | `MachineActor` |
| `ProductChanged` | `product` / `mold` / `recipe`, `previous`, `source` (device/upstream) | `MachineActor` |
| `OutputChanged` | `output_role`, `value`, `cause` | `MachineActor` (024 §6) |
| `PointQualityChanged` | `point`, `role`, `quality` | `MachineActor` (024 §5.4) |
| `DeviceConnectivityChanged` | `device_id`, online/offline, affected `machine_key`s | `EdgeActor` (022 §5) |
| `DeviceLifecycleChanged` | `device_id`, `from`, `to` | registry (022 §3) |
| `DeviceReplaced` | `old`, `new`, affected machines | registry (022 §3.1) |
| `ProcessSample` *(opt-in)* | `role`, `value`, `unit`. Downsampled/deadbanded | `MachineActor` |
| `FirmwareUpdated` / `FirmwareFailed` | `device_id`, `version`, `error` | OTA (011) |
| `CommandResult` | `command_id`, `status`, `detail` | 028 |

Raw point samples are **not** canonical events and never cross the hook in bulk (UH4, 026).
The schema lives in `aero-schema` (013 §4), codegen'd for C++, TS and C# (013 T3).

### 3.3 Clock policy

- **Nodes must run time sync** (NTP/PTP, or a local plant time source for air-gapped sites). This
  is a deployment requirement, not an option. Window attribution, binding validity (024 §2.2) and
  upstream job lookup all depend on it.
- Each node reports its measured clock offset (016 metrics). An offset above a threshold
  (default 2 s) raises `ClockSkew` and stamps events `clock_quality = unsynced`.
- Device-supplied timestamps that are pre-epoch (device not yet SNTP-synced) or skewed beyond a
  threshold are **replaced by `ingest_ts`** and flagged `device_ts_replaced`.

## 4. Delivery bindings

| Binding | Guarantee | Use |
|---|---|---|
| **Consumer-ack adapter** (AeroOEE, AeroMes) | at-least-once, per-event ack, cursor advances on ack | systems of record |
| **REST webhook** | at-least-once (ack = 2xx). HMAC-signed body, retry with backoff, auto-pause after N consecutive failures (= suspension, §2.1), delivery statistics | third parties |
| **Bus** (Kafka, RabbitMQ, external MQTT) | via 017's **`IBridgeSink`**. Guarantee as the sink declares | integrations |
| **MQTT topics on the native broker**: `aero/{site}/machines/{machine_key}/events/{event_type}` | **at-most-once live view.** The broker acks the publish, not the consumer | dashboards, Studio, quick integrations. **Never** a system of record |

There's no second bus/webhook seam beside `IBridgeSink`. Bus delivery reuses it (017 M3/M8).

## 5. Inbound (upstream → AeroEdge)

Inbound messages enter through **any node's** gateway and are routed to the target actor by
Quark actor routing (027 §2). They authenticate as a **service identity** (023) and are
authorized per command type and machine scope (012 M4/M5).

| Inbound | Effect |
|---|---|
| `MachinesUpserted` / `MachineDeactivated` | machine master data, with `version` + writer identity (024 §1) |
| `SetOutput` / `Interlock` | output by machine + role (024 §6) |
| `SetCurrentProduct` | product context when the machine has no `product` binding (024 §5.5) |
| `ApplyRecipe` / parameters, `StartLine`, `StopLine`, `MESOrderReceived` | as in 012 §2.2 |
| `DeployFirmware` | OTA rollout (011) |
| Device commands, terminal request/response | 028 |

## 6. Snapshot and resync

Push alone isn't enough. A consumer that restarts or is restored, and a dashboard that opens,
need the **current** picture:

- `GET /machines/{key}/snapshot` (and a bulk variant per site) returns: state + since, product,
  open-window partial counts, output values, bound-device health, point quality, and the last
  `(producer_epoch, seq)` per producer.
- A new or reset subscription starts with a snapshot, then continues from the cursor.
- This replaces AeroOEE's `reload-state` and serves polling dashboard widgets.

## 7. Consumer obligations (normative for anything consuming the hook)

1. **Deduplicate on `event_id`.** Redelivery is normal.
2. **Accept backfill:** events whose `edge_ts` is up to the outbox retention (026, default 7
   days) in the past. A ±minutes acceptance window would drop every count delivered after an
   outage.
3. **Order by `(producer_epoch, seq)`**, not by arrival (live and backfill lanes interleave).
4. **Treat `MachineStateChanged` as authoritative.** Don't re-derive machine state from raw
   signals in parallel (024 MB4).
5. **Exclude `test = true`** from production and OEE.
6. **Handle `CountGap`**: show it, and don't silently interpolate.
7. Flag overlapping `(source, epoch)` streams as `ConcurrentProducers` (024 §5.1).

> AeroMes today enforces ±5 min on ingest timestamps, doesn't deduplicate, and derives machine
> state itself. Meeting §7 is an AeroMes work item, tracked in AeroSuite doc 03.

## 8. Relationship to 012

- 012 becomes the **MES profile**: `IMesAdapter` = `IUpstreamAdapter` + MES command set (orders,
  recipes, start/stop). `MesGateway` = `UpstreamGateway`. `MesReportNode` = `UpstreamReportNode`
  restricted to MES subscriptions.
- 012's outbound table maps to canonical events: `ProductionFinished` → `CountReported`,
  `TagChanged (selected)` → `ProcessSample`.
- 012 M1–M5 remain normative for all adapters.

## 9. Invariants (normative)

- **UH1**: events are staged in the producer's transactional outbox in the same commit as the
  state change. No adapter keeps a private outbox.
- **UH2**: a `best_effort` subscription never blocks or drops events for a `critical` one. Only
  critical subscriptions hold the outbox.
- **UH3**: delivery is at-least-once, ordered per `(source, producer_epoch)` by `seq`. Every
  event has a collision-free `event_id`.
- **UH4**: upstream receives canonical events only. Raw point samples never cross the hook in
  bulk.
- **UH5**: one schema source of truth (`aero-schema`), with `schema_version`. Additive changes
  only within a major version.
- **UH6**: MQTT topic delivery is a live view (at-most-once) and is never the delivery path of a
  system of record.
- **UH7**: inbound commands are routed node-side. The control plane isn't on the data or
  command path (027 CP3).
- **UH8**: event time is the node clock under the §3.3 clock policy. Device timestamps are
  trusted only when sane.

## 10. Implementation status

012's `MesGateway` + `RestMesAdapter` design exists. Not started: producer outboxes,
per-subscription cursors and classes, live/backfill lanes, leases, canonical schema in
`aero-schema`, snapshot API, AeroOEE adapter, clock-offset reporting.

## 11. Open questions

- Durable cursor: wait for Quark's deferred ack-cursor, or keep cursors in an AeroEdge table in
  the Quark 012 store?
- Default backfill drain rate, and whether it adapts to consumer latency.
- **P2:** namespaced **extension events** (e.g. `x-takako.TrayMoved`) through the same outbox
  and schema registry, for customer-specific domain events.
