# 008 — Extension Model: Native and WASM

> Draft v0.1. How third parties (and pro-code teams) extend AeroEdge without recompiling the
> platform. Every extension — node or driver — implements the same interface it would as a
> built-in; this spec defines the two out-of-tree loading mechanisms behind that seam and
> resolves the Script/Rule-language question from 005 §8.

## 1. One seam, three implementations (restating 005 §6 / 006 §9)

`INode` (005) and `IDriver` (006) are the two extension points. Each has three
implementations, and the flow executor / stream layer is oblivious to which is which — that
obliviousness *is* the seam:

| Kind | Address space | Trust | Crash blast radius | Use |
|---|---|---|---|---|
| **Built-in** | in-process, compiled in | full | process | platform-shipped nodes/drivers |
| **Native** | in-process, `.so`/`.dll` | **full** | **process** | trusted vendor protocols, high-perf compute |
| **WASM** | sandboxed VM | **none needed** | contained to the VM | untrusted/marketplace nodes, low-code rules |

The trust/blast-radius column is the whole decision: **Native is fast but shares your address
space** (a bad native node crashes the actor process); **WASM is sandboxed but pays a boundary
cost** (safe to run untrusted code). Pick per extension.

## 2. Native extensions

A native extension is a shared library exposing a small **C ABI** factory (C ABI, not C++, so
it survives compiler/STL-version differences):

```c
// The stable C boundary an AeroEdge native extension exports. C++ types never cross it.
extern "C" {
    // Called once at load. Returns the ABI version the extension was built against.
    uint32_t aero_ext_abi_version(void);

    // Enumerate what this library provides (type_ids + categories), copied into host memory.
    const AeroExtManifest* aero_ext_manifest(void);

    // Construct a node/driver by type_id. Returns an opaque handle + a vtable of C function
    // pointers (process/configure/descriptor for a node). The host wraps it in a C++ adapter.
    AeroExtHandle aero_ext_create(const char* type_id);
    void          aero_ext_destroy(AeroExtHandle);
}
```

- **`NativeNode` / `NativeDriver` adapters.** The host wraps the C handle in a C++ `INode`/
  `IDriver` (005/006) that forwards `process`/`run` across the C function pointers. From the
  flow's view it is an ordinary node.
- **ABI versioning.** The host checks `aero_ext_abi_version` against its own; a mismatch fails
  the load at deploy, never at runtime. The C ABI is the compatibility contract — the extension
  and host may be built by different compilers/STL versions.
- **No exceptions across the boundary.** Errors are C return codes/`NodeResult`; a native
  extension must not throw across the C ABI (Quark's supervision guard, 007/ADR-009, only
  contains throws at the *host* handler boundary, not inside foreign code).
- **Trust.** A native extension runs with full process privileges. It is for **trusted** code
  (vetted vendor drivers, in-house compute). Loading one is a deployment-time trust decision;
  signatures (§5) make that decision auditable but do not sandbox.

## 3. WASM extensions

A WASM extension is a `.wasm` module driven by an embedded runtime (wasmtime or wasm3 behind a
`IWasmRuntime` seam — the choice is a build/deployment decision, not baked in):

- **`WasmNode` adapter.** A host `INode` whose `process()` (a) copies the needed slice of
  `ProcessingContext` into the guest's linear memory, (b) calls the guest's exported `process`,
  (c) copies results back. Pointers never cross — the guest sees only its own linear memory
  (this is what makes it safe, and what makes it coarse-grained, §4).
- **Sandbox = memory + capability isolation.** The guest cannot touch host memory, open a
  socket, or read a file except through **host functions** the host explicitly exports. Those
  host functions are **capability-gated by Quark 020**: a WASM node gets only the capabilities
  its manifest requests and the deployment grants (e.g. "read tags", never "open socket").
- **Determinism / resource bounds.** Guest execution is fuel/step-bounded so a runaway WASM node
  cannot stall the actor (respecting I1); exceeding the bound fails the node (`NodeResult::Error`),
  it never hangs the worker.
- **No host threads, no host lifecycle.** A WASM node obeys the same node rules (N1–N6) — it
  computes over a context view and returns. It cannot spawn, block, or persist on its own.

## 4. Granularity — coarse, not micro (normative)

Both Native and WASM cross a boundary costlier than a virtual call. Therefore:

- **G1** — an out-of-tree node should do **meaningful work per `process` call** (a whole decode,
  a whole rule evaluation, an FFT), not a per-tag arithmetic op. Per-tag micro-ops stay built-in.
- **G2** — the flow compiler may *warn* when an out-of-tree node's declared work is trivially
  small relative to boundary cost. This keeps the extension seam from being mis-used as a hot
  inner loop.

This is the same reasoning as I7 (keep the node layer off Quark's mailbox hot path), extended:
keep the *boundary-crossing* node layer off the per-tag inner loop.

## 5. Packaging, manifest, signing, marketplace

An extension ships as a **bundle**: the artifact (`.so`/`.dll`/`.wasm`) + a manifest.

```text
manifest:
  name, version, abi_version
  provides: [ {type_id, category, inputs/outputs, version} ... ]   # → node/driver descriptors
  requires_capabilities: [ read_tags, mes_report, ... ]            # WASM: gated by Quark 020
  signature                                                        # signed by publisher
```

- **Signed and verified.** Bundles are signed; the host verifies against a trust root (Quark
  020) before load — the same signing posture as firmware images (011 §3). Signing makes the
  *source* auditable; only WASM adds *runtime* sandboxing on top.
- **A bundle also carries config + UI.** Beyond the runtime artifact, a bundle ships the plugin's
  **config schema** and an optional **Studio UI contribution** (schema-driven form + custom
  micro-frontend), versioned and signed as one unit — see [015](015-Configuration-Model-and-Studio-Plugin-UI.md).
  Runtime, schema, and UI move together so a UI can never offer what its runtime can't do.
- **Marketplace (future, original spec §16).** The manifest + signature + capability declaration
  is exactly what a marketplace needs: publish signed bundles, install by pinning
  `(name, version)`, run untrusted ones as WASM with least-privilege capabilities. No new
  runtime concept — it is packaging + trust policy over this seam.

## 6. Script / Rule extensions — resolving 005 §8

The low-code Rule/Script need (a `ScriptNode` for user-authored decision logic) is **not a new
mechanism** — it is a node kind on this seam:

| Need | Implementation | Why |
|---|---|---|
| **Pro-code custom logic** (a team writes a real algorithm) | **WASM node** (compile C/C++/Rust/AssemblyScript → `.wasm`) | sandboxed, language-flexible, marketplace-shippable |
| **Low-code rules** (thresholds, switches, simple expressions authored in a visual designer) | a built-in **`ExprRuleNode`** interpreting a small, safe expression DSL | no VM needed for a threshold; evaluates fast, can't escape |
| **Embedded scripting** (Lua-style, if a site demands it) | a built-in `ScriptNode` wrapping a small interpreter, treated like any node | last resort; prefer WASM for sandbox + DSL for simple rules |

**Recommendation:** low-code rules → **`ExprRuleNode` + DSL** (no sandbox VM for a threshold);
pro-code/untrusted logic → **WASM**. A general embedded interpreter (Lua) is available as a
node but not the default — WASM already covers "run user code safely."

## 7. Invariants (normative)

- **E1** — every extension implements the *same* `INode`/`IDriver` as a built-in; the flow/stream
  layer cannot tell the difference.
- **E2** — Native extensions cross a **C ABI**; no C++ types and no exceptions cross the boundary.
- **E3** — WASM extensions are memory-sandboxed and capability-gated (Quark 020); the guest reaches
  the host only through explicitly exported, granted host functions.
- **E4** — WASM execution is resource-bounded (fuel/steps); a runaway extension fails, never hangs
  a worker (I1).
- **E5** — extensions obey node rules N1–N6 / driver rules D1–D6; they never own threads or
  lifecycle (I8).
- **E6** — bundles are signed and verified against a Quark 020 trust root before load; WASM adds
  runtime sandboxing, Native does not.
- **E7** — out-of-tree nodes are coarse-grained (§4); per-tag micro-ops stay built-in.

## 8. Open questions

- **WASM runtime choice** — wasmtime (fuller, heavier) vs wasm3 (tiny, interpreter) vs a WAMR
  build; an edge-footprint benchmark decides, behind the `IWasmRuntime` seam. Likely per-deployment.
- **Expression DSL surface** — the exact grammar/functions of `ExprRuleNode` (comparison, boolean,
  arithmetic, time windows?); grow from real rule requirements, keep it non-Turing-complete so it
  can't loop.
- **WASM ↔ context marshalling cost** — how much of `ProcessingContext` a WASM node needs copied;
  a large `tags` set could dominate the boundary cost. May need a selective/columnar view. Benchmark
  with the first real WASM node.
- ~~**Hot-swapping an extension**~~ **Resolved in [009 §4](009-Deployment-and-Flow-Versioning.md):**
  WASM modules (sandboxed, ref-counted) Live-swap; Native `.so` version changes are **BuildOnly**
  (drain + re-activate so no call is inside the old library at unload).
