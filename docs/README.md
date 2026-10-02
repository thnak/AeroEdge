# AeroEdge documentation

```text
docs/
├── guides/                  How to build, run and use AeroEdge
│   ├── setup-guide.md
│   └── user-guide.md
├── specs/                   The RFC — architecture overview + numbered specs (source of truth)
│   ├── ArchitectureSpecification.md
│   └── NNN-<Title>.md
└── IMPLEMENTATION-PLAN.md   Phased build plan for the specs
```

Contributor rules stay at the repo root: [AGENTS.md](../AGENTS.md) and [CONVENTIONS.md](../CONVENTIONS.md).

## Guides

- **[Setup Guide](guides/setup-guide.md)** — build from source, run the daemon, run the Studio.
- **[User Guide](guides/user-guide.md)** — deploy & manage flows, the built-in node/driver catalog, the
  rule expression language, the REST API, monitoring.
- **[Native MQTT Broker — Message Flow](native-broker-message-flow.md)** — where the embedded broker
  runs, what happens to a PUBLISH, clustering, and the not-yet-wired path into flows (Mermaid diagrams).

## Specs — reading order

| # | Document | Covers | Status |
|---|---|---|---|
| — | [ArchitectureSpecification.md](specs/ArchitectureSpecification.md) | Vision, principles, invariants, glossary | Draft |
| — | [IMPLEMENTATION-PLAN.md](IMPLEMENTATION-PLAN.md) | Phased build plan (Phases 0–10) for all specs + projects; Quark readiness gates; verification | Draft |
| 001 | [001-Actor-Model-and-Quark-Mapping.md](specs/001-Actor-Model-and-Quark-Mapping.md) | Domain actors; what maps to which Quark primitive; what we must **not** rebuild | Draft |
| 002 | [002-Message-Model-Command-and-Event.md](specs/002-Message-Model-Command-and-Event.md) | Command (mutating, FIFO) vs Event (immutable, published); the event bus | Draft |
| 003 | [003-Processing-Context.md](specs/003-Processing-Context.md) | The per-command mutable context threaded through a Flow; lifetime & ownership | Draft |
| 004 | [004-Flow-Runtime-and-DAG.md](specs/004-Flow-Runtime-and-DAG.md) | Compile-once DAG, execution model, triggers, sync-vs-async decision | Draft |
| 005 | [005-Node-SDK-and-INode.md](specs/005-Node-SDK-and-INode.md) | The `INode` contract, node categories, registry, versioning | Draft |
| 006 | [006-Drivers-and-Sources.md](specs/006-Drivers-and-Sources.md) | Drivers (PLC/MQTT/TCP/Serial/Camera) over Quark streaming; backpressure; frame lifetime; write/fencing | Draft |
| 007 | [007-State-and-Persistence.md](specs/007-State-and-Persistence.md) | Three state tiers; Quark 012 Snapshot/EventSourced × Sync/Batched; stateful-node rule; fencing | Draft |
| 008 | [008-Extension-Model-Native-and-WASM.md](specs/008-Extension-Model-Native-and-WASM.md) | Native (C ABI) & WASM (sandboxed) nodes/drivers behind one seam; Script/Rule DSL; signing | Draft |
| 009 | [009-Deployment-and-Flow-Versioning.md](specs/009-Deployment-and-Flow-Versioning.md) | Declarative Applications; deploy-time compile; hot-reload (Live vs BuildOnly); staged rollout | Draft |
| 010 | [010-Distribution-and-Horizontal-Scale.md](specs/010-Distribution-and-Horizontal-Scale.md) | Cluster of edge nodes; device-affinity placement & rebalancing over Quark 010/025/026 | Draft |
| 011 | [011-Firmware-OTA.md](specs/011-Firmware-OTA.md) | Signed, staged, rollback-safe edge firmware OTA; fleet rollout orchestration | Draft |
| 012 | [012-MES-Integration-Hook.md](specs/012-MES-Integration-Hook.md) | The `IMesAdapter` seam; bidirectional MES report/command with a durable outbox | Draft |
| 013 | [013-Solution-Topology-and-Studio.md](specs/013-Solution-Topology-and-Studio.md) | Project/module breakdown: Runtime + SDK + built-ins + API (this repo) vs the Studio tooling repo; build phasing | Draft |
| 014 | [014-Transport-Interface-and-Pluggable-Transports.md](specs/014-Transport-Interface-and-Pluggable-Transports.md) | Multi-transport (Local/TCP/MQTT/gRPC) as adapters behind Quark's `Transport` seam; broker-as-client, never reimplemented; embedded broker = distributed plugin | Draft |
| 015 | [015-Configuration-Model-and-Studio-Plugin-UI.md](specs/015-Configuration-Model-and-Studio-Plugin-UI.md) | Full-feature protocol config (MQTT/OPC UA/Modbus) via plugin UI contributions: schema-driven forms + custom micro-frontends; runtime-assisted discovery | Draft |
| 016 | [016-Studio-IA-and-Observability.md](specs/016-Studio-IA-and-Observability.md) | Studio information architecture; fleet/OTA/MES observability and their REST surface | Draft |
| 017 | [017-Native-Broker-and-Southbound-Termination.md](specs/017-Native-Broker-and-Southbound-Termination.md) | Embedded MQTT broker as a distributed plugin behind the 014 seam; southbound termination at the edge | Draft |
| 018 | [018-Multi-Protocol-Southbound-OPC-UA-Modbus.md](specs/018-Multi-Protocol-Southbound-OPC-UA-Modbus.md) | Modbus-TCP and OPC-UA southbound drivers under the 006 driver contract | Draft |
| 019 | [019-Flow-Graph-Model-and-Studio-Canvas-API.md](specs/019-Flow-Graph-Model-and-Studio-Canvas-API.md) | Flow as a graph (branch/fan-out/merge); Studio canvas API | Draft |
| 020 | [020-Block-Shape-Taxonomy-and-Loop-Constructs.md](specs/020-Block-Shape-Taxonomy-and-Loop-Constructs.md) | Blockly-grounded block shape taxonomy; loop constructs; expr-tree editor | Draft |
| 021 | [021-Native-Broker-Performance-Redesign.md](specs/021-Native-Broker-Performance-Redesign.md) | Native broker I/O & fan-out performance redesign (research log) | WIP |
| 022 | [022-Device-Registry-and-Lifecycle.md](specs/022-Device-Registry-and-Lifecycle.md) | Device models, devices, points, gateway/child devices, payload codecs; lifecycle, replacement; session-based health; versioned projections | Draft |
| 023 | [023-Identity-Provisioning-and-Broker-Access.md](specs/023-Identity-Provisioning-and-Broker-Access.md) | Device/service/node identities; per-unit claim; client-id binding; templated ACL; revocation incl. node-local; required 017 changes | Draft |
| 024 | [024-Machine-Binding-Signal-Roles-and-State.md](specs/024-Machine-Binding-Signal-Roles-and-State.md) | Point → machine bindings with roles/qualifiers; I/O layers; counting at the source; `MachineActor` windows, epochs, state (incl. `Unknown`), point quality; outputs by role | Draft |
| 025 | [025-Upstream-Hook-and-Canonical-Events.md](specs/025-Upstream-Hook-and-Canonical-Events.md) | Generalizes 012: producer outboxes, critical/best-effort subscriptions, live/backfill lanes, event identity, clock policy, snapshot, consumer obligations | Draft |
| 026 | [026-Edge-Data-Policy-and-Retention.md](specs/026-Edge-Data-Policy-and-Retention.md) | Data classes and retention, ingest-throttle chain, sizing, backup and epoch-bumping restore | Draft |
| 027 | [027-Control-Plane-and-Edge-Node-Topology.md](specs/027-Control-Plane-and-Edge-Node-Topology.md) | `control` / `node` / `all` roles; `(control_epoch, version)` projections; node autonomy and local API; inbound routing; module changes | Draft |
| 028 | [028-Device-Protocol-v4-and-Commands.md](specs/028-Device-Protocol-v4-and-Commands.md) | AeroEdge-native device protocol (re-flashed ESP32 fleet); acknowledged device commands; request/response bridge for terminals; signed OTA; firmware conformance | Draft |

New specs take the next free number (`022-…`) and get a row here.
