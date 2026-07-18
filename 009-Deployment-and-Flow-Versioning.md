# 009 — Deployment and Flow Versioning

> Draft v0.1. How a flow gets from a designer/author into a running actor, how it is versioned,
> and how it is hot-reloaded on a live actor without dropping in-flight Commands. This spec
> resolves the hot-reload question from 004 §7 (the `CompiledFlow` swap) and completes the RFC
> core. It rides Quark's live-reconfig (ADR-008) and rolling-upgrade (021); it invents no new
> distribution.

## 1. What "deploy" means here

Deployment is the act of turning an **authored flow definition** into a **compiled, running
flow** on the actors of a cluster. It is distinct from firmware OTA (011 §1): OTA updates
*device* firmware; deployment updates the *AeroEdge processing graph*. Both use signed bundles
and staged rollout, but they target different things.

```text
 Designer / author  ──▶  Flow Definition (declarative graph)  ──▶  Deployment Bundle (signed)
                                                                         │  distribute (§5)
                                                                         ▼
                              Flow Compiler (004 §2.1) on each node  ──▶  CompiledFlow
                                                                         │  install (§4)
                                                                         ▼
                                                      live actor runs it (hot-reload §4)
```

## 2. The deployment unit — an Application

The unit of deployment is an **Application**: a versioned, self-contained bundle describing a
coherent slice of edge behavior.

```text
Application v<semver>:
  flows:      [ {trigger, graph of node type_ids + wiring + per-node config} ... ]   # 004/005
  drivers:    [ {type_id, device binding, config} ... ]                              # 006
  bindings:   actor-kind → flows/drivers; placement affinity tags                    # 001/010
  extensions: pinned (name, version) of any Native/WASM bundles it uses              # 008
  mes:        adapter selection + canonical-DTO mappings (optional)                  # 012
  signature
```

- **Declarative, designer-friendly.** The flow graph is data (JSON/graph description), the
  output of the future Visual Flow Designer (§8) or hand-authored. Node *logic* is compiled C++
  /WASM (005/008); the flow *topology* is data. This is the low-code/pro-code split made
  concrete.
- **Self-contained + pinned.** An Application pins the node and extension versions it was
  authored against (005 §5, 008 §5) so a deploy is reproducible and cannot silently pick up a
  different node build.
- **Signed.** Verified against a Quark 020 trust root before install, same posture as extensions
  (008 §5) and firmware (011 §3).

## 3. Compile at deploy, never at runtime (restating I3)

On each target node, the Flow Compiler (004 §2.1) validates and compiles the Application's flows
into immutable `CompiledFlow`s **at deploy time**: DAG acyclicity, slot type matching, node
resolution from the registry (005 §5), extension load (008). A definition that fails validation
is rejected at deploy — it never reaches a running actor. Runtime execution only ever walks an
already-compiled plan.

## 4. Hot-reload — resolving 004 §7

Swapping a flow on a **live** actor without dropping in-flight Commands is the load-bearing
capability. AeroEdge gets it from Quark's mechanisms, adding only the compile+swap glue:

1. **Compile the new flow off to the side.** The new `CompiledFlow` is built (§3) while the old
   one keeps running. Compilation touches no live state.
2. **Reach a quiescent point.** The actor drains to a consistent point via Quark
   `quiesce(Drain)` (015) — admission is sealed, in-flight handlers complete. On a `Sequential`
   actor this is *between messages* and resolves synchronously (same primitive that makes
   snapshots free, 007 §2.3). **No in-flight Command is dropped** — they finish on the old flow.
3. **Publish the swap.** The actor's flow pointer is replaced with the new `CompiledFlow`. This
   is a single publish, exactly Quark's **Hot-Leaf live reconfig** (ADR-008): the live read-set
   is a swap-published pointer, not a rebuild. Subsequent Commands run the new flow.
4. **Retire the old.** The old `CompiledFlow` (and any extension instances only it referenced)
   is destroyed once no execution references it.

### Live vs BuildOnly changes (Quark ADR-008 classes)

Not every change can be a hot pointer-swap. AeroEdge classifies deploy changes exactly as Quark
classifies reconfig:

| Change | Class | How |
|---|---|---|
| Node config tweak (threshold, scale factor), swap a flow graph | **Live** | quiesce + pointer-swap (§4) |
| Add a new flow / new node type | **Live** (guarded) | Quark guarded `add_actor_type` incremental validation + table swap |
| Change actor placement affinity, add an actor kind, change persistence model/mode | **BuildOnly** | requires re-activation / rolling deploy (§5); fail-fast if attempted live |

A change's class is determined at deploy and enforced — a BuildOnly change attempted as a live
swap is rejected, not silently half-applied.

### The hard case: unloading a Native extension

Hot-swapping a flow that *drops* a Native `.so` is the one case that cannot be a pure
pointer-swap: in-flight calls may still be inside the old library. Rule: an Application upgrade
that changes a **Native** extension version is **BuildOnly** (drain + re-activate the actor, so
no call is inside the old `.so` at unload). WASM modules, being sandboxed and reference-counted,
can be Live-swapped. This resolves 008 §8's hot-swap sub-question.

## 5. Cluster-wide deployment and rollout

An Application deploys to many nodes. AeroEdge leans entirely on Quark 021 rolling-upgrade +
distribution (010):

- **Distribution.** The signed bundle is disseminated to nodes (over Quark transport / an artifact
  store); each node compiles locally (§3). No central compile.
- **Staged / canary rollout.** Like firmware (011 §4), an Application rolls out in waves: canary a
  few actors/nodes, watch health (Quark 009 metrics — error rate, deadline misses), advance only
  if the wave is healthy, **auto-pause** on regression. Rate-limited by Quark 022.
- **Rolling, drain-safe.** BuildOnly changes ride Quark 021 fenced drain hand-off, so device
  actors migrate/re-activate without dual-driving a device (010 §3). Live changes hot-reload in
  place per §4.

## 6. Versioning, rollback, and state compatibility

- **Application semver.** Each deploy is a versioned Application; the running version per node/
  actor is observable (Quark 009). Rollback = deploy the prior signed Application version.
- **Node/extension version pinning** (005 §5, 008 §5) makes a version reproducible.
- **State compatibility.** A new flow version must remain compatible with **persisted actor
  state** (007). Actor-state schema evolution is Quark 016 (tagged records, migrate-on-read), so a
  flow upgrade that changes what actor state means must ship a 016 migration — checked at deploy,
  not discovered at recovery. A rollback must not strand state written by the newer version
  (forward-compat window is an Application concern, flagged in §9).

## 7. Configuration source

Application definitions and per-node config load through Quark 013 (programmatic `EngineConfig`
+ the TOML/JSON loader seam) and env vars. AeroEdge adds the Application schema on top; it does
not invent a config system. Secrets (MES creds, signing roots) come from Quark 020, never inline
in a definition (012 §M5).

## 8. Futures this spec sets up (original architecture §16)

The declarative Application + hot-reload + versioning is the foundation for the roadmap items,
all of which become *tooling over this spec*, not new runtime:

- **Visual Flow Designer** — emits the Application flow graph (§2).
- **Remote Deployment** — push a signed Application to a remote cluster (§5).
- **Flow Versioning** — §6 (this spec).
- **Hot Reload** — §4 (this spec).
- **Debugger / Replay / Time-Travel** — build on `EventSourced` actors (007 §2.1) + trace ids
  (002 §5); a debugger replays a recorded Command/Event stream through a compiled flow.

## 9. Invariants (normative)

- **P1** — a flow is compiled and validated at deploy; an invalid definition never reaches a
  running actor (I3).
- **P2** — hot-reload drains to a quiescent point before swapping; no in-flight Command is dropped
  (§4).
- **P3** — deploy changes are classified Live vs BuildOnly and enforced; a BuildOnly change is
  never half-applied as a live swap.
- **P4** — Applications are signed and verified before install (§2), and pin their node/extension
  versions (§6).
- **P5** — a flow upgrade that changes actor-state meaning ships a Quark 016 migration, checked at
  deploy; recovery never discovers an incompatible schema.
- **P6** — cluster rollout is staged, health-gated, and drain-safe; a bad Application cannot take
  down a fleet before the canary gate catches it (mirrors 011 O5).
- **P7** — deployment, rollout, and reconfig mechanisms are Quark 013/021/022/ADR-008; AeroEdge
  adds the Application schema and the compile+swap glue, nothing more.

## 10. Open questions

- **Application vs per-actor granularity** — can two actors on one node run different Application
  versions during a rollout, or is the node the atomic unit? Affects canary design; leaning
  per-actor-kind, confirm with Quark 021 usage.
- **Definition schema + designer contract** — the exact JSON/graph schema the Visual Designer
  emits; co-design with the first designer prototype rather than freezing now.
- **Rollback state window** — how long newer-version state stays forward-compatible for rollback
  (007 retention); bounded, policy TBD.
- **Atomic multi-actor deploy** — an Application spanning `LineActor` + its `EdgeActor`s may need a
  coordinated cutover (all-or-nothing); may need a two-phase deploy over Quark 021. Defer until a
  real cross-actor-versioning requirement appears.
