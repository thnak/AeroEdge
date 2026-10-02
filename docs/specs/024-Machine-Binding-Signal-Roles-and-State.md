# 024 — Machine Binding, Signal Roles and State Derivation

> Draft v0.2. Where a **device** becomes a **machine**. Points (022) are physical addresses.
> This spec binds them to business machines with a **role**, turns them into machine-level
> counts and a machine state, and defines how outputs are addressed. It's the layer that lets
> everything upstream (AeroOEE, AeroMes) ignore tags, pins and protocols.
>
> v0.2 incorporates the red-team and use-case review:
> - counting at the source with sequence numbers;
> - good/total and channel qualifiers;
> - pairing moved upstream;
> - cycle-time statistics;
> - richer level/threshold rules;
> - an `Unknown` state;
> - node-apply-time binding semantics;
> - a persistence policy that doesn't fsync per pulse;
> - point quality, binding templates and a commissioning flag.
>
> Suite context: [AeroSuite 07 §5](https://github.com/Iot-Viet-Solution/AeroSuite/blob/main/docs/07-aeroedge-target-scope.md).

## 1. Machines are not created here

AeroEdge does **not** own machines. It receives a read-only machine list (suite key, name,
type, site, `version`) from the suite's single master-data writer (AeroMes, or AeroOEE
standalone) through the upstream hook's inbound path (025 §5). Master-data events carry the
writer's **service identity** and a **version**. Events from a non-writer service, or with a
stale version, are rejected. AeroEdge binds to machines. It never edits them.

Bindings may also target a **non-machine asset** (line, building, utility) for P2 roles such as
energy (§9). These assets come from the same master-data stream.

## 2. Binding

A **binding** = `point → (machine, role, qualifiers, processing rule)`, plus a validity interval
and flags.

### 2.1 Roles

| Role | Qualifiers | Meaning | Typical source |
|---|---|---|---|
| `count` | `kind = total \| good`, `channel` (default 0) | produced cycles / strokes / units | device counter, PLC counter, interrupt input |
| `reject` | `channel`, `code` (optional) | rejected units | reject sensor, PLC counter |
| `run` | | machine running / cycling | run lamp, motor contactor, CT clamp |
| `fault` | `code_map` | fault active, with code | alarm output, PLC alarm word, error pins |
| `mode` | | auto / manual / setup | selector switch, PLC word |
| `product` | | product / mold / recipe identifier | mold RFID, PLC recipe no. |
| `process` | | process value for trend / SPC | temperature, pressure, current |
| `output:<name>` | | a **writable** point (§6) | tower light, buzzer, interlock relay |

- `kind = total | good` lets upstream keep AeroOEE's existing inference: Total + Scrap gives
  Good = T − S, and Good + Scrap gives Total = G + S. That inference is done **upstream**
  (layer 4). AeroEdge only reports what each channel measures.
- `channel` covers multi-lane and multi-position machines (e.g. a 6-position press, rolls A/B/C).
  Counts are reported per channel.
- **Pair groups:** bindings may carry `pair_group = <name>` to tell upstream which channels form
  a pair (left/right shoe, two-sided part). **The pairing calculation itself is upstream.**
  AeroOEE takes the min over *run-cumulative* channel totals, and the sum of per-window minimums
  is not the min of the sums, so a windowed edge rule would change the numbers.

### 2.2 Time semantics

- **Effective time = node apply time.** A binding change takes effect when the node hosting the
  `MachineActor` applies the new projection (022 §4). A `valid_from` in the past is **refused**.
  History is never rewritten (MB2), and the node can't honor a backdated binding without
  rewriting events it has already sent.
- Applying a binding change **closes the affected machine's open windows** first. The closed
  windows are emitted under the old binding, then the new binding starts.
- **Counter baselines belong to the point** (held by the point's `EdgeActor`), not to the
  binding. Moving a PLC counter to another machine keeps its baseline, so the new machine's
  first window doesn't swallow the whole counter.
- `MachineDeactivated` (025 §5) closes and emits all open windows of that machine, then closes
  its bindings.

### 2.3 Flags, templates, bulk

- **`test` flag** (commissioning): counts and states from a `test` binding are shown in Studio
  and emitted upstream with `test = true`. Consumers exclude them from OEE and production.
  Devices in `Maintenance` (022 §3) deliver as `test` too.
- **Binding templates** per machine type: a named set of role bindings by point *slot* (e.g.
  "MultiCavityMould", "PairedGoodChannel": AeroOEE's 9 presets are ported as templates), applied
  to N machines at once.
- **Bulk import/export** (CSV/JSON) of bindings through `aero-api`.

Bindings live in the registry (022) and are EventSourced (audit: who rebound what, when).

## 3. I/O layers

| Layer | Examples | Owner |
|---|---|---|
| 1. Physical I/O | pin mode, interrupt edge, **debounce**, pull-up, analog range, register address, poll rate | **AeroEdge**: point config (022 §2) |
| 2. Signal processing | counter delta, rollover, scaling, thresholds, hysteresis, blink, truth tables | **AeroEdge**: binding rule (§4) |
| 3. Machine meaning | point → machine + role + qualifiers | **AeroEdge**: binding (§2) |
| 4. Product meaning | parts per cycle (cavities), components per signal, pairing, good = total − scrap, disposition by product | **Upstream**: needs product/mold master data and run context AeroEdge doesn't own |

**Unit rule:** AeroEdge reports counts in **machine units** (cycles, strokes, or scaled counts)
and states the unit in the event (025). Upstream converts to parts. Every factor, including
debounce, is applied **once**, in exactly one layer.

## 4. Counting at the source

A count must be **idempotent across redelivery**. 002 §4 makes Events at-least-once, and a
`TagChanged` redelivered across nodes must not be counted twice. So AeroEdge never counts
"events that arrived". It computes deltas of a **source-held counter** and dedupes by sequence.

| Source | How counting works |
|---|---|
| **Device protocol v4** (028) | device keeps a cumulative counter per input (persisted on the device), sends `{counter, counter_epoch, boot_id, seq, cycle stats}` |
| **PLC counter** (Modbus/OPC UA) | the PLC register is the counter. AeroEdge reads it |
| **Interrupt input on the driver host** (DI card on the edge box) | the **driver** counts edges into a cumulative counter with `seq` at the source. Only allowed on interrupt-capable inputs |

Edge counting on a **polled** digital point is **rejected at validation**, because pulses shorter
than the poll interval are invisible.

### 4.1 Built-in processing rules

| Rule | For | Parameters |
|---|---|---|
| `counter_delta` | all counts | width (16/32/64-bit), rollover; **reset detection**: `counter_epoch` change (028 §4.2), a decrease that isn't a rollover, or a configured daily reset hour → new baseline, **no delta**. A reboot with an unchanged epoch continues the count |
| `scale` | counts, values | factor (may be negative), offset |
| `level` | `run`, `fault`, `mode` | digital active-high/low **or** analog threshold; hysteresis; min hold time |
| `blink` | tower-light sensors | blink period range → distinct state (e.g. blinking yellow = alarm) |
| `truth_table` | multi-input state | inputs → `run`/`fault`/`mode` value (e.g. green/yellow/red lamp sensors) |
| `threshold_count` | analog or score inputs (AI camera) | threshold, re-arm level → counter with `seq` |
| `code_map` | `fault`, `reject` | pin/value → code |
| `deadband` / `downsample` | `process` | deadband, interval |

Deduplication: the `MachineActor` keeps, per point, the last applied `(boot_id, seq)` or counter
value. A reading at or below it is a duplicate and is ignored.

### 4.2 Cycle statistics

When the source provides per-pulse timing (protocol v4 `cycle_ms`, or the driver's interrupt
timestamps), the window carries `cycle_ms {min, avg, max, last, n}`. An **opt-in**
`CycleReported` per cycle can be enabled per binding for run analysis. AeroOEE's
warm-up/degradation analytics need this, and dropping it would be a regression.

## 5. `MachineActor`

One per bound machine (or asset):

```text
EdgeActor(dev A) ──TagChanged(point, value, seq)──┐
EdgeActor(dev B) ──TagChanged(point, value, seq)──┼──▶ MachineActor(M-03)
                                                   │      • dedupe by (point, boot_id, seq)
                                                   │      • apply binding rules (§4)
                                                   │      • accumulate windows
                                                   │      • derive machine state (§5.3)
                                                   └──▶   • stage events in its own outbox (025 §2)
```

### 5.1 Placement and epochs

- **Placement:** affinity to the segment of its bound devices (010 §2.1).
- Each activation increments a persisted **`producer_epoch`** (stamped with the node id). All
  events carry `(producer_epoch, seq)` (025 §3).
- **Cross-node migration is not assumed.** 010 §5 records fenced cross-process migration as not
  yet real. Until it is:
  - **Restart on the same node** restores state from the local store and continues.
  - **Activation on another node** (failover) is a **cold start**:
    1. counter baselines are unknown, so the first reading of each point establishes a baseline
       and produces no delta;
    2. emit `CountGap{machine, from, to}` covering the time since the last window this node can
       prove was delivered, or "unknown";
    3. counts from devices that hold their own counter (v4, PLC) are **recovered on the next
       reading**, because their counter kept counting. Only driver-host interrupt counts in the
       gap are lost, and the gap event says so.
  - **Split brain:** two nodes may briefly both produce for one machine. Their epochs differ, so
    upstream sees overlapping `(machine, epoch)` streams. It deduplicates by event identity and
    flags `ConcurrentProducers` for review. Event keys never collide, so nothing is silently
    dropped.

### 5.2 Windows and persistence

- Windows close every *W* seconds (default 15 s, per machine type) or on: state change, product
  change, binding change, machine deactivation.
- **Persistence policy** (avoids an fsync per pulse on eMMC/SD):

  | State | Persisted |
  |---|---|
  | closed window + outbox staging | **Sync**, in one transaction (025 §2) |
  | counter baselines / last `(boot_id, seq)` | **Sync at window close**. Between closes they're recovered from the source counter |
  | open-window accumulation | **Batched** (default 1 s). Loss bound: driver-host interrupt counts in the last batch. Counter sources lose nothing |
  | `last_seen`, live values | volatile (022 DR7) |

### 5.3 State model

Shared with AeroOEE/AeroMes (AeroSuite 02):

| State | Default derivation |
|---|---|
| `Offline` | every device bound to `run` **and** `count` is Offline (022 §5) |
| `Unknown` | some devices needed for derivation are Offline or their points are stale (§5.4), so the state can't be derived. Never reported as `Idle` or `Running` from stale data |
| `Down` | `fault` active |
| `Setup` | `mode = setup` |
| `Running` | `run` active, **or** counts arriving within the machine's **`signal_timeout`** |
| `Idle` | none of the above, with all needed inputs fresh |

- `signal_timeout` is a per-machine (or machine-type) edge parameter. It is **not** ideal cycle
  time, which is product master data AeroEdge doesn't own.
- `PlannedStop` is **not** an edge state. It needs the schedule and calendar, so upstream
  derives it.
- Rules are configurable per **machine type**, with per-machine overrides. AeroEdge classifies
  *state*. It doesn't classify losses (planned vs unplanned, reason codes).

### 5.4 Point quality

Each binding evaluates the quality of its point:

| Quality | Trigger |
|---|---|
| `stale` | no update for longer than the role's staleness limit while the device is online |
| `flatline` | value unchanged for longer than an expected-activity limit while the machine is `Running` by another signal |
| `error` | device or codec reports an error value (e.g. `ERR`, Modbus exception) |

Changes emit `PointQualityChanged` (025 §3) and feed the `Unknown` state. This catches the
"device online, point stuck at 0 for months" failure, e.g. a counter pointed at the wrong PLC.

### 5.5 Product context

- If the machine has a `product` binding, the device-sourced value is authoritative.
- Otherwise upstream sets it with `SetCurrentProduct(machine, product/mold/recipe)` (025 §5).
- Either way, a product change **closes the open window** so counts split exactly at the
  changeover, and `ProductChanged` is emitted.

## 6. Outputs (writable points)

- Physical andon and tower-light outputs are **new capability**. Neither AeroOEE nor the
  customer systems drive physical outputs today.
- Upstream commands outputs **by machine + output role**: `SetOutput(machine, "andon_light",
  red)`, `Interlock(machine, on)`. It never addresses a device or pin.
- Inbound commands enter through **any node's** upstream gateway and are routed to the
  `MachineActor` by Quark actor routing (027 §2). The control plane isn't on this path.
- The `MachineActor` resolves role → bound point → `tell` the owning `EdgeActor`, whose driver
  writes it (006 write path).
- **Authorization:** only service identities granted `output:<role>` for that machine (023).
- **Fallback on upstream loss:** each output role declares `hold` (default) or `set <value>`.
  "Upstream loss" means the **liveness lease** of the subscription that owns that output has
  expired (025 §2.3).
- **Safety:** safety-critical interlocks belong in the machine's own safety circuit. AeroEdge
  outputs are production-control or advisory. Write-capable drivers in multi-node deployments
  stay gated by 010 §6.
- Every applied change emits `OutputChanged` (cause: command / fallback).

## 7. Invariants (normative)

- **MB1**: AeroEdge never creates or edits machines. It accepts machine master data only from
  the authorized writer, with a newer version.
- **MB2**: bindings take effect at node apply time. Rebinding closes open windows first and
  never rewrites emitted events.
- **MB3**: counts are in machine units with an explicit unit. Product-dependent factors,
  pairing and good/total inference are never applied at the edge.
- **MB4**: machine state is derived in exactly one place (`MachineActor`) and published once.
  All consumers see the same state stream.
- **MB5**: counting is idempotent. It works on source-held counters deduplicated by
  `(point, boot_id, seq)`, never on "events received".
- **MB6**: on a cold start, a missing baseline never produces a delta. The gap is reported as
  `CountGap`.
- **MB7**: state is `Unknown`, never a guessed `Idle`/`Running`, when the inputs needed to
  derive it are offline or stale.
- **MB8**: upstream addresses outputs by machine + role only. The role → point mapping is
  AeroEdge-internal.
- **MB9**: closed windows and their outbox entries are persisted Sync in one transaction. Open
  accumulation is Batched with a stated loss bound.

## 8. Implementation status

Not started. Depends on 022 (points, registry), 025 (event contract, outbox), 028 (device
protocol v4 for counters with `boot_id`/`seq`).

## 9. Open questions

- Default *W* and `signal_timeout` per machine type: ship presets for injection, press, sewing?
- Who edits bindings: Studio only, or also AeroOEE's machine screen through `aero-api`?
- **P2:** `energy` role (cumulative kWh via `counter_delta`, `EnergyReported`) on machine and
  non-machine assets. `call:<kind>` role for andon call buttons (`AndonCallRaised`).
  Measurement/ID-scan roles (stable weight, RFID/barcode with duplicate suppression).
