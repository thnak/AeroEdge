# AeroEdge — Implementation Plan

> Draft v0.1. The phased build plan for everything specified in 001–015: the C++ runtime plane,
> the SDKs, the shared schema, the management API, the Studio, and the CLI. Sequenced by the
> dependency DAG, built as **vertical slices** (each phase runs end-to-end), and verified the way
> QuarkCpp verifies itself (GCC+Clang, ASan/UBSan/TSan, deterministic tests, benchmarks against
> budgets).

## 1. Strategy

- **Thin over Quark, always.** Every phase links the real `quark::quark` static lib and *uses* its
  primitives (Engine, ActorRef, StreamChannel, Persistent, Transport seam). We write the Flow/Node/
  Driver layer and the tooling — never a mailbox, scheduler, timer, or transport of our own.
- **Vertical slices, not horizontal layers.** Each phase delivers something that runs end-to-end
  (a flow executes, a driver ingests, a deploy lands), so integration risk is paid down early — the
  walking skeleton (013 §7) is Phase 1, not Phase 8.
- **Contract-first at every seam.** `aero-sdk` (INode/IDriver) and `aero-schema` (Application +
  config) are frozen early and versioned; everything else is built against them so parallel
  workstreams don't collide (013 T1/T3).
- **Verify like Quark.** Two compilers, three sanitizers, deterministic tests using Quark's TestKit
  (014-sim) + `InMemoryStore` + `LoopbackTransport`, and micro-benchmarks for the flow-execute hot
  path (mirrors Quark's 0-alloc / p99 gates). CI from Phase 0.

## 2. QuarkCpp readiness — the one hard external dependency

Quark's core is real and buildable **today** (verified: `Engine`, `ActorRef`, `register_actor`,
`StreamChannel`, `open_stream`, `Persistent`, `InMemoryStore` all present; builds as `quark::quark`).
But Quark is an RFC with some subsystems **design-settled + ADR-proven yet not fully productionized**
— notably the **real socket transport (019/021)** and some **cluster mechanics**. Consequences:

| AeroEdge need | Quark status | Plan response |
|---|---|---|
| Actor core, flow-in-handler, tell/ask | real, buildable | build against it now (Phases 1–5) |
| Streaming ingestion (024) | real (`StreamChannel`, `open_stream`) | build now (Phase 2) |
| Persistence (012) | real reference (`InMemoryStore`, `FileStore`) | build now; SQLite/Rocks opt-in later (Phase 3) |
| In-process / loopback transport | real (`LoopbackTransport`) | multi-node **tests** use it (Phases 7–8) |
| Real TCP/gRPC socket transport | ADR-proven, **adapter pending (019/021)** | **gate**: real cross-node runs wait on it; until then, loopback + our MQTT/gRPC adapters cover it |

**Rule:** where a Quark adapter isn't productionized, AeroEdge builds against the header API and runs
on Quark's reference/loopback implementations for tests, and the *production multi-node* exit
criteria for Phases 7–8 are explicitly gated on Quark's transport landing (tracked in §5 risks).

## 3. Project build order (the DAG)

```
aero-sdk ─┬─ aero-core ─┬─ aero-nodes ──┐
          │             ├─ aero-drivers ─┤
          │             ├─ aero-mes ─────┤
          │             └─ aero-runtime ─┴─ aero-api ─┬─ aero-cli
          │                                            └─ aero-studio (React, separate repo)
          └─ aero-sdk-native / aero-sdk-wasm (published)
aero-schema ── (feeds aero-runtime + aero-api + aero-studio; codegen TS/C++)
```

Dependencies flow one way (013 T5). `aero-studio` starts once `aero-api` is stable (after Phase 4)
and proceeds in parallel.

## 4. Phases

Each phase: **Goal · Deliverables · Specs realized · Exit criteria · Quark dependency.**

### Phase 0 — Foundations & scaffold
- **Goal:** a buildable, CI-gated superbuild linking Quark, with empty but compiling libs and the
  frozen SDK types.
- **Deliverables:** repo layout + CMake superbuild; `quark::quark` linked; `aero-sdk` skeleton
  (`INode`, `IDriver`, `ProcessingContext`, `NodeDescriptor`, `NodeResult`, C-ABI stubs) compiling;
  `CONVENTIONS.md`; CI matrix (GCC+Clang × Debug/Release × ASan/UBSan/TSan); a trivial test.
- **Specs:** 003, 005, 006, 013 (structure).
- **Exit:** `cmake --build` green on both compilers; a no-op test runs under all sanitizers; Quark
  sample 01 builds in our CI (proves the link).
- **Quark dep:** core headers only.

### Phase 1 — Flow core + walking skeleton  ★ the integration-risk retirement
- **Goal:** one `EdgeActor` runs a compiled 3-node flow triggered by a Command, on the real engine.
- **Deliverables:** `aero-core` — `ProcessingContext` (003), `CompiledFlow` + executor (004,
  sync-first), node registry, `EdgeActorBase` (001), Command→Flow→Event glue + event bus (002);
  `aero-nodes` — one Source, one Transform, one Output built-in.
- **Specs:** 001, 002, 003, 004, 005.
- **Exit:** a deterministic test feeds a Command, the flow walks Source→Transform→Output, state
  commits, an Event publishes — 0 heap alloc on the execute path (gate), passes under TSan. Mirrors
  Quark sample 03.
- **Quark dep:** Engine, Activation, ActorRef, register_actor — all real.

### Phase 2 — Drivers & streaming ingestion
- **Goal:** a real driver ingests device frames through Quark 024 into a flow, with backpressure.
- **Deliverables:** `aero-drivers` — a TCP driver (push, `BlockingHandler` lane) + a polled driver
  (pull, Quark timer); `StreamSink` binding to `StreamChannel`; frame-lifetime handling (006 §4).
- **Specs:** 006 (+ 024 usage).
- **Exit:** TCP driver streams N frames → flow; under a fast producer / slow consumer, credit
  backpressure stalls the driver with **zero dropped frames** (test); FIFO-per-stream verified.
- **Quark dep:** StreamChannel/open_stream (real), BlockingHandler (ADR-015).

### Phase 3 — State & persistence
- **Goal:** actor state is durable and survives restart/migration.
- **Deliverables:** wire `Persistent<Snapshot, Sync>` into `EdgeActorBase`; `commit` path; recovery
  on activation + driver re-open; stateful-node rule (007 §6) enforced; telemetry-not-persisted
  discipline.
- **Specs:** 007 (+ 012 usage).
- **Exit:** restart an actor → state recovers from `InMemoryStore`/`FileStore`; a `Sync` setpoint
  is durable before ack; a transient node rebuilds cold. Recovery test passes.
- **Quark dep:** Persistent, InMemoryStore/FileStore (real).

### Phase 4 — Runtime daemon + schema + management API + CLI
- **Goal:** deploy a declarative Application to a running daemon and observe it.
- **Deliverables:** `aero-schema` v1 (Application + per-plugin config schema, codegen TS+C++);
  `aero-runtime` daemon (boots Engine, loads an Application, instantiates actors/flows/drivers);
  `aero-api` (REST deploy/status/rollback + WS/SSE metrics, per 013 decision); `aero-cli`
  (`aero deploy`, `aero status`).
- **Specs:** 009 (deploy), 013 (api/cli/schema).
- **Exit:** `aero deploy app.json` stands up the Phase-1 flow in the daemon; `GET /status` and a WS
  metrics stream work; `aero-cli` drives it. This is the first *product-shaped* milestone.
- **Quark dep:** core + observability (009) snapshot API.

### Phase 5 — Deploy lifecycle: compile-at-deploy, hot-reload, versioning
- **Goal:** change a live actor's flow without dropping in-flight Commands.
- **Deliverables:** Flow Compiler validation (004 §2.1: acyclicity, slot types, node resolution);
  hot-reload via `quiesce(Drain)` + pointer-swap (009 §4); Live vs BuildOnly classification;
  Application versioning + rollback; state-compat check hook (009 §6 / 016 migration).
- **Specs:** 009.
- **Exit:** hot-swap a flow on a running actor under load → **0 dropped/duplicated Commands**
  (test); an invalid definition is rejected at deploy; a BuildOnly change forces re-activation.
- **Quark dep:** quiesce (015), live-reconfig (ADR-008), serialization migration (016).

### Phase 6 — Extension model (Native → WASM)
- **Goal:** load out-of-tree nodes/drivers behind the same seam.
- **Deliverables:** `aero-sdk-native` (C-ABI headers + helper) + native `.so` loader with ABI-version
  check; then `IWasmRuntime` seam + a WASM host (wasmtime **or** wasm3 — benchmark-decided) +
  `aero-sdk-wasm` guest bindings + capability gating (020); `ExprRuleNode` + expression DSL.
- **Specs:** 008 (+ 005 §8 resolution).
- **Exit:** a flow runs a node loaded from a `.so` and a node from a `.wasm`; a WASM node exceeding
  its fuel budget fails without hanging the worker; capability request/grant enforced.
- **Quark dep:** supervision guard (007), capabilities (020).

### Phase 7 — Transport plugins
- **Goal:** actors communicate over pluggable transports; cross-node streams over flow-controlled ones.
- **Deliverables:** confirm Local + Quark-TCP paths; `MqttTransport` (client to external broker, QoS≥1,
  **FIFO resequencing shim**, 014 §5) + `GrpcTransport` behind Quark's `Transport` seam; per-link
  transport policy (014 §8); cross-node stream over TCP/gRPC (014 §7).
- **Specs:** 014.
- **Exit:** two nodes exchange actor messages over MQTT with **per-sender FIFO preserved** (shim
  test); a cross-node stream over TCP holds end-to-end backpressure; MQTT-stream backpressure
  boundary behavior is explicit. *(Production multi-node gated on Quark 019/021 socket transport;
  until then loopback + our adapters over a local broker.)*
- **Quark dep:** Transport seam (real), **real socket transport pending (gate)**.

### Phase 8 — Distribution & horizontal-scale hardening
- **Goal:** device actors place, migrate, and rebalance across nodes safely.
- **Deliverables:** device-affinity placement policy over Quark capabilities/HRW (010 §2); fenced
  migration + driver re-dial on re-activation (010 §3, 006 §8); multi-node deployment via `aero-cli`.
- **Specs:** 010.
- **Exit:** a device actor migrates across nodes **fenced** (never dual-driven), recovers state,
  re-opens its driver, resumes flows — verified on a loopback cluster, then on real sockets when
  Quark lands them.
- **Quark dep:** placement/membership/fenced-hand-off (010/021/025), **socket transport (gate)**.

### Phase 9 — Studio (web) + plugin UI + runtime-assisted discovery  *(parallel from Phase 4)*
- **Goal:** design, configure, deploy, and monitor from the browser.
- **Deliverables:** `AeroStudio` React+Vite app — Flow Designer (emits Application graph, 009 §2);
  Tier-1 schema-driven config forms (015 §3); `aero-studio-sdk` (components + host API); one Tier-2
  custom UI (OPC UA address-space browser **or** Modbus register-map editor); deploy/rollout +
  live monitoring over `aero-api`; runtime-assisted `browse`/`test_connect` (015 §5).
- **Specs:** 013, 015 (+ 009/011/012 control surfaces).
- **Exit:** an operator builds a flow, configures a driver (incl. one live discovery), deploys, and
  watches metrics — entirely in the Studio. Tier-2 micro-frontend tech decided (015 §10).
- **Quark dep:** none directly (talks to `aero-api`).

### Phase 10 — MES hook, Firmware OTA, protocol breadth  *(breadth workstream, overlaps 6–9)*
- **Goal:** the domain-complete platform: MES integration, OTA, and the headline protocols full-featured.
- **Deliverables:** `aero-mes` (`IMesAdapter` + `MesGateway` + `RestMesAdapter` + transactional
  outbox, 012); Firmware OTA state machine + `FleetActor` staged rollout + rollback (011); protocol
  breadth — full MQTT, Modbus, OPC UA drivers + their Tier-2 config UIs; more nodes (average, FFT,
  CRC, JSON, threshold/switch).
- **Specs:** 011, 012 (+ 005/006/015 breadth).
- **Exit:** a `ProductionFinished` round-trips to a mock MES through the durable outbox (survives an
  MES outage); an OTA rollout canaries → commits with a forced-failure rollback; MQTT/Modbus/OPC UA
  each configurable end-to-end in the Studio and ingesting into a flow.
- **Quark dep:** delivery/outbox (017), security/secrets (020), governance (022).

## 5. Cross-cutting workstreams (run continuously, not a phase)

- **`aero-schema` evolution** — every plugin/config/Application change updates the canonical schema
  + codegen; migrations for config/state changes (015 §10, 009 §6).
- **Security (020 usage)** — signing/verification of bundles + Applications + firmware from Phase 6;
  principal propagation on the API from Phase 4; at-rest secrets from Phase 3.
- **Observability (009 usage)** — surface Quark metrics/traces through `aero-api` from Phase 4;
  trace ids threaded Command→Event from Phase 1.
- **Verification** — sanitizers + two compilers every phase; flow-execute micro-benchmark (0-alloc,
  p99) from Phase 1; deterministic sim tests (Quark 014) for concurrency-sensitive phases (5, 7, 8).
- **Docs** — keep 001–015 in sync as implementation reveals gaps; ADR-style records for any
  AeroEdge hot-path/design decision (mirror Quark's `decisions/`).

## 6. Sequencing, parallelism, and risks

- **Critical path:** Phase 0 → 1 → 2 → 3 → 4 → 5. These are strictly ordered (each builds on the
  prior's runtime surface). 4 is the first product-shaped milestone.
- **Parallelizable after Phase 4:** Studio (9), extension model (6), transports (7) — mostly
  independent given the frozen `aero-sdk`/`aero-schema` contracts. Protocol breadth (10) overlaps 6–9.
- **Top risks:**
  1. **Quark socket transport (019/021) not yet productionized** → gates *production* multi-node
     (7, 8). Mitigation: loopback + our MQTT/gRPC adapters carry cross-node until it lands; keep the
     dependency visible.
  2. **`ProcessingContext` field layout churn** as node categories grow (003 §6) → freeze the
     lifetime/ownership rules early (done in-spec), let fields evolve behind them.
  3. **Tier-2 UI isolation tech** (015 §10) — decide with the first real Tier-2 plugin, not before.
  4. **Config-schema expressiveness vs protocol reality** (Modbus register maps, OPC address space)
     → may need schema supersets; contain to `aero-schema`, don't leak into `aero-sdk`.

## 7. Suggested first move

Execute **Phase 0 + Phase 1 together** as the opening slice: scaffold the superbuild, freeze the
`aero-sdk` types, and stand up the walking skeleton (one `EdgeActor`, a compiled 3-node flow, a
passing deterministic test on the real Quark engine). That single slice validates the entire
"thin over Quark" thesis in running code and de-risks everything after it.
```
cmake -S . -B build && cmake --build build && ctest --test-dir build   # the Phase-0/1 green bar
```
