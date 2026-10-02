# 005 — Node SDK and INode

> Draft v0.1. The extension contract. Everything pluggable in AeroEdge — built-in, native,
> and (later) WASM — implements one interface: `INode`. This spec defines that contract,
> the node categories, the registry, and the rules that keep nodes runtime-blind (I4).

## 1. The contract

```cpp
namespace aero {

enum class NodeResult { Continue, Stop, Error };

class INode {
public:
    virtual ~INode() = default;

    // The ONLY method the flow executor calls per Command. Runs to completion,
    // synchronously, off Quark's mailbox hot path (I7). Must not block long,
    // sleep, spawn a thread, or touch the Actor/Flow/Engine (I1, I4).
    virtual NodeResult process(ProcessingContext& ctx) noexcept = 0;

    // Called ONCE at flow-compile time (004 §2.1), never per Command.
    // Node reads its configuration and pre-allocates here.
    virtual NodeStatus configure(const NodeConfig& cfg) noexcept { return NodeStatus::Ok; }

    // Compile-time descriptor: category, declared input/output slots, type identity.
    // Drives DAG validation (004 §2.1) — types and wiring are checked before deploy.
    virtual const NodeDescriptor& descriptor() const noexcept = 0;
};

} // namespace aero
```

Three methods, three phases:

| Method | When | Frequency | May allocate? |
|---|---|---|---|
| `descriptor()` | compile-time validation | per compile | — (returns static) |
| `configure()` | flow compile | once | yes (pre-allocation) |
| `process()` | flow execute | per Command | **no** (0 alloc, I3) |

A Node knows only `ProcessingContext&`. It never learns which Actor, Flow, or Engine it
belongs to (I4). This is what makes the same node reusable across actors and flows.

## 2. Node categories

The category is metadata (in the descriptor), not a separate interface — all four
implement `INode`. It drives validation and documents intent.

| Category | Role | Reads from context | Writes to context | Examples |
|---|---|---|---|---|
| **Source** | introduce data | `frame` | `tags` | Binary framing, packet split |
| **Transform** | reshape data | `frame`/`tags` | `tags`/`scratch` | Binary Decode, JSON Parse, CRC, Scale, Average, FFT |
| **Rule** | business logic / routing | `tags` | branch flag, `events` | Threshold, Switch, Decision, Script |
| **Output** | stage egress | `tags` | `output`, `events` | MES, MQTT, Database, REST, Alarm |

> **Source vs Driver.** A *Source node* runs inside a flow and shapes an already-arrived
> `frame`. A *Driver* (006) is the thing that produces frames from a device and feeds them
> into the actor's stream. The flow never opens a socket (004 §4); Source nodes decode what
> the driver already delivered.

### Output nodes never block (I1)

An Output node does not perform network/DB I/O in `process()`. It **stages** a payload into
`ctx.output` (and/or appends an Event). The actor forwards staged egress to a dedicated
I/O actor via `tell` after the flow completes (004 §4). `process()` for an Output node is
pure staging — fast, non-blocking, allocation-free.

## 3. Node descriptor and DAG validation

```cpp
struct NodeDescriptor {
    NodeCategory category;
    std::string_view type_id;           // stable identity, e.g. "aero.transform.scale"
    std::span<const SlotSpec> inputs;    // named+typed input slots this node consumes
    std::span<const SlotSpec> outputs;   // named+typed output slots this node produces
    Version version;                     // semantic version for evolution
};
```

The Flow Compiler (004 §2.1) uses descriptors to prove, **before deploy**, that:

- every input slot a node declares is produced by some upstream node's output slot;
- slot types match;
- the graph is acyclic and has exactly one trigger.

Validation is compile-time, not runtime — a mis-wired flow fails at deploy, never in
production.

## 4. Authoring a built-in node

Built-in nodes are ordinary C++ classes. Illustrative Transform:

```cpp
class ScaleNode final : public INode {
public:
    NodeStatus configure(const NodeConfig& cfg) noexcept override {
        factor_ = cfg.get_double("factor", 1.0);   // read config ONCE
        return NodeStatus::Ok;
    }
    NodeResult process(ProcessingContext& ctx) noexcept override {
        for (auto& tag : ctx.tags) tag.value *= factor_;   // no alloc, no block
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDescriptor; }
private:
    double factor_ = 1.0;
};
```

## 5. The node registry

Nodes are resolved by `type_id` at compile time:

- **Built-in nodes** register at static-init (or via an explicit `register_node<T>()` call),
  populating a `type_id → factory` table.
- The Flow Compiler looks up each graph node's `type_id`, constructs the `INode`, calls
  `configure`, and stores the pointer in the plan. **All lookups happen at compile time**
  (I3) — `process()` never consults the registry.
- **Versioning:** the registry keys on `(type_id, version)` so a flow pins the node version
  it was authored against; evolution rules mirror Quark's serialization evolution (016).

## 6. Extension surface — one seam, three implementations

All three implement the *same* `INode`, so the flow executor is oblivious to which is which
(the point of the seam):

| Kind | What it is | Status |
|---|---|---|
| **Built-in** | compiled-in C++ `INode` | v0.1 |
| **Native** | `INode` loaded from a shared library (`.so`/`.dll`) at deploy | planned (008) |
| **WASM** | `INode` adapter that drives a WASM module (wasmtime/wasm3) | future (008) |

The Native and WASM adapters are themselves `INode`s that forward `process()` across the
FFI/WASM boundary. Because the boundary crossing costs more than a virtual call, they are
appropriate for coarse-grained nodes (a whole decode/rule), not per-tag micro-nodes. Full
design in 008 (Extension Model).

## 7. Rules a node must obey (normative)

- **N1** — `process()` is `noexcept`, non-blocking, allocation-free, and sees only
  `ProcessingContext&`.
- **N2** — no threads, no sleep, no locks inside a node.
- **N3** — all configuration and pre-allocation happen in `configure()`, once.
- **N4** — a node must not stash a `ProcessingContext*` beyond the `process()` call (I6).
- **N5** — Output nodes stage egress; they never perform the I/O themselves (I1).
- **N6** — a node's `descriptor()` returns a stable, static descriptor for validation.

## 8. Open questions

- ~~**Stateful nodes across Commands**~~ **Resolved in [007 §6](007-State-and-Persistence.md):**
  transient node state lives in the (per-actor, single-executor) node instance and is lost/rebuilt
  on deactivation or migration; durable accumulation is *promoted to actor state* and persisted via
  Quark 012. Rule: **nodes compute, actors remember.**
- **Script/Rule nodes** (embedded scripting for low-code rules) — a `ScriptNode` is just an
  `INode` wrapping an interpreter; language choice (Lua? a small DSL? WASM?) deferred to 008.
- **Typed slots vs a type-erased tag bag** — the SlotSpec type system's strictness trades
  validation power against authoring friction; pin with 004 §2.2 branch design.
