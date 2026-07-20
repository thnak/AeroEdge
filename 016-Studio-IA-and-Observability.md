# 016 — Studio Information Architecture and Fleet/OTA/MES Observability

> Draft v0.1. Phase 9 shipped a Studio that proves the Flow-Designer-to-`aero-api` path end to end,
> but it is a single page: one hardcoded `Application`, a linear node list, one Deploy/Monitor panel.
> Everything the platform does beyond a single flow — distribution/placement (010), firmware OTA
> (011), the MES outbox (012), and managing more than one deployed `Application` — has **no UI**, and
> two of those three (OTA, MES) have **no REST surface either**: `FleetActor`/`MesGatewayActor`/
> `ClusterView` are real, tested classes that nothing in the running daemon constructs. This spec
> defines the IA that closes that gap and the `aero-api` surface it needs, stating plainly which part
> of each is real production wiring and which is an honest gate (mirroring the NullMqtt/NullGrpc
> pattern from 014 Phase 7).

## 1. Information architecture

Today: one `<div className="app">` with a header, the Flow Designer, and a Deploy/Monitor panel,
hardcoded to a single `Application` (`HELLO` in `App.tsx`). Replacing it with a shell + four routes:

```text
AppShell (sidebar nav + content area)
├── /flows          Application catalog (multi-app list) → Flow Designer + Deploy/Monitor for one
├── /fleet          Nodes, capabilities, device placement          (010, NEW REST)
├── /ota            Firmware rollout status, wave progress          (011, NEW REST)
└── /mes            MES outbox stats, pending/delivered, drain      (012, NEW REST)
```

`/flows` subsumes today's whole page: the Application catalog (013 §5 T2, §6 gap — Studio previously
managed one hardcoded app despite `aero-api` already supporting many) is the entry list; selecting an
app opens the existing Flow Designer + Deploy/Monitor panel scoped to it. `/fleet`, `/ota`, `/mes` are
net-new — this spec is what makes them possible.

This is a **presentation-layer change only**: no `aero-schema` DTO changes, no change to
`Application`/`FlowNode`/`DriverSpec` (013 T3). `aero-studio-sdk`'s component contract (015 §4) is
unaffected; the design-system primitives added here (§4) are the natural start of that SDK.

## 2. New `aero-api` endpoint groups

All three groups are read-mostly REST+JSON, consistent with the existing `/apps`, `/status` shape in
`include/aero/api/rest_api.hpp` — plain `GET` returning an `nlohmann::json` built by a `Runtime`-owned
method, `{"error": msg}` + 4xx on failure, no auth/versioning prefix (matches today's convention; not
introduced here). No SSE is added yet — Fleet/OTA/MES views poll on a timer like `/status` did before
`/metrics/stream` existed; a streaming variant is future work, not blocking this phase.

### 2.1 `GET /fleet` — real, for a single daemon instance

Returns the local node's `NodeCapabilities` plus every device actor `Runtime` knows about and its
placement result (`aero::cluster::place`, 010 §2). Wraps `ClusterView`, seeded at daemon start from
the local node's capability config (already a documented input to `place_weighted`, 010) plus the
device registry `Runtime` already tracks for driver bring-up.

```json
{ "nodes": [{ "id": "...", "capabilities": {"flags": [...], "labels": {...}} }],
  "devices": [{ "id": "...", "node": "...", "eligible": true }] }
```

**Honest scope**: real for what one daemon can see. Real *multi-node* membership (multiple daemons
gossiping capabilities) is unchanged from the existing, already-documented gate on Quark's
productionized socket transport (019/021) — `/fleet` reports whatever `ClusterView` has been seeded
or told about via the existing loopback/cluster test path, not a live multi-process membership
protocol. This spec does not claim to close that gate; it only wires the *policy* layer (`aero-cluster`)
that already exists into somewhere the Studio can read it.

### 2.2 `GET /ota/rollouts`, `POST /ota/rollouts` — real orchestration, driven by registered drivers

`Runtime` owns a `FleetActor` (011 §4) seeded from the same device registry as §2.1, each device
paired with whatever `IOtaDriver`-shaped object is actually registered for it. `GET` returns the
current `RolloutState` and each completed `WaveResult`; `POST` (body: `{image, trust_key}`) starts a
new rollout run.

```json
{ "state": "Running|Paused|Completed|Idle",
  "waves": [{ "name": "canary", "attempted": 3, "succeeded": 3, "success_rate": 1.0, "passed": true }],
  "devices_updated": 3, "devices_rolled_back": 0 }
```

**Honest scope**: the wave/gate/auto-pause orchestration (`FleetActor::run`, 011 O5) is real —
identical logic to `tests/ota_rollout.cpp`, just reachable through `Runtime` instead of a standalone
`FleetActor`. What is gated is what a "device" *is*: until a real firmware-push driver + signed-image
crypto (Quark 020) exist, the devices `FleetActor` drives are `MockOtaDriver`s, the same honest gate
already recorded for OTA in the implementation plan. The UI must not claim this pushes real firmware
to real hardware — it visualizes real rollout *policy* execution against the currently-registered
drivers, mock or real.

### 2.3 `GET /mes/outbox`, `POST /mes/outbox/drain` — real, ungated

`Runtime` owns a `MesGatewayActor<Store>` (012 §3) wired to a `RestMesAdapter` pointed at a
configured MES endpoint URL. This is genuine production wiring — `RestMesAdapter` already talks to a
real HTTP server in `tests/mes_outbox.cpp`; nothing about it is a stub. `GET` returns `OutboxStats`;
`POST /mes/outbox/drain` sends a `DrainOutbox` nudge (useful after an MES outage clears, 012 M3).

```json
{ "staged": 12, "pending": 2, "delivered": 10 }
```

No gate here: this is the one subsystem in this spec that is fully real end to end once wired.

## 3. Layering (unaffected, restated for the new wiring)

`aero-ota`, `aero-mes`, `aero-cluster` are today `INTERFACE` libraries linked **only** by their own
tests — not by `aero-runtime` or `aero-api`. Wiring them in follows the existing one-way dependency
rule (`CONVENTIONS.md`): `aero-sdk → aero-core → {aero-nodes, aero-drivers, aero-mes, aero-ota,
aero-cluster, aero-runtime} → aero-api → aero-cli`. `Runtime` gains three new owned members and three
new status/action methods (`mes_outbox_stats()`, `placement_status()`, `ota_status()`/`start_rollout()`)
mirroring the shape of its existing `status()`/`list()`. `include/aero/api/rest_api.hpp` gains the
routes in §2, each a thin call into one of those methods — httplib stays confined to
`aero-api`/`aero-cli`/`aero-runtime`-daemon/`RestMesAdapter`, unchanged from today's confinement rule.

## 4. Studio design system (start of `aero-studio-sdk`)

`components.tsx` today has three primitives (`Panel`, `Button`, `Field`). This spec adds the minimum
needed for a multi-page shell and status-heavy views, on the same CSS-custom-property token system
already in `styles.css` (`--bg/--panel/--line/--fg/--muted/--accent/--danger`, extended with
`--success`/`--warning` for OTA/MES status pills):

- `AppShell` — sidebar nav + content area, replacing the single `<header>`.
- `NavLink` — active-route-aware sidebar link.
- `Table` — plain data table (device lists, wave results, outbox entries).
- `StatCard` — a labelled metric tile (node count, pending count, success rate).
- `Badge` / `StatusPill` — ok/warn/error coloring, generalizing today's one-off `.badge`/`.live-badge`.
- `Tabs` — for grouping (e.g. Fleet's Nodes vs Devices).

This is intentionally still small (015 U7 — host stays small); it is the seed of the real
`aero-studio-sdk` package (013 §5, 015 §4), not the full thing — splitting it into its own published
package is future work, out of scope here.

## 5. Invariants (normative)

- **S1** — new REST routes are additive only; `/apps`, `/status`, `/metrics/stream` are unchanged.
- **S2** — no `aero-schema` change; `Application`/`FlowNode`/`DriverSpec` wire shape is untouched by
  this spec (the Flow Designer's node-graph-canvas upgrade, tracked separately, is a presentation
  change over the same array — order still encodes the DAG, 004 v0.1).
- **S3** — every new view keeps 013 T2: the Studio talks only to `aero-api`, never directly to a
  device, a node process, or Quark.
- **S4** — a view that surfaces a gated subsystem (OTA's mock-driver scope, Fleet's single-daemon
  membership scope) must say so in the UI, not present it as fully live production behavior (mirrors
  R5 — no overstating completion).
- **S5** — `aero-ota`/`aero-mes`/`aero-cluster` wiring into `Runtime` follows the same one-way
  layering as every other subsystem (R1); no new dependency direction is introduced.

## 6. Open questions

- **Polling vs SSE for Fleet/OTA/MES.** `/status` started as poll-only and grew `/metrics/stream`
  later; the same may happen here once a real usage pattern (e.g. watching a rollout live) justifies
  it. Not needed for the first cut.
- **Device registry source.** §2.1/§2.2 assume `Runtime` already has *some* device registry to seed
  `ClusterView`/`FleetActor` from; the exact source (daemon config file vs a future `/devices`
  management endpoint) is deferred to the Phase 11.2 implementation, not fixed by this spec.
- **`aero-studio-sdk` extraction.** §4's primitives stay inline in `studio/src/components.tsx` for
  now; splitting them into a standalone versioned package is deferred until a second consumer (a real
  Tier-2 plugin UI, 015) needs it.
