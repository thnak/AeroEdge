# 022 — Device Registry and Lifecycle

> Draft v0.2. The management layer AeroEdge was missing: **what devices exist**, what they
> look like (model, points), where they are, and what state they are in. Specs 006/017/018 say
> how bytes get in. This spec says *which device* they belong to. It answers the
> "device→capability source of truth" question left open in 010 §6.
>
> v0.2 incorporates the red-team and use-case review: gateway/child devices, device
> replacement, staged point changes, projection scope, volatile `last_seen`, health history.
>
> Suite context: AeroEdge takes over device management from AeroOEE
> ([AeroSuite 07](https://github.com/Iot-Viet-Solution/AeroSuite/blob/main/docs/07-aeroedge-target-scope.md)).

## 1. Why a registry

Today a device exists only implicitly, as a driver config inside an Application (009) plus an
`EdgeActor` (001). That's enough to run a flow, but not enough to manage a fleet. Nothing
answers *which devices do we own*, *which firmware they run*, *which are offline*, or *which
machine they measure*. OTA (011), placement (010), binding (024), health (016) and identity
(023) all need one source of truth. The registry is it.

## 2. Concepts

| Concept | Meaning | Notes |
|---|---|---|
| **Device model** | Template for a kind of device: protocol, **payload codec** (§2.2), points, firmware family, capabilities (heartbeat, A/B slots, OTA strategy, command set per 028). | Reused by many devices. Versioned. |
| **Device** | One physical instance: stable `device_id`, model ref, site/segment, network address, firmware version, lifecycle state, optional **parent** (§2.1). | Each device has health and identity. Only transport owners have a driver (§2.1). |
| **Point** | One signal on a device: address (pin / register / node id / topic field), direction (in/out), data type, unit, physical I/O settings (pin mode, interrupt edge, debounce, analog range, poll rate). | Inherited from the model, overridable per device. Address shape is the protocol plugin's config schema (015). **Debounce lives only here** (024 §3 layer 1). |
| **Device group** | Named set of devices (site, line, model, hand-picked). | Target for OTA rollouts (011 §4), config pushes and access scopes (023). |
| **Firmware artifact** | Versioned, **signed** image for a device model, with checksum. | The catalog 011 pulls from. |

A **point is not a machine signal.** It's a physical address. Meaning ("this is the part
counter of machine M-03") is assigned by binding (024).

### 2.1 Gateway and child devices

A Modbus RTU bus (30 slaves on one serial port), an OPC UA server session or a multi-channel IO
box carries many logical devices over **one** transport. 006 D1 allows exactly one producer
per stream, so:

- A **gateway device** owns the transport. It has the driver, the `EdgeActor` that runs the
  stream, and the connection health.
- **Child devices** (`parent = gateway`) have their own records, points, health and bindings,
  but no driver. Their frames are demultiplexed by the gateway's actor and forwarded to the
  child's `EdgeActor` by `tell`.
- Health is per child where the protocol allows (Modbus exception/timeout per slave). Otherwise
  it's inherited from the gateway.
- Revocation and OTA can target the gateway or a single child, as the protocol allows (023, 011).

A directly connected device (an MQTT ESP32) is simply a device with no parent and its own driver
or broker session.

### 2.2 Payload codec

The device model names a **payload codec**: how a frame or MQTT message is decoded into point
values (and how commands are encoded, 028). Codecs are plugins (008), registered by name and
version. Built-ins:

| Codec | For |
|---|---|
| `aero-device-v4` | AeroEdge-native device protocol (028). **Required for all ESP32-class devices**, since the existing fleet is re-flashed (no legacy formats are carried over). |
| `modbus-registers` | Register map → points, with word order and float/scaled-int decoding |
| `opcua-nodes` | Node id → point |
| `json-path` | Generic JSON over MQTT/HTTP for third-party devices |

## 3. Lifecycle

```text
Registered ─▶ Provisioned ─▶ Active ⇄ Offline
                               │  ▲      │
                               ▼  │      ▼
                            Maintenance ◀┘
                               │
            any state ─────────┴──▶ Retired
```

| State | Entered by | Meaning |
|---|---|---|
| `Registered` | operator / import / claim request (023) | Record exists. No credential. No `EdgeActor`. |
| `Provisioned` | credential issued (023) | Device may connect. `EdgeActor` is activated on first contact or driver start. |
| `Active` / `Offline` | **derived** from connectivity (§5) | Never set by hand. |
| `Maintenance` | operator, from `Active` **or** `Offline` | Data still flows but is flagged. Bindings deliver as `test` (024 §2). |
| `Retired` | operator | Credential revoked (023), bindings closed (024), `EdgeActor` deactivated. Record and history kept. |

Transitions are **Commands** on the `DeviceRegistryActor` (§4). Lifecycle changes emit
`DeviceLifecycleChanged`. Connectivity changes emit `DeviceConnectivityChanged` (§5). These are
two separate events, because maintenance isn't a connectivity state.

### 3.1 Device replacement

`ReplaceDevice(old, new)` is one Command, used when a maintenance tech swaps a dead counter for a
spare:

1. `new` must be `Provisioned` (or is provisioned in the same Command) with the same model or a
   compatible one.
2. All open bindings of `old` are closed and re-opened on `new` with **effective time = node
   apply time** (024 §2.2).
3. Counter baselines of `new`'s points are **reset to unknown**. The first reading establishes a
   baseline and produces no delta (024 §4), so there's no false spike.
4. `old` → `Retired` (identity revoked).
5. Emits `DeviceReplaced{old, new}` upstream so consumers can annotate the gap.

## 4. Where the registry lives

- **`DeviceRegistryActor`**: one per deployment, hosted on the **control plane** (027). It
  owns the authoritative records as actor state, persisted via Quark 012 **EventSourced**, since
  registry changes need an audit trail.
- **Projections to nodes.** Each node receives a read-only projection of the devices it **could**
  host: every device whose capability tags (site/segment/gateway) the node satisfies, not
  only the devices currently placed there. Placement is computed by the cluster (010 HRW), so the
  control plane doesn't know it, and a failover must work while the control plane is down
  (027 CP1). Nodes persist their projection as **Snapshot actor state** (S1). `shard_memory` is
  only the in-memory read path (007 S5).
- **Projections are versioned** `(control_epoch, version)` (027 §3). A node applies only a
  projection newer than the one it holds.
- **Placement input:** a device's site/segment/gateway fields are the capability tags 010 §2.1
  places `EdgeActor`s by. This answers 010 §6.

### 4.1 Changes that affect running Applications

An Application (009) references devices by `device_id`. The driver takes connection and point
config from the projection. A point change (address, scaling, poll rate) therefore changes
running behavior, and it **must not bypass 009's safety gates**:

- Point and connection changes are classified **Live** or **BuildOnly** like any 009 change
  (P3/P4).
- They roll out through 009 §5 **staged, health-gated rollout** to the affected nodes, with
  rollback to the previous projection version (P6).
- Cosmetic changes (name, description, group membership) are applied immediately.

## 5. Health and connectivity

Connectivity is derived **primarily from the transport session**, not from heartbeats. Many
devices (pulse-only counters, report-on-change PLCs) are legitimately silent while their machine
is idle.

| Signal | Used for |
|---|---|
| Broker session / driver connection (017, 006) | **Primary.** Session up = online, session down = offline |
| Heartbeat (only for models that declare `heartbeat` capability) | Detects a hung device behind a live session: `k × interval` (default `k = 3`) with no heartbeat → Offline |
| Gateway per-child status (Modbus timeout, OPC UA bad status) | Child health (§2.1) |

- **One offline definition per model.** The broker keep-alive (017) and heartbeat rule are not
  two competing clocks. The model declares which is authoritative.
- **`last_seen` is volatile.** It's held in memory and persisted only when connectivity state
  changes. It's never persisted per frame (007 S4).
- **While ingest is throttled** (026 §3), offline detection is suspended for devices whose
  silence is caused by AeroEdge itself.
- Health fields: `connection_state`, `last_seen`, `firmware_version`, `boot_id`, `uptime`,
  `rssi` / signal quality, `free_heap`, `reboot_reason`, OTA phase/error (011). The fields come
  from the codec (028 §4 heartbeat).
- **Health history** (sessions, heartbeats, reboots, device logs) is a retained data class
  (026 §2), queryable in Studio. This replaces AeroOEE's heartbeat, connection-gap and device-log
  analytics.
- **Point quality** (stale, flatline, error) is evaluated per binding in 024 §5.4.

## 6. Import and API

- Bulk import/export (CSV/JSON) of models, devices, points, groups and bindings (024 §2.3)
  through `aero-api` (013 T2: no side channel).
- Every write goes through the actor, which validates and emits Events. The API never writes
  storage directly.
- **`device_id` charset:** `[A-Za-z0-9_-]{1,64}`. It's used verbatim in topics and MQTT
  client-ids (023 IA6), so `/ + # $` and whitespace are rejected.

## 7. Invariants (normative)

- **DR1**: the registry is the single source of truth for which devices exist and how they are
  addressed. Applications reference devices by `device_id` and never embed connection or point
  config.
- **DR2**: exactly one transport owner (driver) per physical connection. Child devices of a
  gateway have actors and health but no driver (§2.1).
- **DR3**: `Active`/`Offline` are derived from observed connectivity and are never set by an
  operator.
- **DR4**: retiring or replacing a device revokes its identity (023) and closes or moves its
  bindings (024) in the same Command. History stays resolvable by `device_id`.
- **DR5**: registry changes are EventSourced (auditable). Nodes hold only versioned, read-only
  projections.
- **DR6**: point and connection changes reach running nodes only through 009's staged,
  rollback-capable rollout.
- **DR7**: per-frame telemetry is never persisted as registry or actor state. `last_seen` is
  persisted only on connectivity change.

## 8. Implementation status

Not started. Suggested order: model + device + point records → versioned projection to eligible
nodes → derived driver config → session-based connectivity → gateway/child demux → replace
device → import/export → Studio screens.

## 9. Open questions

- **Operator terminals** (station tablets, HMI boxes): register them as devices (OTA, health),
  with no points, while their business APIs stay in AeroOEE/AeroMes? Leaning yes.
- **Auto-discovery** (OPC UA browse, Modbus scan): create `Registered` devices automatically,
  or only suggest them? Ties to 015's runtime-assisted discovery.
- **Point overrides:** full override per device, or only address/scaling?
- **P2:** HTTP-posting devices (006/018 HTTP southbound), scales/RFID/barcode codecs, energy
  meters on non-machine assets (binds via 024).
