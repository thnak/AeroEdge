# 007 — State and Persistence

> Draft v0.1. The three tiers of state in AeroEdge, where each lives, and how durability is
> delegated to QuarkCpp 012. This spec resolves the stateful-node question left open in
> 005 §8 and the event-replay question raised in 002 §7.

## 1. Three tiers — one rule each

The original architecture sketch names three kinds of state; AeroEdge maps each to a
concrete owner and a single governing rule:

| Tier | What | Owner | Durable? | Rule |
|---|---|---|---|---|
| **Actor state** | committed device/line facts (last setpoint, counters, OTA phase, connection state) | actor member fields | **yes** — Quark 012 | the *only* tier that survives restart/migration; the single write point is `commit` |
| **Flow context** | per-execution working set (`tags`, `scratch`, `output`, `events`) | `ProcessingContext` (003) | **no** | dies with the flow; never persisted, never shared |
| **Shared cache** | read-mostly reference data (device config, calibration tables, tag maps) | Quark `shard_memory` | rebuildable | read-mostly; not the source of truth for mutable state |

The tiers are not interchangeable. Putting mutable device state in the shared cache, or
persisting the flow context, is a design error the invariants below forbid.

## 2. Actor state → Quark 012 (normative mapping)

Actor state is ordinary actor member fields, made durable with Quark's `Persistent` policy
in the CRTP base — AeroEdge writes **no** persistence machinery:

```cpp
struct EdgeActor
  : EdgeActorBase<EdgeActor, Sequential,
                  Persistent<Snapshot, PersistMode::Sync>> {   // Quark 012 policy
    // ... member fields ARE the durable state; commit(ctx) mutates them, Quark persists them.
};
```

### 2.1 Model: `Snapshot` vs `EventSourced`

| Model | Stores | Use in AeroEdge |
|---|---|---|
| **`Snapshot`** (default) | latest serialized state | the default for device/line actors — small, bounded state (setpoints, counters, connection/OTA phase) |
| **`EventSourced`** | append-only log of state-changing events + periodic snapshot | actors that need **audit / replay / time-travel** (002 §7, §16 future) — e.g. a `LineActor` whose production history must be reconstructable |

Recommendation: **Snapshot by default**, opt into `EventSourced` only where an audit trail or
replay is a stated requirement. EventSourced is the substrate for the Replay/Time-Travel
future directions and for MES backfill (012 §7) — but it costs log growth + compaction, so
it is a deliberate choice, not the default.

### 2.2 Mode: `Sync` vs `Batched` — an industrial-safety decision

| Mode | Guarantee | AeroEdge use |
|---|---|---|
| **`Sync`** | mutation is durable before the message completes / before an `ask` reply | **required** for safety-critical state: actuator setpoints, OTA phase (011 O4), anything whose loss could mis-drive a device |
| **`Batched`** | async, coalesced; acked before durable (bounded loss window) | acceptable for high-rate, reconstructable counters where a small loss window is tolerable |

**Normative:** actuator/OTA/setpoint state uses `Sync`. Telemetry-derived counters may use
`Batched`. The choice is per actor type and is a safety review point, not a performance tweak.

### 2.3 Consistent-point snapshots come for free on Sequential actors

Quark takes a snapshot at a quiescent point (015 `quiesce(Drain)`). A `Sequential` actor is
**always** quiescent between messages, so the snapshot guard resolves synchronously and adds
no cost (Quark `snapshot_sequential`). Because AeroEdge device/line actors are `Sequential`
(002: Commands are FIFO-sequential), we get torn-state-free snapshots with no extra work.

## 3. What to persist — and what NOT to

The biggest edge-platform footgun is persisting the telemetry firehose. AeroEdge's rule:

- **Persist decisions and durable facts:** current setpoint, alarm state, production counters,
  connection state, OTA phase, last-known-good config version.
- **Do NOT persist the raw telemetry stream.** A `TagChanged` at 1 kHz is *not* actor state —
  it is an Event (002) routed to consumers (MES 012, a historian, a time-series DB via an
  Output node). Persisting every frame would swamp the `Store` and defeat Quark's 0-alloc
  ingestion (024).
- **Historization is an Output concern, not actor state.** Long-term signal history goes to a
  historian/TSDB through an Output node + Egress actor (004 §4), not into the actor's snapshot.

## 4. Flow context is never durable (restating 003)

`ProcessingContext` and everything in it (`tags`, `scratch`, `output`, `events`) is
per-execution and dies with the flow (I6). Durability happens only when `commit` promotes a
value into actor state, or when an Output node stages egress. There is no path that persists
the context itself. This keeps the hot path allocation-free and the durability surface small.

## 5. Shared cache → Quark `shard_memory`

Read-mostly reference data shared across an actor's executions (or across co-located actors)
lives in Quark's shard-owned memory (`shard_memory.hpp`):

- **Read-mostly, not mutable truth.** Device config, calibration curves, tag→address maps,
  unit-scaling tables. Loaded/refreshed cold (on activation or config change, Quark 013), read
  hot by nodes.
- **Not the source of truth for mutable state.** If a value changes as a *result* of
  processing, it is actor state (tier 1), not cache. Cache is for inputs that change rarely and
  externally.
- **Concurrency.** Shared cache is read-mostly; updates happen at cold points (config reload),
  never mid-flow. This respects the single-executor model — a flow never races a cache write.

## 6. Stateful nodes — resolving 005 §8

A node may need state across Commands (a moving `Average`, a debounce window, a rate counter).
Where does it live?

**Resolution (normative):**

- **Transient node state lives in the node instance.** Because a `CompiledFlow` is owned by an
  actor (004 §2.1), each node instance is **per-actor** and single-executor-owned — so a node
  member field (e.g. a ring of recent samples) is race-free and fast. This is the default for
  windowed/aggregating nodes.
- **Transient node state is NOT durable and does NOT survive migration.** On deactivation or a
  fenced migration to another node (010 §3), node-instance state is **lost and rebuilt** — the
  node starts cold. This is acceptable for statistics that self-heal (a moving average refills
  in N samples).
- **State that must survive restart/migration is promoted to actor state.** If a node's
  accumulated value is a durable fact (e.g. a shift production count, an energy total), the node
  does **not** hold it — it reads/stages it through `ActorView` (003 §3), and `commit` persists
  it via Quark 012. The node computes; the actor owns and persists.

Rule of thumb: **nodes compute, actors remember.** A node holds only state it is willing to
lose on deactivation. Anything else is promoted to tier 1.

## 7. Recovery, fencing, and migration

- **Recovery on activation.** When an actor activates (cold start or post-migration), Quark 012
  loads the last snapshot (or replays the event log), decoding through the 016 migration chain.
  The driver then re-`open`s the device (006 §8) and flows resume. AeroEdge adds only the
  driver re-dial; state recovery is Quark's.
- **Fencing / split-brain.** Two layers guard against a double-writer: the actor is fenced at
  the cluster level (010 §3 / Quark 021 fenced hand-off), and the `Store` itself carries a
  **fencing token** (Quark 012) that rejects a stale writer's commits. For industrial state this
  belt-and-suspenders is intentional.
- **Migration consequence, stated plainly:** tier-1 (actor) state survives; tier-2 (flow
  context) is per-execution so there is nothing to move; tier-3 (cache) is rebuilt; node-instance
  transient state is rebuilt (§6). A stateful node needing continuity across migration must be
  tier-1.

## 8. Encryption at rest and sensitivity

State that is sensitive (credentials, recipes, keys) uses Quark's at-rest encryption
(`at_rest.hpp`, 020) behind the same `Store` seam. AeroEdge does not roll its own crypto or
keystore; it flags which state fields are sensitive and lets Quark 020 handle encryption + key
management. Recipes/setpoints that are IP, and any embedded MES/device credentials, are
candidates.

## 9. Invariants (normative)

- **S1** — actor member fields are the only durable, migration-surviving state; `commit` is the
  single write point (I5).
- **S2** — the flow context is never persisted and never shared (I6).
- **S3** — safety-critical state (actuator setpoint, OTA phase) persists `Sync`; loss is not an
  option for it.
- **S4** — raw telemetry is Events/egress, not persisted actor state (§3).
- **S5** — shared cache is read-mostly reference data, never the source of truth for mutable
  state.
- **S6** — a stateful node holds only state it can lose on deactivation; durable accumulation is
  promoted to actor state (§6).
- **S7** — persistence, fencing, snapshotting, and at-rest encryption are Quark 012/020; AeroEdge
  writes none of them.

## 10. Open questions

- **Snapshot vs EventSourced per actor kind** — which actors genuinely need audit/replay (drives
  MES backfill 012 §7 and the Replay/Time-Travel futures). Decide per domain, not globally.
- **Store backend per deployment** — `FileStore` (std-only WAL) for a single edge node vs
  `SqliteStore`/`RocksStore` for richer query/retention; a deployment choice (Quark
  PersistenceAdapters.md), tie to 009.
- **Retention & compaction policy** for EventSourced actors and the MES outbox (012) — bounded
  vs time-windowed; affects backfill/replay reach.
- **Cache coherence across nodes** — when device config changes centrally, how co-located shard
  caches on multiple nodes are invalidated; likely a config-push Command (013) rather than shared
  mutable memory. Confirm with 009.
