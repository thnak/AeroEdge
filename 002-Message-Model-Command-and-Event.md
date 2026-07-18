# 002 — Message Model: Command and Event

> Draft v0.1. The two message kinds AeroEdge distinguishes, their guarantees, and how
> each maps to a Quark primitive. This is the contract that makes I5 ("Command mutates,
> Event notifies") enforceable rather than aspirational.

## 1. Two kinds, one transport

AeroEdge has exactly two message kinds. Both ride Quark's messaging (006) — there is no
separate AeroEdge transport.

| | **Command** | **Event** |
|---|---|---|
| Purpose | change actor state | notify that something happened |
| Mutability | may mutate state | immutable |
| Ordering | FIFO, strictly sequential per actor | published post-flow, unordered across subscribers |
| Delivery | `ActorRef<A>::tell` (or `ask` for reply) | `tell` to each subscriber ref |
| Produced by | outside world, drivers, other actors | Flows, after they complete |
| Triggers a Flow? | yes (Command-triggered flows) | yes (Event-triggered flows) — on the *receiving* actor |
| Example | `ReceivePacket`, `PollPLC`, `MESOrderReceived` | `TagChanged`, `AlarmRaised`, `ConnectionLost` |

The distinction is **semantic and enforced by convention + base-class API**, not by two
different Quark channels: an Event is just a Command-shaped message that (a) is only ever
*published* by the flow layer after commit, and (b) whose handlers must not mutate the
state the flow committed. See §4.

## 2. Command

A Command is a plain struct delivered by `tell`. Because Quark guarantees per-actor
mailbox FIFO and single-executor drain (001, ADR-002), Commands for one actor are:

- **ordered** — processed in send order from any single sender;
- **sequential** — never two at once for the same actor (I2);
- **state-mutating** — the handler may write actor fields; the Flow's `commit` step is
  the sanctioned place to apply mutations gathered in the `ProcessingContext` (I5).

Commands that need a reply use `ask<R>` — e.g. `ConnectDevice → ask<ConnectResult>`. The
reply is the only value returned; state changes happen as a side effect of the handler.

### Command → Flow binding

A Command type is bound to at most one Flow per actor. The binding is declared at deploy
time (004 §"Triggers"), producing a compile-time or table-driven dispatch:

```text
ReceivePacket   ──▶  DecodeFlow
PollPLC         ──▶  PollFlow
MESOrderReceived──▶  OrderFlow
```

## 3. Event

An Event is **immutable** and **published only after a Flow completes** (I5). It never
mutates the state of the actor that produced it. Producing an Event during flow execution
means *appending it to `ProcessingContext.events`*; the actor base publishes the drained
event list after `commit`:

```text
handle(Command):
    ctx = build_context()
    flow.execute(ctx)          # nodes may append events to ctx.events
    commit(ctx)                # apply state mutations   (I5)
    for e in ctx.drain_events():
        publish(e)             # tell each subscriber    (post-flow only)
```

This ordering is the whole point: a subscriber can never observe an Event whose
corresponding state change has not yet been committed.

## 4. The Event bus

Events must reach interested actors (e.g. a `LineActor` subscribes to `TagChanged` from
its `EdgeActor`s). AeroEdge's event bus is a **thin routing layer over Quark `tell`** —
not a new broker:

- **Direct subscription (v0.1).** An actor holds `ActorRef`s to its subscribers (as in
  sample 03). `publish(e)` = `tell(e)` to each. Zero new infrastructure; fully typed.
- **Named topics (planned).** A `TopicRegistry` maps an event type/topic to a list of
  subscriber refs, resolved at deploy time. Still compiles down to `tell` per subscriber.
- **Cross-node (deferred).** When actors live on different nodes, delivery is Quark's
  distribution (010) + delivery guarantees (017). AeroEdge adds nothing here.

Delivery semantics for Events are **at-least-once by default** (Quark 017); Event handlers
must therefore be **idempotent**. This is a design constraint on Event-triggered Flows,
stated here so 004 can assume it.

### Event handler = Event-triggered Flow

On the receiving actor, an Event is handled exactly like a Command: it triggers a Flow.
The only added rule is idempotency (above). Mechanically there is no second code path.

## 5. Message identity, tracing, deadlines

Every message carries Quark's ambient `MessageContext` (008/018): `stop_token`, deadline,
trace id, headers. AeroEdge does **not** define its own envelope. A Flow reads the deadline
to decide whether to shed work under overload (Quark 022), and propagates the trace id into
emitted Events so a `TagChanged` can be traced back to the `ReceivePacket` that caused it.

## 6. Serialization

When a Command or Event crosses a node boundary it is serialized by Quark's `describe`
mechanism (016) — one `QUARK_SERIALIZE` per message type. AeroEdge message structs are
ordinary Quark-serializable types; no bespoke wire format. Local (same-node) delivery is
zero-copy and never serializes (I6 applies to `ProcessingContext`, not to inter-actor
messages, which *may* serialize when remote).

## 7. Open questions

- **Topic model:** typed direct refs (v0.1) vs a string-keyed topic registry — pick when a
  real fan-out workload appears. Direct refs are the default until then.
- ~~**Event replay / event-sourcing**~~ **Addressed in [007 §2.1](007-State-and-Persistence.md):**
  actors opt into Quark 012 `EventSourced` only where audit/replay/MES-backfill is required;
  `Snapshot` is the default. Retention/compaction reach is the remaining sub-question there.
- **Backpressure on Event fan-out:** a slow subscriber must not stall the publisher;
  resolved by Quark 022/024 governance, but the AeroEdge policy knob is TBD.
