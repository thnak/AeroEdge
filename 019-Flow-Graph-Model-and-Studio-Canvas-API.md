# 019 — Flow Graph Model and Studio Canvas API

> Draft v0.2. Upgrades the Flow model from a linear list to a real graph — branch/fan-out/merge,
> Node-RED-style — while keeping 004's compile-once, zero-alloc execution invariant untouched. **§2/§3
> (schema `edges[]` + Flow Compiler topo-sort/branch-routing) and §7's `aero.flow.switch` are
> DELIVERED (2026-08-18)** — built smaller than originally drafted: fan-out and merge needed zero new
> machinery once it was confirmed nodes share one mutable `ProcessingContext` rather than having real
> per-port data (§2/§3). §4 (Studio canvas editing) and §5's other routes (`POST /flows/validate`,
> trace SSE, `debug-run`) remain undelivered, separate follow-up work. Also narrows 008/015's plugin
> ambition: built-in nodes/drivers stay the default; the extension ABI is kept as a boundary, not
> grown into an active marketplace, until a real third-party need shows up (decided in discussion,
> 2026-08-18).

## 1. Where this starts from (grounded in current code, not aspiration)

- **Schema** (`include/aero/schema/application.hpp`): **as of this ADR, `NodeSpec` has an `id` and
  `Application` has an optional `edges: EdgeSpec[]`** (§2) — grounding below describes the state
  BEFORE this ADR, kept for context on why §2/§3 were shaped the way they were.
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

## 2. Schema — from ordered array to a real graph (extends 004 §7, 013 T3) — DELIVERED

`aero-schema`'s `Application` gained (`include/aero/schema/application.hpp`):

```jsonc
{
  "flow": [
    { "id": "decode1", "type_id": "aero.source.decode", "config": {} },
    { "id": "scale1",  "type_id": "aero.transform.scale", "config": { "factor": 2 } }
  ],
  "edges": [
    { "from": "decode1", "from_port": "true", "to": "scale1" }
  ]
}
```

- Every `NodeSpec` gets an `id` (stable string, unique within the flow) — `load_application` always
  populates it: explicit from JSON if present, else synthesized `"n<index>"`.
- `edges: EdgeSpec[]` replaces "array order is the DAG" when non-empty. An edge is
  `{from, from_port?, to}` — **no `to_port`**, and **no ports on `NodeDescriptor`**. Both were in the
  original draft of this section and were cut before implementation: nodes have no real per-input
  slot at all today — every `INode::process(ProcessingContext&)` reads/writes the SAME shared
  `ctx.tags`/`ctx.output`/`ctx.events` buffers (confirmed by re-reading `core/compiled_flow.hpp`/
  `sdk/node.hpp` before writing code, agreed with the user 2026-08-18). An edge only needs to say
  which node runs next and under what branch condition — there is no per-node "input" to address, so
  a `to_port` would have had no meaning. `from_port` is empty for an unconditional edge, or a branch
  label (`"true"`/`"false"` in v1 — the only source is `aero.flow.switch`, §5) for a conditional one.

This is additive to `aero-schema` — `examples/hello_flow.json` and its round-trip test are untouched
(`edges` empty → the exact pre-existing linear path, §6/G6).

## 3. Compile phase — branch/fan-out, same zero-alloc execute (extends 004 §2–§3) — DELIVERED

The Flow Compiler (`include/aero/runtime/flow_compiler.hpp`) grows but stays entirely at compile
time — no `StepKind` enum, because fan-out and merge turned out to need none:

- **Fan-out is free.** Any node may already have >1 outgoing edge; `edges[]` alone expresses it, no
  dedicated node type or compiler case needed.
- **Merge is free.** Two branches writing into the shared `ctx` have already "merged" by the time a
  downstream node runs — a merge point is purely a *topological-order* guarantee (the compiler
  schedules it after every declared upstream branch), not a data-routing concern. This resolved §10's
  original "multi-input merge semantics" open question outright: since execution is one synchronous
  array walk (nothing runs concurrently within a flow execution), there is no runtime "wait" to
  arbitrate — wait-for-all/first-of-N/windowed-join are async-dataflow concepts that don't apply here.
- **Branching is the one real addition**, via `order_flow_graph()`, run only when `app.edges` is
  non-empty:
  1. Duplicate-id check.
  2. Edge-endpoint check (`from`/`to` must resolve to a known id).
  3. **Kahn's algorithm** does topo-sort AND cycle detection in one pass — nodes left over after the
     queue drains are a cycle.
  4. **Root check**: exactly one zero-in-degree node, and it must be the flow's Source-category node
     (generalizes the pre-existing `has_source` check into "the graph's root IS the source").
  5. **Branch-label assignment**: for each node, the distinct `from_port` values across its incoming
     edges must be either all-empty (unconditional) or a single non-empty label; a MIX (e.g.
     `{"", "true"}`) is rejected — "not supported yet" rather than silently mishandled (P1).
  6. **Single branch source** (added after `/code-review` caught it): `ctx.active_branch` is one
     field, not a stack, so at most one node in the graph may be the source of a labeled edge. A
     second independent `aero.flow.switch` compiles but at runtime one switch's decision would
     silently overwrite the other's before its own labeled steps are checked — rejected at deploy
     instead ("flow has more than one branch-producing node").
- `CompiledFlow::Step` (`core/compiled_flow.hpp`) gained one field: `std::string_view required_label`.
  `execute()` skips a step when `!required_label.empty() && required_label != ctx.active_branch` — an
  O(1) `string_view` compare, 0-alloc, no graph resolution (I3 untouched). `ctx.active_branch`
  (`sdk/processing_context.hpp`) is a new `string_view` field, set by `aero.flow.switch` to a static
  `"true"`/`"false"` literal (§5) — nothing else writes it. Cleared in `ProcessingContext::reset()`
  alongside every other per-Command buffer (`/code-review` caught this being missed on the first pass
  — a reused context leaking a stale branch decision into the next Command would have been a real,
  if latent, bug).

**`Stop` keeps its exact existing behavior**: it aborts the whole remaining step array, even under
fan-out — a documented v1 limitation (§10 used to carry this as an open question; it's now a
conscious, recorded choice, not something left undecided), not a bug.

## 4. Studio canvas — turning the existing React Flow instance into a real editor

`FlowDesigner.tsx` already depends on `@xyflow/react`; this was a mode change, not a new dependency.
**Delivered** (frontend-only slice — the backend already accepted everything this emits, since
`load_application` silently ignores unknown top-level keys):

- `nodesDraggable`/`nodesConnectable`/`edgesReconnectable` flipped to `true`; `onConnect` appends a
  `GraphEdge` (`application.ts`), `onEdgesDelete` removes one, `onNodesChange` tracks drag position,
  `onNodeClick`(via the card's own `onSelect`) keeps the existing select-to-configure behavior.
- A flow with no manually-drawn connection stays in **legacy mode**: `model.edges` is empty, array
  order is still the DAG, and `toApplication` emits the exact historical minimal shape (no `id`, no
  `edges` key) — the hello_flow round-trip test is untouched. The first drawn/deleted connection
  **materializes** `model.edges` from the current implicit `n[i]->n[i+1]` chain plus the edit, and the
  model stays in graph mode from then on (mirrors this section's own §6 G6 migration story, applied
  symmetrically client-side).
- `FlowCanvasNode` grows from one Top/Bottom `Handle` pair to **named handles per branch label** —
  NOT a `NodeDescriptor.ports` concept (§2 cut that: nodes have no real per-input slot to advertise).
  Handles are a Studio-only presentation detail — every node gets one generic target + one generic
  source handle, except `aero.flow.switch`, which trades its one generic source handle for a labeled
  "true"/"false" pair. Node-RED-style, but thinner: the backend doesn't route distinct data per
  handle, only a branch label (§3). (Category-based handle-hiding — e.g. no source handle on an
  Output node — was tried and reverted: it broke the implicit-chain fallback whenever an Output node
  sits mid-array, a legal legacy-mode pattern, e.g. `aero.output.mes` as a side-effect that doesn't
  stop the flow.)
- **Node position is session-only client state (`FlowDesigner`'s own `positions`), never part of
  `FlowModel`/the wire `Application`** — not the `application.studio_meta.layout` sidecar this section
  originally proposed. Reasoning: nothing in the Studio re-fetches a deployed Application to
  repopulate the Designer today (no `GET /apps/{name}` load-back path exists), so a persisted layout
  would never be read back; worse, if `toApplication` emitted *any* layout — even a freshly
  auto-computed one — it would break `application.test.ts`'s exact-shape round-trip guarantee. A
  layered topological auto-layout (depth = longest path from a root) is the position default;
  dragging overrides it for the session. Revisit the sidecar approach if/when a load-back path exists.
- **Not delivered, still out of scope** (unchanged from this section's original text): the palette
  stays today's `<select>` "+ Add node…" dropdown rather than a category sidebar with drag-to-canvas;
  the Node-RED-style inject/debug node and the EMQX-style live trace overlay both still need runtime
  hooks this slice didn't build (§5/§10).

## 5. New `aero-api` surface (extends 013 §9's REST+JSON / SSE decision, mirrors 016's route style)

| Route | Purpose | Status |
|---|---|---|
| `GET /catalog` | Serve `NodeDescriptor`/`DriverDescriptor` (type_id, category, config schema) straight from the runtime's own registry (013 §2 `aero-nodes`/`aero-drivers`). | **Delivered** (standalone slice, §7) — no ports in the schema; §2 confirmed `NodeDescriptor` never gets a port concept, `fields` covers config only |
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

## 7. Utility nodes — delivered, and two retired (per discussion, 2026-08-18)

The request that motivated closing the catalog-drift gap also asked for a few "utility" nodes: branch,
parallel (fan-out), merge, and an HTTP output.

- **`aero.output.http` — delivered**, in the standalone catalog-drift-fix slice alongside `GET /catalog`
  (not gated on this ADR at all): an ordinary `Output`-category node, same shape as `aero.output.mes`
  (`nodes/http_output_node.hpp`). It stages a `StagedHttpRequest` (`sdk/processing_context.hpp`); a new,
  deliberately non-durable `HttpEgressActor` (`egress/http_egress_actor.hpp`, its own `aero-egress`
  library) delivers it off the flow (I1) — explicitly NOT `aero-mes`'s durable-outbox shape, since a
  generic HTTP utility node has no at-least-once requirement by default. `Runtime::configure_http_egress()`
  / `http_send()` mirror `configure_mes()`/`mes_stage()`'s exact scope, including the same honest gap:
  no live-flow-actor auto-forwarding is wired yet for either (a test/caller drives the hand-off
  manually) — §3's branch work does not change that.
- **`aero.flow.switch` — delivered** (`nodes/switch_node.hpp`, `NodeCategory::Rule`). The one utility
  node that needed new runtime machinery — it *chooses* a path, which `process() -> NodeResult` alone
  can't signal. Config `{expr}` (same DSL as `aero.rule.expr` — reuses
  `expr_detail::Program::evaluate()`, factored out of `ExprRuleNode` into a public method on `Program`
  itself so the two nodes share one 0-alloc RPN evaluator instead of duplicating it). `process()`
  evaluates `expr` and sets `ctx.active_branch` to the static literal `"true"` or `"false"` (§3),
  always returns `Continue` — unlike `aero.rule.expr`, a switch never stops the flow, it only routes.
  v1 is binary only (no N-way case labels) — matches `ExprRuleNode`'s existing boolean-expression
  precedent exactly; N-way switching is future work if a real need shows up.
- **`aero.flow.fanout` and `aero.flow.merge` — retired, not built, because they turned out to be
  unnecessary as distinct node types.** §3 explains why: any node can already have >1 outgoing edge
  (fan-out), and two branches writing into the shared `ProcessingContext` have already merged by the
  time a downstream node runs (merge). Neither needs config, a factory, or a `NodeCategory` — adding
  them would be pure ceremony over what `edges[]` + topo-sort already does for free. If a Studio
  canvas ever wants an explicit visual "this is where branches merge" marker, that is a presentation
  concern (§4's Studio-canvas work, separate and not yet built) — it does not need a backend node.

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

- **G1** — graph validation (`order_flow_graph`) + topological ordering is compile-time only (extends
  004 I3); execution never resolves the graph, allocates, or looks up a node by id/name. Confirmed:
  full `ctest` suite (50/50, including `flow_zero_alloc`) green after this landed.
- **G2** — node position/layout is Studio-only metadata (`studio_meta`); the Flow Compiler and
  runtime never read it, never fail on its absence, never let it affect deploy semantics. (Still
  applies to the not-yet-built §4 Studio-canvas work.)
- **G3** — `GET /catalog` is generated from the runtime's actual registry; the Studio never
  hand-maintains a duplicate catalog (retires `catalog.ts`'s hardcoded arrays). Delivered.
- **G4** — new routes (§5) are additive; `/apps`, `/status`, `/metrics/stream` shapes are unchanged
  (mirrors 016 S1).
- **G5** — branch routing compiles to a single `string_view required_label` per `CompiledFlow::Step`,
  checked with an O(1) compare at execute time — never a runtime graph walk, never a per-node input
  slot (extends 004 I3). Merge needs no compile-time OR runtime state at all beyond topological order
  (closes 003 §6's open item on this axis: no typed/type-erased scratch was needed for this work).
- **G6** — a `flow[]` with no `edges[]` keeps today's linear semantics exactly; `examples/hello_flow.json`
  and its round-trip test are unmodified. Confirmed by the full test suite staying green.
- **G7** — `Stop` aborts the whole remaining step array regardless of fan-out (§3) — a fired switch
  branch or a parallel sibling does not get special-cased; refining this is deferred, not silently
  assumed away.

## 10. Open questions

- **`debug-run` sandboxed execution** (§5) — running a compiled flow against one sample payload
  without touching live actor/persistence state. Real design work (does it reuse the actor's
  `CompiledFlow*` read-only? a scratch `ProcessingContext` only?) — not committed for v0.1.
- **Trace endpoint cost** — per-step counters on every `CompiledFlow` step add a small amount of
  state/writes to the hot path; needs to be measured against `flow_zero_alloc`'s CI gate (an atomic
  counter increment is not an allocation, but it is a write — confirm it's cheap enough to always-on,
  or make it opt-in per deployed flow).
- **Per-branch `Stop` scoping** — G7 above is the conservative v1 choice (Stop aborts everything); a
  future increment could scope it to the firing branch only if a real use case needs sibling branches
  to keep running after one aborts. Not needed yet.
- **N-way switch (beyond true/false)** — v1's `aero.flow.switch` is binary only (§7); a case/label
  switch would need to relax the single-active-label restriction and the "no conflicting labels"
  compiler check (§3) to something richer. Revisit only if a real flow needs more than one boolean
  branch point.
- ~~Multiple independent switch points in one flow~~ **Resolved: rejected at deploy (§3 step 6).**
  `ctx.active_branch` is a single field, not a stack — v1 supports exactly one active branch-producing
  node per flow, matching 004's original "linear + single switch" scope; a second one is a compile-time
  error, not a silent misroute. Nested/sibling switches sharing one flow is future work if ever needed.

~~Multi-input merge semantics~~ **Resolved (§3):** execution is one synchronous array walk, so there is
no runtime race to arbitrate — merge is purely topological order, no wait/first-of-N/windowed-join
concept applies.
~~Port type granularity~~ **Moot (§2):** `NodeDescriptor` never gained a ports concept — there is
nothing to type.
~~Fan-out semantics vs `NodeResult::Stop`~~ **Resolved as G7 above.**
