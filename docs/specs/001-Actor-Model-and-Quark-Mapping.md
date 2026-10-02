# 001 — Actor Model and Quark Mapping

> Draft v0.1. How AeroEdge domain actors are expressed on QuarkCpp, and the exact
> primitive-by-primitive mapping. This spec exists to prevent the single biggest failure
> mode: re-implementing the actor runtime.

## 1. Principle

A domain actor (`EdgeActor`, `LineActor`, `RobotActor`) **is a `quark::Actor` subclass**.
It adds one thing over a plain Quark actor: it *owns a set of compiled Flows* and maps
incoming Quark messages to Flow executions. Everything else — mailbox, scheduling,
lifecycle, timers, state ownership — is inherited from Quark unchanged.

## 2. The mapping table (normative)

| AeroEdge concept | QuarkCpp primitive | Spec / ADR |
|---|---|---|
| Edge Runtime (isolated context) | An `Activation` of a `quark::Actor` | 001, ADR-008 |
| Actor mailbox | Intrusive Vyukov MPSC mailbox | 003, ADR-002 |
| Command delivery (FIFO, sequential) | `ActorRef<A>::tell(Cmd)` | 006, ADR-007 |
| Request/reply (e.g. read state) | `ActorRef<A>::ask<R>(Query)` | 006, ADR-007 |
| Actor state | Actor member fields (single-executor owned) | 001, 005 |
| Actor lifecycle Created→…→Disposed | Activation lifecycle | 001, ADR-008 |
| Timer / Schedule trigger | Timer wheel / delayed & periodic sends | 011 |
| Durable scheduled wake-up | Reminders on the `Store` seam | 027 |
| Persist actor state | Snapshot / event-sourced `Store` | 012 |
| Isolation, no shared memory | Single-executor invariant | 001, 015 |
| Event publish to other actors | `tell` to subscriber refs / dead-letter | 009, 017 |
| Backpressured device ingestion | Inbound `StreamChannel` credit-ring | 024, ADR-005 |
| Blocking driver I/O | `BlockingHandler` (`quark::blocking<>`) | ADR-015 |
| Placement across nodes (future) | HRW placement + SWIM + relay | 010, 025, 026 |
| Supervision / restart | Quark supervision policies | 007, ADR-009 |

**Nothing in the left column is re-implemented in AeroEdge.** AeroEdge code that appears
to need a mailbox, a scheduler, a thread, or a timer is a bug — use the Quark primitive.

## 3. Anatomy of a domain actor

A domain actor is thin. It:

1. declares a `using protocol = Protocol<...>` enumerating the Commands (and Ask
   envelopes) it accepts — exactly as a plain Quark actor does;
2. holds one or more **compiled Flows** as member fields;
3. in each `handle(const Cmd&)`, builds a `ProcessingContext` and runs the bound Flow;
4. after the Flow completes, publishes any Events it produced.

Sketch (illustrative — final signatures land with 003/004/005):

```cpp
#include "quark/core/actor.hpp"
#include "aeroedge/flow/flow.hpp"           // AeroEdge

using namespace quark;

// Commands (mutate state) and an Ask (read state).
struct ReceivePacket { Frame frame; };
struct GetTags {};

struct EdgeActor : Actor<EdgeActor, Sequential, Priority<0>, DrainBudget<16>> {
    using protocol = Protocol<ReceivePacket, Ask<GetTags, TagCollection>>;

    // A compiled Flow, built once at deploy time — NOT rebuilt per message (I3).
    aero::CompiledFlow decode_flow_;   // Source → Decode → Normalize → Rule → Output

    void handle(const ReceivePacket& cmd) noexcept {
        aero::ProcessingContext ctx{/*actor view*/ *this, cmd.frame};
        decode_flow_.execute(ctx);               // walk the compiled DAG (I2, I7)
        commit(ctx);                             // apply mutations to actor state (I5)
        publish_events(ctx.drain_events());      // emit AFTER flow completes (I5)
    }

    void handle(const Ask<GetTags, TagCollection>& m) noexcept { m.respond(tags_); }

private:
    TagCollection tags_;
    // commit(), publish_events() defined in the AeroEdge actor base (see below).
};
```

## 4. The `aero::EdgeActorBase` helper (planned)

Rather than repeat `commit` / `publish_events` in every actor, AeroEdge provides a CRTP
base that sits *between* the user's actor and `quark::Actor`:

```cpp
template <class Derived, class... Policies>
struct EdgeActorBase : quark::Actor<Derived, Policies...> { /* commit, publish_events, flow registry */ };
```

This base is **still a Quark actor** — it adds no runtime, only the Command→Flow→Event
glue. It never introduces a thread, a lock, or a queue (I1). Exact shape is pinned once
003 (ProcessingContext) and 004 (Flow) are settled, since its two helpers depend on both.

## 5. Actor kinds (initial set)

| Actor | Manages | Typical Commands | Typical Events |
|---|---|---|---|
| `EdgeActor` | one physical edge device | `ReceivePacket`, `PollPLC`, `ConnectDevice` | `TagChanged`, `ConnectionLost` |
| `LineActor` | one production line (aggregates devices) | `MESOrderReceived`, `DeviceUpdate` | `ProductionFinished`, `AlarmRaised` |
| `RobotActor` | one robot/cell | `PollPLC`, `MoveCommand` | `TagChanged`, `AlarmRaised` |

These are *examples*, not a closed set — new actor kinds are new `EdgeActorBase`
subclasses. Actor **topology** (which actor owns which device, how a LineActor addresses
its EdgeActors) is an addressing concern handled by Quark's `ActorRef` identity (006);
AeroEdge just stores the downstream refs, exactly like sample 03's pipeline.

## 6. What this spec deliberately leaves open

- The `EdgeActorBase` final signature (waits on 003 + 004).
- How Flows are *deployed* into an actor (config-driven vs compiled-in) → 009.
- Cross-actor Event routing / subscription model → 002 §"Event bus".
- Multi-node placement of device actors → deferred to Quark 010/025/026 usage, not a v0.1 concern.
