# 019 — Flow Graph Model and Studio Canvas API

> Draft v0.1. Upgrades the Flow Designer from a linear list dressed up as a canvas (Phase 11.8:
> `@xyflow/react` rendering `Application.flow: NodeSpec[]` with dragging/connecting disabled) to a
> real node-graph — drag, wire, branch/merge, Node-RED/EMQX-style — while keeping 004's compile-once,
> zero-alloc execution invariant untouched. Also narrows 008/015's plugin ambition: built-in
> nodes/drivers stay the default; the extension ABI is kept as a boundary, not grown into an active
> marketplace, until a real third-party need shows up (decided in discussion, 2026-08-18).

## 1. Where this starts from (grounded in current code, not aspiration)

- **Schema** (`include/aero/schema/application.hpp`): `Application.flow` is `std::vector<NodeSpec>`,
  strictly ordered — array order **is** the DAG (004 v0.1: linear + single switch). No node `id`, no
  edges, no ports.
- **Studio canvas** (`studio/src/FlowDesigner.tsx`, `FlowCanvasNode.tsx`, Phase 11.8): already renders
  through React Flow, but `nodesDraggable={false}`, `nodesConnectable={false}`,
  `edgesReconnectable={false}` — a presentation upgrade over the same array, with ↑/↓/✕ buttons doing
  what array splice used to do. The code comments say this plainly: *"this card doesn't let a user
  express branching the schema doesn't support."*
- **`aero-api`** (`include/aero/api/rest_api.hpp`): **`GET /catalog` now exists** (delivered ahead of
  the rest of this ADR, in the standalone catalog-drift-fix slice — see §7) — but `POST /flows/validate`
  and per-step trace are still gaps.
- **`INode`/`NodeDescriptor`** (`include/aero/sdk/node.hpp`): `{category, type_id}` only — no ports.
  005 already deferred this: *"Slots/versioning (005 §3) are added when the compiler needs them."*
- **Runtime execution** (004 §2.2, §3): compile-once invariant **I3** — `CompiledFlow::execute` walks
  a pre-built step array, 0 allocation, 0 graph resolution, 1 virtual call per node. This is not just
  documented; it's CI-gated today (`flow_zero_alloc` in `tests/nodes/breadth_nodes.cpp`,
  `include/aero/nodes/compute_nodes.hpp`, wired in `tests/CMakeLists.txt`, per `CONVENTIONS.md`
  "Invariant gates are pass/fail tests"). **This ADR does not touch I3** — it extends what the compile
  phase is allowed to produce, not what the execute phase is allowed to do.

004 §7 already named the gap this ADR closes: *"Branch/fan-out/merge semantics beyond linear + single
switch — revision 2."* This is that revision, paired with the Studio-facing graph API needed to author
it.

## 2. Schema — from ordered array to a real graph (extends 004 §7, 013 T3)

`aero-schema`'s `Application` gains:

```jsonc
{
  "flow": [
    { "id": "decode1", "type_id": "aero.source.decode", "config": {} },
    { "id": "scale1",  "type_id": "aero.transform.scale", "config": { "factor": 2 } }
  ],
  "edges": [
    { "from": "decode1", "from_port": "out", "to": "scale1", "to_port": "in" }
  ]
}
```

- Every `NodeSpec` gets a required `id` (stable string, unique within the flow) — today's array index
  is the implicit id; this makes it explicit and edge-addressable.
- `edges: EdgeSpec[]` replaces "array order is the DAG." An edge is `{from, from_port?, to, to_port?}`;
  omitted ports default to a node's single implicit in/out (keeps today's linear flows expressible
  with zero edges boilerplate — see §6 migration).
- `NodeDescriptor` (005) gains **typed ports**: `{name, kind}` where `kind` is a coarse tag (`scalar`,
  `frame`, `event` — matching what 004 §2.1 already calls "types match"). Full type system is
  explicitly out of scope for v0.1; start coarse, grow if a real mismatch bug demands it.

This is a schema-shape change, not a philosophy change — `aero-schema` stays the one canonical
contract (013 T3), codegen'd to TS/C++ as before.

## 3. Compile phase — real branch/merge, same zero-alloc execute (extends 004 §2–§3)

The Flow Compiler's job grows but stays entirely at compile time:

1. **Validate**: DAG acyclic (unchanged rule, now over `edges[]` instead of implicit adjacency),
   every `to` port resolved from some `from` port, port `kind` compatibility, exactly one trigger
   node (unchanged, 004 §5).
2. **Topological order → still a flat step array.** This is the load-bearing constraint: even with
   real fan-out/fan-in, execution stays `for (Step& step : steps_) step.node->process(ctx)` — no
   graph walk at runtime. Extend `Step` with a `StepKind`:
   - `Linear` — today's shape, unconditional next step.
   - `Fanout` — N downstream steps all enabled (both branches run); compiler lays out all N in
     topological order with no runtime branching.
   - `ConditionalBranch` — a Rule/switch node sets a context flag; downstream steps whose branch flag
     doesn't match are skipped (004 §2.2 already sketches this for the single-switch case — this
     generalizes it, doesn't invent a new mechanism).
   - `Merge` — a step whose node needs >1 input; requires the multi-input slots below to be already
     populated before it runs (compiler ensures ordering).
3. **Multi-input context slots** (extends 003 §6, still an open item there — "typed struct vs
   type-erased; decides plan layout"): a merge node reads N named input slots from
   `ProcessingContext`, laid out once at compile time, same as today's single-input path. No
   allocation, no map lookup on the hot path — this ADR is the concrete reason 003 §6 needs to close.

No wait/timeout semantics for merge in v0.1 (see open questions, §10) — a merge step assumes every
upstream producer runs earlier in the same synchronous walk (true for fan-out from one trigger; not
true for merging across independently-triggered flows, which is out of scope here).

## 4. Studio canvas — turning the existing React Flow instance into a real editor

`FlowDesigner.tsx` already depends on `@xyflow/react`; this is a mode change, not a new dependency:

- Flip `nodesDraggable`/`nodesConnectable`/`edgesReconnectable` to `true`; wire `onConnect` to append
  an `EdgeSpec`, `onNodesChange`(drag) to update layout (next bullet), `onNodeClick` keeps today's
  select-to-configure behavior.
- `FlowCanvasNode` grows from one Top/Bottom `Handle` pair to **named handles per port**, positioned
  from the node's `NodeDescriptor.ports` (served by `GET /catalog`, §5) — this is the direct Node-RED
  parallel (typed connectors, not one generic wire).
- **Node position is UI-only state, never runtime input.** Store it as a Studio-side sidecar the
  runtime never parses — e.g. `application.studio_meta.layout: {[nodeId]: {x, y}}` — a field the
  Flow Compiler explicitly ignores. This keeps 013 T3 ("Studio and runtime cannot drift") true: a
  layout change alone must never be a redeploy-worthy diff.
- Palette: today's `<select>` "+ Add node…" becomes a proper category sidebar (Source/Transform/
  Rule/Output, matching `NodeCategory` from `node.hpp`), drag-to-canvas instead of dropdown-to-append
  — same catalog data, different affordance.
- Node-RED-style **inject/debug node**: a way to push one sample payload at a chosen node and see it
  propagate, without a full deploy. Needs a runtime hook — flagged, not committed, in §5/§10.
- EMQX-style **live trace overlay**: while `/metrics/stream` is open, highlight the last-active
  step/edge on canvas. Needs per-step trace data the runtime doesn't expose yet — new endpoint, §5.

## 5. New `aero-api` surface (extends 013 §9's REST+JSON / SSE decision, mirrors 016's route style)

| Route | Purpose | Status |
|---|---|---|
| `GET /catalog` | Serve `NodeDescriptor`/`DriverDescriptor` (type_id, category, config schema) straight from the runtime's own registry (013 §2 `aero-nodes`/`aero-drivers`). | **Delivered** (standalone slice, §7) — ports aren't in the schema yet since `NodeDescriptor` has no port concept until §2's graph work lands; today's `fields` cover config only |
| `POST /flows/validate` | Run §3.1–2 (validate + topo-sort) against a candidate graph **without** binding nodes or deploying — live-as-you-wire feedback, same two-stage-validation posture as 015 §7 extended to graph structure (dangling port, type mismatch, cycle, multiple triggers). | New, straightforward |
| `GET /flows/{name}/trace` (SSE) | Per-step last-result + timestamp for the currently deployed flow, feeding the canvas trace overlay (§4). Additive; does not change `/metrics/stream`'s existing shape (016 S1: "new REST routes are additive only"). | New, needs a per-step counter added to `CompiledFlow`/`Runtime::status()`-adjacent state |
| `POST /flows/{name}/debug-run` | Inject one sample payload through the compiled flow off the live path, for the inject/debug affordance (§4). | **Not committed for v0.1** — needs a sandboxed execution mode in `Runtime` that doesn't touch live actor state; real design work, tracked as an open question (§10) |

All four follow the existing pattern in `rest_api.hpp`: thin route → one `Runtime`-owned method, JSON
body/response, `{"error": msg}` + 4xx on failure — no new pattern introduced (R1, 016 §3).

## 6. Migration — today's linear flows keep working

- A `flow[]` with no `edges[]` is still valid: the compiler falls back to "array order is the DAG,
  each node's implicit output feeds the next node's implicit input" — exactly today's v0.1 behavior.
  This means `examples/hello_flow.json` and its round-trip test (`application.test.ts`) do **not**
  need to change to keep passing; `edges[]` is the new, optional way to express anything beyond
  linear.
- Node `id` becomes required going forward; for a `flow[]` with no explicit `id`s, the loader
  synthesizes one from array index (`"n0"`, `"n1"`, …) so old JSON without `id` still parses.
- This is additive to `aero-schema`, not a version bump on its own — 009's flow-versioning story is
  unaffected; a graph that actually branches is a new *capability* an Application opts into by adding
  `edges[]`, not a breaking change to the wire shape.

## 7. Utility nodes — delivered vs. planned (per discussion, 2026-08-18)

The request that motivated closing the catalog-drift gap also asked for a few "utility" nodes: branch,
parallel (fan-out), merge, and an HTTP output. These split cleanly by whether they need §2/§3's graph
model or not:

- **`aero.output.http` — delivered**, in the standalone catalog-drift-fix slice alongside `GET /catalog`
  (not gated on this ADR at all): an ordinary `Output`-category node, same shape as `aero.output.mes`
  (`nodes/http_output_node.hpp`). It stages a `StagedHttpRequest` (`sdk/processing_context.hpp`); a new,
  deliberately non-durable `HttpEgressActor` (`egress/http_egress_actor.hpp`, its own `aero-egress`
  library) delivers it off the flow (I1) — explicitly NOT `aero-mes`'s durable-outbox shape, since a
  generic HTTP utility node has no at-least-once requirement by default. `Runtime::configure_http_egress()`
  / `http_send()` mirror `configure_mes()`/`mes_stage()`'s exact scope, including the same honest gap:
  no live-flow-actor auto-forwarding is wired yet for either (a test/caller drives the hand-off
  manually) — this ADR's §3/§4 branch work does not change that.
- **`aero.flow.switch` (branch), `aero.flow.fanout` (parallel), `aero.flow.merge` — planned, NOT
  functional yet.** Each needs a node with more than one next/previous step, which today's linear
  `flow[]` (even with §2's `edges[]` extension not yet built) cannot express. They are blocked on §2's
  schema (`edges[]`, node `id`) and §3's `StepKind` compiler work landing first. Rough field-schema
  sketch for when that happens:
  - `aero.flow.switch` — `expr` (String, an `ExprRuleNode`-style condition) + N labeled outputs (the
    `ConditionalBranch` `StepKind`, §3).
  - `aero.flow.fanout` — no config; N static outputs, all enabled (the `Fanout` `StepKind`, §3).
  - `aero.flow.merge` — input count + a strategy field, directly blocked on §10's open "multi-input merge
    semantics" question (wait-for-all vs first-of-N vs windowed join) — do not implement before that's
    decided.

  Do not register any of these three in `register_builtins()` before §2/§3 land — a node claiming to
  branch/merge with no compiler support behind it would be exactly the kind of overstated-completion gap
  R5/S4 (016) exist to prevent.

## 8. Plugin-scope narrowing (per discussion — recorded here, not re-litigated)

- 008 (Extension Model) and 015 (Plugin UI) stay correct at the **mechanism** level — the
  native/WASM ABI boundary and the Tier-1/Tier-2 config-UI split are still the right shape for
  AeroEdge's own built-in nodes/drivers (013 §2: `aero-nodes`, `aero-drivers`).
- What narrows: **no marketplace, no third-party signed-bundle distribution, no dynamically-loaded
  Tier-2 UI** in the near term. Every catalog entry is built-in, ships in the same repo/binary.
- Concrete effect on this ADR: `GET /catalog` (§5) reads the runtime's in-process registry — no
  bundle-loading/discovery path needs to exist for the canvas to work. If/when a real external
  extension shows up, `GET /catalog` is already the right shape to grow into serving it too.

## 9. Invariants (normative)

- **G1** — graph validation + topological ordering is compile-time only (extends 004 I3); execution
  never resolves the graph, allocates, or looks up a node by id/name. `flow_zero_alloc` (or its
  extension for branch/merge flows) must stay green.
- **G2** — node position/layout is Studio-only metadata (`studio_meta`); the Flow Compiler and
  runtime never read it, never fail on its absence, never let it affect deploy semantics.
- **G3** — `GET /catalog` is generated from the runtime's actual registry; once it ships, the Studio
  never hand-maintains a duplicate catalog (retires `catalog.ts`'s hardcoded arrays).
- **G4** — new routes (§5) are additive; `/apps`, `/status`, `/metrics/stream` shapes are unchanged
  (mirrors 016 S1).
- **G5** — branch/merge compiles to `StepKind` skip-flags + pre-laid-out multi-input context slots,
  never a runtime graph walk (extends 004 I3, closes 003 §6's open item).
- **G6** — a `flow[]` with no `edges[]` keeps today's linear semantics exactly; this ADR must not
  break `examples/hello_flow.json` or its round-trip test.

## 10. Open questions

- **Multi-input merge semantics** — wait-for-all vs first-of-N vs windowed join. Blocks finalizing
  3§6/003 §6's context slot layout and §7's `aero.flow.merge`; needs a decision before implementation
  starts.
- **`debug-run` sandboxed execution** (§5) — running a compiled flow against one sample payload
  without touching live actor/persistence state. Real design work (does it reuse the actor's
  `CompiledFlow*` read-only? a scratch `ProcessingContext` only?) — not committed for v0.1.
- **Port type granularity** — coarse `scalar`/`frame`/`event` tags (chosen for v0.1) vs a fuller type
  system later; revisit only if a real mismatch bug demands it, per 004 §2.1's existing precedent.
- **Trace endpoint cost** — per-step counters on every `CompiledFlow` step add a small amount of
  state/writes to the hot path; needs to be measured against `flow_zero_alloc`'s CI gate (an atomic
  counter increment is not an allocation, but it is a write — confirm it's cheap enough to always-on,
  or make it opt-in per deployed flow).
- **Fan-out semantics vs `NodeResult::Stop`** — today a `Stop` short-circuits a linear walk (004
  §2.2); with real fan-out, does `Stop` on one branch stop only that branch or the whole step array?
  Needs a decision alongside `StepKind::Fanout` (§3).
