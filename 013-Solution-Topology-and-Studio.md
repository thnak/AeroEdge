# 013 — Solution Topology and Studio

> Draft v0.1. How AeroEdge decomposes into projects/modules, across three planes: the
> **Runtime** (the edge daemon + API), the **SDK / Extension** surface (what extension authors
> build against), and the **Studio** (the tooling to configure, build, extend, deploy, and
> monitor). This answers "how many projects do we need?" with a concrete, phaseable breakdown.

## 1. Three planes

```text
   ┌──────────────────────────── STUDIO plane (tooling) ────────────────────────────┐
   │  aero-studio        visual flow designer · SDK/ext builder · deploy · OTA · monitor │
   │  aero-cli           headless deploy/rollout/inspect (CI/CD)                          │
   └───────────────────────────────┬────────────────────────────────────────────────┘
                                    │ speaks the management API + Application schema
   ┌──────────────── SHARED ────────┴─────────────────────────────────────────────────┐
   │  aero-schema        Application/flow definition + canonical DTOs (language-neutral) │
   └───────────────────────────────┬────────────────────────────────────────────────┘
                                    │
   ┌──────────────────────────── RUNTIME plane (C++, on QuarkCpp) ──────────────────────┐
   │  aero-api           management/control API surface (deploy, status, OTA, metrics)   │
   │  aero-runtime       the edge daemon: hosts Quark engine, loads Applications, actors │
   │  aero-mes           IMesAdapter framework + built-in adapters (012)                 │
   │  aero-drivers       built-in drivers: TCP/MQTT/Modbus/OPC UA (006)                  │
   │  aero-nodes         built-in nodes: decode/scale/average/FFT/threshold/output (005) │
   │  aero-core          Flow Runtime, ProcessingContext, Flow compiler, registries      │
   │  aero-sdk           PUBLIC surface: INode/IDriver, descriptors, context view, C ABI  │
   └───────────────────────────────┬────────────────────────────────────────────────┘
                                    │ depends on
                            ┌───────┴────────┐
                            │   QuarkCpp     │  (actor engine — reused, not forked)
                            └────────────────┘
   ┌──────────────────── EXTENSION plane (out-of-tree, 008) ────────────────────────────┐
   │  aero-sdk-native    C-ABI headers vendors compile a .so/.dll against               │
   │  aero-sdk-wasm      guest-side SDK (Rust/C/AssemblyScript) → .wasm nodes            │
   └────────────────────────────────────────────────────────────────────────────────────┘
```

## 2. Runtime-plane projects (C++, this repo)

| Project | Responsibility | Depends on | Spec |
|---|---|---|---|
| **`aero-sdk`** | The **stable public surface**: `INode`, `IDriver`, `ProcessingContext` view, `NodeDescriptor`, `NodeResult`, the C ABI (008). What every node/driver — built-in or out-of-tree — compiles against. Deliberately small and slow-changing. | QuarkCpp (types only) | 005, 006, 008 |
| **`aero-core`** | The engine we build: Flow Runtime + compiler (004), ProcessingContext impl (003), node/driver registries (005/006), the `EdgeActorBase` glue (001), the event bus (002). | `aero-sdk`, QuarkCpp | 001–004 |
| **`aero-nodes`** | Built-in node library (Transform/Rule/Output/Source): decode, JSON parse, CRC, scale, average, FFT, threshold, switch, `ExprRuleNode`, MES/MQTT/DB/REST/Alarm outputs. | `aero-sdk`, `aero-core` | 005 |
| **`aero-drivers`** | Built-in drivers: TCP, Serial, MQTT, Modbus, OPC UA. | `aero-sdk`, `aero-core` | 006 |
| **`aero-mes`** | `IMesAdapter` framework + `MesGateway` actor + `RestMesAdapter`; site adapters loaded as extensions. | `aero-sdk`, `aero-core` | 012 |
| **`aero-runtime`** | The **deployable edge daemon** (the "runtime" you named): boots a Quark engine, loads a signed Application (009), instantiates actors/drivers/flows, hosts extension loaders (native/WASM, 008), owns OTA state machines (011). One binary per edge node. | all of the above | 009, 011 |
| **`aero-api`** | The **management/control API** (the "api" you named): deploy/rollback Application, query actor/flow/device status, drive OTA rollouts, stream metrics/logs. **REST+JSON** for request/response (deploy/status/OTA control); **WebSocket/SSE** for live metrics/logs/rollout progress (decided §9). The Studio and CLI talk to this. | `aero-runtime`, `aero-schema` | 009, 011 |

> `aero-sdk` is split out from `aero-core` on purpose: it is the **compatibility contract** for
> extensions (008 §2 ABI versioning). Extensions pin an `aero-sdk` version; `aero-core` can churn
> without breaking them.

## 3. Extension-plane projects (out-of-tree authoring, 008)

| Project | For | Ships |
|---|---|---|
| **`aero-sdk-native`** | vendors writing a trusted native driver/node | C-ABI headers + a thin C++ helper wrapping `INode`/`IDriver` to the C boundary (008 §2) |
| **`aero-sdk-wasm`** | anyone writing a sandboxed/marketplace node | guest-side bindings (Rust / C / AssemblyScript) to the host ABI + the capability declarations (008 §3) |

These are **published artifacts**, not part of the edge daemon build — an extension author
depends on them, compiles a bundle (008 §5), and deploys it through the API.

## 4. Shared plane

| Project | Responsibility |
|---|---|
| **`aero-schema`** | The **language-neutral contract** between Studio and Runtime: the Application/flow definition schema (009 §2) and the canonical MES DTOs (012 §3). One source of truth, codegen'd into C++ (runtime) and TS/C# (studio) so both sides can't drift. Likely JSON Schema or a small IDL. |

## 5. Studio plane (tooling — separate tech stack)

The **Studio** is the human-facing tool to *configure, build, extend, deploy, and monitor* —
distinct from the runtime it drives. **Decided (§9): a web app on React + Vite** (matches the
sibling `DemoUiVite` stack), talking to `aero-api` over REST+JSON (+ WS/SSE for live views) and
emitting `aero-schema` Applications.

| Project | Responsibility | Spec touchpoints |
|---|---|---|
| **`aero-studio`** | **React + Vite web app.** Visual **Flow Designer** (drag nodes, wire the DAG → emits the Application graph, 009 §2); **device/driver config**; **extension/SDK management** (browse, install, pin native/WASM bundles, 008 §5); **deployment & rollout control** (canary/staged, 009 §5); **OTA fleet management** (011 §4); **live monitoring** (metrics/traces from Quark 009, streamed over WS/SSE); **flow debugger/replay** (future, 009 §8). | 008, 009, 011, 012 |
| **`aero-cli`** | The headless equivalent for CI/CD and scripting: `aero deploy app.json`, `aero rollout --canary`, `aero ota push`, `aero status`. Same API as the Studio. | 009, 011 |

The Studio **produces** Application definitions and **consumes** the management API — it has no
privileged path into the runtime; everything goes through `aero-api` (so the API is the single
audited control surface, aligning with Quark 020 authz).

### "Extend SDK" in the Studio

You called out "build/extend SDK." Concretely, the Studio's SDK/extension role is:

- **Scaffold** a new node/driver project from `aero-sdk-native` / `aero-sdk-wasm` templates.
- **Build** it (invoke the toolchain locally or in CI) into a signed bundle (008 §5).
- **Register** it in a catalog/marketplace (008 §5) and **install** it into an Application by
  pinning `(name, version)`.

The Studio does not *host* extension code — it authors and packages it; the **runtime** loads it.

> **Per-protocol config UI.** Each transport/driver/node plugin (MQTT, OPC UA, Modbus…) must expose
> its *full* config, and each contributes its *own* UI — schema-driven forms for the simple fields, a
> custom micro-frontend (on `aero-studio-sdk`) for rich interactions (OPC UA address-space browser,
> Modbus register-map editor). The Studio host stays small; per-protocol weight is lazy-loaded per
> bundle. Live device discovery/test goes through `aero-api` → runtime → driver, never the browser.
> Full design in [015](015-Configuration-Model-and-Studio-Plugin-UI.md).

## 6. Repository strategy (recommendation)

- **Runtime + SDK + built-ins + API → one repo (this `AeroEdge` repo), many CMake targets.** They
  share the `aero-sdk` contract, version together, and are built by the same C++ toolchain. A
  mono-repo of libraries keeps the `aero-sdk`↔`aero-core`↔built-ins boundaries honest and testable.
- **`aero-studio` → its own repo (`AeroStudio`).** React + Vite, different toolchain and release
  cadence, talks only over the API — no reason to couple its build to the C++ runtime.
- **`aero-schema` → shared, single source of truth.** Either a small repo both sides submodule, or
  it lives in `AeroEdge` and the Studio consumes generated artifacts. Recommend: **home it in
  `AeroEdge`, publish generated TS/C# types** so the C++ side (which the runtime enforces) is
  canonical.
- **QuarkCpp stays its own repo** (already is), consumed by `AeroEdge` as an external dependency.

Net: **~2 repos to start** (`AeroEdge` for the whole runtime plane + schema; `AeroStudio` for
tooling), with the extension SDKs published as artifacts out of `AeroEdge`.

## 7. Build phasing — what a first cut needs

Not all 9 runtime projects at once. Minimum walking skeleton (matches the earlier scaffold idea):

1. `aero-sdk` (INode/IDriver/context) + `aero-core` (Flow compiler + executor) — the heart.
2. A handful of `aero-nodes` (a Source, a Transform, an Output) + one `aero-drivers` (TCP).
3. `aero-runtime` hosting one `EdgeActor` running a compiled 3-node flow on QuarkCpp.
4. A trivial `aero-api` (`POST /apps` to deploy a JSON Application; `GET /status`) + `aero-cli`.

`aero-mes`, native/WASM loaders, OTA, and the Studio layer on **after** that skeleton runs — each
is an additive module against the same `aero-sdk` contract.

## 8. Invariants (normative)

- **T1** — `aero-sdk` is the extension compatibility contract; it changes slowly and is versioned
  independently of `aero-core` (008 §2).
- **T2** — the Studio and CLI reach the runtime **only** through `aero-api`; there is no side channel
  (single audited control surface, Quark 020).
- **T3** — the Application/DTO schema (`aero-schema`) has one source of truth, codegen'd to every
  consumer; Studio and runtime cannot drift.
- **T4** — the Studio authors and packages extensions; it never hosts/executes edge extension code —
  the runtime loads it (008).
- **T5** — every project's dependencies flow **downward** in §1 (Studio → API → runtime → core →
  sdk → Quark); no upward or cyclic dependency.

## 9. Decisions and open questions

**Decided:**

- **Studio platform → web app on React + Vite** (matches sibling `DemoUiVite`): cross-platform,
  central hosting, natural for fleet monitoring and remote deployment. A future thin desktop
  companion for hardware-local commissioning is not precluded, but is not planned.
- **Management API → REST+JSON for request/response, WebSocket/SSE for live streams** (metrics,
  logs, rollout/OTA progress). Trivially consumable by the web Studio, the CLI, and curl; no
  grpc-web proxy needed. `aero-schema` DTOs codegen to TS for the Studio and C++ for the runtime.

**Still open:**

- **CLI language** — reuse the C++ runtime libs (link an `aero-api` client) vs a standalone
  Go/Rust binary for portability. Minor; defer.
- **Marketplace hosting** — where signed bundles live (an artifact registry); ties to 008 §5.
  Defer until extensions are real.
- **Studio ↔ runtime deployment topology** — does the Studio talk to each edge node's `aero-api`
  directly, or to a central control-plane service that fans out to nodes? Fleet scale (010) will
  decide; a central control plane is likely once node counts grow. Defer.
