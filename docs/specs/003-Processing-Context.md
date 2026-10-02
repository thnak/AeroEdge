# 003 — Processing Context

> Draft v0.1. The per-Command mutable struct threaded through a Flow. This is the data
> spine of the whole processing model; its lifetime rule (I6) is what keeps the design
> simple and zero-copy.

## 1. What it is

A `ProcessingContext` is created for **one Command execution**, threaded by reference
through every Node in the Flow's DAG, and destroyed when the Flow completes. It is:

- **mutable** — Nodes read upstream fields and write their outputs into it;
- **not copied** — passed as `ProcessingContext&` everywhere (I6);
- **not serialized** — it never crosses a wire; only Commands/Events do (002 §6);
- **single-executor owned** — because a Flow runs inside one Quark handler for one
  Command (I2), there is no concurrent access, hence no locks.

## 2. Shape (illustrative)

```cpp
namespace aero {

struct ProcessingContext {
    ActorView      actor;      // narrow, read-mostly view of the owning actor (see §3)
    Metadata       metadata;   // trace id, deadline, headers — borrowed from Quark MessageContext
    Frame          frame;      // raw inbound bytes / the triggering payload
    TagCollection  tags;       // decoded/normalized signals — the working set
    ScratchState   scratch;    // per-execution transform scratch (dies with the context)
    OutputBuffer   output;     // what Output nodes stage for emission
    EventBuffer    events;     // Events appended during the flow, published post-commit (002 §3)
    StopToken      cancel;     // Quark cancellation token — nodes check it between steps
};

} // namespace aero
```

Field set is **not final** — it grows as node categories need it — but the *lifetime and
ownership rules* below are normative and stable.

## 3. `ActorView` — the narrow window onto the actor

A Node must not see the whole Actor (I4: node logic is runtime-blind). It sees an
`ActorView`: a small, explicit facade exposing only:

- **read** of committed actor state relevant to processing (e.g. last known tag values,
  device config);
- **staged mutations** — a Node does *not* write actor fields directly; it writes into
  `tags` / `output` / `scratch`, and the actor's `commit(ctx)` step (001 §3, 002 §3)
  applies them to real state after the Flow completes.

This indirection is what makes I5 enforceable: Nodes physically cannot mutate committed
state mid-flow; they can only stage. `commit` is the single write point.

## 4. Lifetime and allocation

- **One context per Command.** Built at handler entry, destroyed at handler exit.
- **Stack or shard arena, never the heap hot path.** The context and its buffers come
  from the actor's shard-owned `pmr` arena (Quark 003) or live inline; a Flow execution
  performs **zero heap allocations on the steady path** (mirrors Quark's 0-alloc invariant).
- **No escape.** A `ProcessingContext&` must never be stored beyond the flow execution,
  captured into a `tell`, or handed to another actor. What crosses actor boundaries is a
  Command/Event *copied out of* the context (`output` / `events`), never the context.

### The one subtlety: async nodes

If a Node suspends on async I/O (coroutine flows — 004 §"Sync vs async"), the context must
outlive the suspend point. v0.1 sidesteps this by being **sync-first**: the context is a
stack/arena value whose lifetime is the synchronous handler, so suspension is not on the
table. When coroutine flows land, the context becomes a coroutine-frame member with the
same "no escape" rule — its lifetime extends to the awaiting task, still single-executor.
This is called out so the field layout above stays coroutine-friendly (trivially movable,
no self-referential pointers).

## 5. What the context is NOT

- Not a message envelope — that's Quark's `MessageContext` (002 §5), which the context
  *borrows* `metadata`/`cancel` from.
- Not shared state — cross-actor shared/read-only cache is a separate concern (007).
- Not persistent — nothing in the context is durable; durability is `commit` + Quark 012.

## 6. Open questions

- ~~Exact `TagCollection` / `Frame` representation (owning vs view over the stream buffer)~~
  **Resolved in [006 §4](006-Drivers-and-Sources.md):** `ctx.frame` *views* the live Quark
  024 slot (inline ≤56 B) or a shard payload-arena buffer (larger), valid only for the flow's
  duration; bytes that must outlive the flow are copied into a Command. Lifetime is the
  synchronous flow — I6 holds, no coupling to stream credit beyond `retire()`.
- Whether `scratch` is typed-per-flow (compile-time) or a type-erased bag. Leaning typed,
  resolved once 004 pins the compiled-DAG representation.
