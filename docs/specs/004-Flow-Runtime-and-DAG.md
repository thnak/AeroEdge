# 004 — Flow Runtime and DAG

> Draft v0.1. The core novel engine of AeroEdge: how a Flow is compiled to a DAG once,
> and executed per Command inside a Quark handler. This spec also takes the position on
> the sync-vs-async question deferred from the overview.

## 1. What a Flow is

A **Flow** is one behavior expressed as a directed acyclic graph of **Nodes**. It is:

- **triggered** by a Command, an Event, or a Timer (§5);
- **compiled once** at deploy time into an executable plan (§3, I3);
- **executed** by walking that plan, threading one `ProcessingContext` (003) through the
  nodes (I2);
- **thread-free and lifecycle-free** — it orchestrates node calls and nothing else (I1, I8).

Canonical shape:

```text
RawFrame ─▶ Decode ─▶ Normalize ─▶ Rule ─▶ Output
```

## 2. Two phases: compile, then execute

### 2.1 Compile (deploy time, once)

A Flow is authored as a graph description (nodes + edges). At deploy time the **Flow
Compiler** turns it into a `CompiledFlow`:

1. **Validate** — DAG is acyclic; every node's declared inputs are satisfied by an
   upstream output; types match; exactly one trigger.
2. **Topologically order** — produce a linear execution schedule (the DAG is a schedule,
   not re-resolved per run).
3. **Bind node instances** — each node's concrete `INode*` is resolved from the registry
   (005) and stored in the plan. Node construction/config happens here, never per-Command.
4. **Lay out the plan** — an array of `{INode*, input-slots, output-slots}` steps plus the
   `ProcessingContext` field layout the schedule needs.

The result is an immutable `CompiledFlow` owned by the actor. Compilation may be expensive;
execution must not be.

### 2.2 Execute (per Command, hot)

```cpp
void CompiledFlow::execute(ProcessingContext& ctx) const noexcept {
    for (const Step& step : steps_) {           // pre-topologically-ordered (I3)
        if (ctx.cancel.stop_requested()) break; // cooperative cancel between steps (Quark 018)
        NodeResult r = step.node->process(ctx);  // one virtual call per node (I7)
        if (r == NodeResult::Stop)  break;        // rule node can short-circuit the flow
        if (r == NodeResult::Error) { ctx.fail(step); break; }
    }
}
```

Execution is a straight walk over a pre-built array. There is **no graph resolution, no
allocation, no locking** on this path (I3, I6). The only indirection is one virtual
`INode::process` call per step — which is exactly why I7 confines this layer *off* Quark's
message-dispatch hot path: at flow granularity, one virtual call per node is cheap and
acceptable; it must never leak into Quark's per-message zero-vtable path.

### On branches and non-linear DAGs

A true DAG can branch (a Rule node routes to one of several downstream subgraphs). The
compiled plan represents this as steps that read a "which branch" flag from the context and
skip disabled steps, **not** by re-walking a graph. Topological order + skip flags keeps
execution a linear array walk while still expressing branch/switch nodes. Full branch
semantics (fan-out, merge) are pinned in a follow-up revision; v0.1 targets linear + single
switch, which covers the canonical decode→rule→output pipeline.

## 3. The compile-once invariant (I3), concretely

| Time | What happens | Cost budget |
|---|---|---|
| Deploy | validate + topo-sort + bind nodes + lay out plan | may be ms; happens once |
| Per Command | walk the step array, one `process` per node | must be ns/µs; 0 alloc |

If any per-Command work would resolve the graph, allocate node state, or look up a node by
name, it belongs in the compile phase instead. This mirrors Quark's own "compile once"
posture (metadata compiled at startup, ADR-008).

## 4. Sync vs async — the position this spec takes

**Decision: Flows are sync-first.** A `CompiledFlow::execute` runs to completion inside one
synchronous Quark handler. Rationale:

- **Matches Quark's zero-cost sync handler.** Quark handlers are synchronous by default and
  only opt into coroutines per message type (README "Handler execution: Hybrid"). A
  sync flow adds no coroutine frame, no suspension machinery.
- **Keeps `ProcessingContext` lifetime trivial (I6/003 §4).** No suspend point means the
  context is a plain stack/arena value that cannot escape.
- **Preserves I2.** One Command, one executor, run-to-completion — no interleaving to reason
  about mid-flow.

**How I/O nodes cope without blocking (I1: never block a worker long):**

- **Output nodes (MES/DB/REST/MQTT publish).** A sync Output node does *not* perform the
  network call inline. It stages the payload and the flow emits a Command to a dedicated
  **I/O actor** (an `EgressActor`) via `tell`; that actor owns the blocking call on its own
  lane. The originating flow returns immediately. This is the sample-03 forwarding pattern.
- **Source nodes (socket/serial read).** Ingestion is not pulled inside a flow at all — it
  rides Quark's inbound streaming (024): the driver feeds frames into the actor's
  `StreamChannel`, each frame arriving as a `ReceivePacket` Command that *triggers* the flow.
  The flow never reads a socket. See 006 (Drivers).

**The coroutine evolution path (when a real workload needs in-flow async):** a Flow may be
marked `Async`, compiling to a `quark::task<>` walk where a node returns an awaitable and
the executor `co_await`s it. The context moves into the coroutine frame (003 §4). This is an
*additive* mode — sync flows keep compiling to the zero-cost path. We do **not** build it in
v0.1; the field layouts in 003 are kept coroutine-friendly so it stays a clean addition.

> If you later find most Output nodes want in-flow async and the EgressActor hop is a
> bottleneck, that's the trigger to promote `Async` flows from "planned" to "build".

## 5. Triggers

A Flow binds to exactly one trigger:

| Trigger | Source | Binding |
|---|---|---|
| **Command** | a `tell` of a Command type (002) | `ReceivePacket → DecodeFlow` |
| **Event** | an Event delivered to this actor (002 §4) | `TagChanged → AlarmFlow` |
| **Timer** | Quark timer wheel / reminder (011, 027) | `Every 1s → HeartbeatFlow` |

Timer-triggered flows are the clean case where AeroEdge leans entirely on Quark: the actor
schedules a periodic timer (Quark 011); each tick is a Command that triggers the flow. No
AeroEdge timer code.

## 6. Errors and cancellation

- A node returns `NodeResult::Error` → the flow stops, `ctx.fail(step)` records which step
  failed; the actor decides (retry, emit an `AlarmRaised` event, drop). Node code never
  throws across the flow boundary; if it does, Quark's supervision guard (007, ADR-009)
  contains it at the handler boundary.
- Cancellation is cooperative: the executor checks `ctx.cancel` between steps (Quark 018
  deadlines/`stop_token`). Under overload, Quark 022 may shed the *Command* before the flow
  runs at all.

## 7. Open questions

- **Branch/fan-out/merge semantics** beyond linear + single switch (§2.2) — revision 2.
- **Per-flow scratch typing** (003 §6) — typed struct vs type-erased; decides plan layout.
- **Sub-flows / reuse** — can a flow invoke another compiled flow as a node? Likely yes via
  a `SubFlowNode`; deferred until node registry (005) is settled.
- ~~**Hot-reload**~~ **Resolved in [009 §4](009-Deployment-and-Flow-Versioning.md):** compile the
  new flow off to the side, `quiesce(Drain)` to a consistent point (free on Sequential actors),
  pointer-swap the `CompiledFlow` (Quark ADR-008 Hot-Leaf), retire the old — no in-flight Command
  dropped. Native-extension swaps are BuildOnly (drain + re-activate).
