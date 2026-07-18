# AeroEdge — Architecture Specification (Overview)

> Draft v0.1. This is the orientation document: vision, principles, the core
> invariants every other spec is written against, and the glossary. Read this before
> the numbered specs.

## 1. Vision

An **Industrial Edge Platform** that:

- runs isolated **Edge Runtimes** (one per device / line / cell),
- ingests device data and processes it through **event-driven flows**,
- is extensible with an **SDK** (built-in, native, later WASM) — not a fixed rule engine,
- is **low-code + pro-code**: flows are declared as DAGs, node logic is compiled C++,
- achieves high performance by building on the **QuarkCpp** actor engine rather than
  reinventing a runtime.

**Mandated first-class requirements** (drive the specs, not afterthoughts):

- **Distribution & horizontal scale across nodes** — a deployment is a *cluster* of edge
  nodes; device actors are placed and rebalanced across them. → [010](010-Distribution-and-Horizontal-Scale.md)
- **Edge firmware OTA** — signed, staged, rollback-safe firmware updates to managed
  devices, orchestrated as fleet rollouts. → [011](011-Firmware-OTA.md)
- **MES integration hook** — a bidirectional seam (`IMesAdapter`) AeroEdge *defines*, so
  any MES plugs in without changing flows. → [012](012-MES-Integration-Hook.md)

## 2. What we build vs what we reuse

AeroEdge is deliberately **thin over Quark**. The line is bright:

| Concern | Owner |
|---|---|
| Mailbox, ordering, scheduling, work-stealing | **Quark** (001, 002) |
| Activation lifecycle (Created → … → Disposed) | **Quark** (001, ADR-008) |
| Timers, reminders, scheduled work | **Quark** (011, 027) |
| Isolation (single-executor, no shared memory) | **Quark** (001, 015) |
| Persistence (snapshot / event-sourced) | **Quark** (012) |
| Streaming ingestion (credit-ring, backpressure) | **Quark** (024) |
| Distribution, placement, membership | **Quark** (010, 025, 026) |
| **Flow Runtime — Command → DAG → Event** | **AeroEdge** |
| **Node SDK / `INode` contract** | **AeroEdge** |
| **ProcessingContext** | **AeroEdge** |
| **Drivers (PLC/MQTT/TCP/Serial/Camera)** | **AeroEdge** |
| **Domain actors (Edge/Line/Robot)** | **AeroEdge** (thin `quark::Actor` subclasses) |
| Distribution / placement / membership | **Quark** (010, 025, 026, 021) |
| **Device-affinity placement & rebalancing** | **AeroEdge** (policy over Quark, 010) |
| **Firmware OTA (state machine + fleet rollout)** | **AeroEdge** (011) |
| **MES integration hook (`IMesAdapter`)** | **AeroEdge** (012) |

If a design pressure ever pushes AeroEdge to reimplement a "reuse" row, that is a
signal to push the requirement *down into Quark*, not to fork it here.

## 3. Core concepts

**Actor** — an isolated runtime context (a `quark::Actor` subclass). Owns state and a
compiled Flow set. Knows only Commands, Events, and Flows — never PLC/MQTT/MES/Rule
directly. Examples: `EdgeActor`, `LineActor`, `RobotActor`.

**Flow** — the implementation of one behavior, expressed as a DAG of Nodes. Triggered
by a Command, an Event, or a Timer. A Flow contains no threads and manages no lifecycle;
it only orchestrates node execution and threads the `ProcessingContext`.

**Node** — the smallest unit of processing. Contract: `Input → Process → Output`. A Node
knows only its `ProcessingContext`. It never touches the Actor, the Flow, the scheduler,
or a thread. Four categories: **Source, Transform, Rule, Output**.

**Command** — a message that may mutate actor state. FIFO, sequential. Delivered via
Quark `tell`. Examples: `ReceivePacket`, `PollPLC`, `ConnectDevice`, `MESOrderReceived`.

**Event** — an immutable notification, published *after* a Flow completes. Never mutates
actor state. Examples: `TagChanged`, `AlarmRaised`, `ConnectionLost`, `ProductionFinished`.

**ProcessingContext** — a per-Command mutable struct threaded through a Flow. Not copied,
not serialized; it lives for exactly one Flow execution.

## 4. Runtime pipeline (per Command)

```text
Quark mailbox (FIFO)
      │
      ▼  handler runs (single-executor, run-to-completion)
Build ProcessingContext
      │
      ▼
Execute Flow  (compiled DAG: Source → Transform → Rule → Output)
      │
      ▼
Commit actor state
      │
      ▼
Emit Events  (published only after the Flow completes)
```

Commands are processed strictly one at a time per actor (Quark's single-executor
invariant). Events are published at the end, never mid-flow.

## 5. Invariants (load-bearing — every spec obeys these)

- **I1 — Actor owns the runtime.** Lifecycle, scheduling, and isolation are Quark's.
  AeroEdge code never spawns threads, never blocks a worker long, never sleeps.
- **I2 — One Command, one executor.** A Flow executes inside a single Quark handler
  invocation for one Command; no two Commands for the same actor run concurrently.
- **I3 — Flow is compiled once.** The DAG is built at deploy time. Execution walks a
  compiled plan; it never resolves or allocates the graph per Command.
- **I4 — Node logic is runtime-blind.** A Node sees only `ProcessingContext&`. It has no
  reference to Actor, Flow, Engine, or any thread primitive.
- **I5 — Command mutates, Event notifies.** Only Commands change state. Events are
  immutable and published post-flow.
- **I6 — Context is not copied or serialized.** `ProcessingContext` is passed by
  reference through the node chain and dies with the Flow execution.
- **I7 — The Node/Flow layer stays off Quark's mailbox hot path.** One virtual `INode`
  call per node at flow granularity is acceptable; it must never intrude on Quark's
  zero-RTTI/no-vtable message-dispatch path.
- **I8 — Extensions never control lifecycle.** Start/stop/restart of an Actor is Quark's
  decision, surfaced through Quark's supervision (007) — never a Node's.

## 6. The one open decision this draft takes a position on

**Flows are sync-first.** A Flow executes fully within one synchronous Quark handler.
Nodes that must do blocking or async I/O (Output → MES/DB/REST, Source → socket read)
do **not** block the flow: they hand off to a dedicated I/O actor via `tell`, or (later)
run as Quark coroutine/`BlockingHandler` nodes. Rationale and the coroutine evolution
path are in [004-Flow-Runtime-and-DAG.md](004-Flow-Runtime-and-DAG.md) §"Sync vs async".
This keeps `ProcessingContext` lifetime trivial (I6) and preserves Quark's zero-cost
sync path — while leaving a clean door to per-node async when a real workload needs it.

## 7. Glossary

| Term | Meaning |
|---|---|
| **Edge Runtime** | One isolated actor instance managing one device/line/cell |
| **Actor** | A `quark::Actor` subclass owning state + compiled Flows |
| **Flow** | A behavior expressed as a compiled DAG of Nodes |
| **Node** | Smallest processing unit; `INode::process(ProcessingContext&)` |
| **Command** | State-mutating message; FIFO; delivered via Quark `tell` |
| **Event** | Immutable post-flow notification |
| **ProcessingContext** | Per-Command mutable struct threaded through a Flow |
| **Driver** | A Source that bridges a device protocol into `Frame`s |
| **Trigger** | What starts a Flow: a Command, an Event, or a Timer |
| **DAG** | The compiled node graph a Flow executes |
