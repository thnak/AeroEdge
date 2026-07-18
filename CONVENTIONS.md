# AeroEdge — Conventions

Rules every AeroEdge change is written against. Mirrors QuarkCpp's posture; where they differ,
QuarkCpp owns the runtime and AeroEdge stays a thin domain layer over it.

## Language & build

- **C++23**, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`). Two-compiler gate: **GCC + Clang**.
- **Header-first.** Templates/hot paths live in `include/aero/**`; `.cpp` only for non-template units.
- **Verified on Linux/x86-64** for now (same support phase as Quark). Other targets wait on Quark's PAL.
- Build: `cmake -S . -B build && cmake --build build && ctest --test-dir build`.
- QuarkCpp is an external dependency at `../QuarkCpp` (override with `-DQUARK_DIR=...`), consumed via
  `add_subdirectory` and linked as `quark::quark`. **Never fork or vendor it.**

## The thin-over-Quark rule (load-bearing)

- AeroEdge implements the Flow Runtime, Node/Driver SDK, ProcessingContext, and domain policies —
  **nothing else**. Mailbox, scheduler, timer, lifecycle, persistence, streaming, transport, cluster
  are Quark's. Code that appears to need one of those is a bug: use the Quark primitive.
- Every seam has a self-contained default; heavier backends are optional adapters behind it.

## Layering (dependencies flow one way — 013 T5)

```
aero-sdk → aero-core → {aero-nodes, aero-drivers, aero-mes, aero-runtime} → aero-api → aero-cli
```
- `aero-sdk` is the **stable extension contract** (INode/IDriver/context). It changes slowly and is
  versioned independently; `aero-core` may churn behind it.
- No upward or cyclic dependency. The Studio talks to the runtime only through `aero-api`.

## Node / flow invariants (specs 003–005)

- **N1** — `INode::process` is `noexcept`, non-blocking, and **allocation-free on the steady path**.
- Config/pre-allocation happen once (ctor/`configure`), never per Command.
- `ProcessingContext` is passed by reference, reused across Commands (clear-not-free), and never
  escapes a flow execution (I6). Output nodes **stage**; they never do I/O inline (I1).
- Flows are **sync-first** (004 §4); blocking I/O hands off to a dedicated actor via `tell`.

## Verification (every change)

- **Two compilers** (GCC + Clang), **three sanitizers** (ASan+UBSan, TSan) — all green.
  - `cmake -S . -B build-asan -DAEROEDGE_SANITIZER=address,undefined && ctest --test-dir build-asan`
  - `cmake -S . -B build-tsan -DAEROEDGE_SANITIZER=thread && ctest --test-dir build-tsan`
- **Invariant gates are pass/fail tests**, not noise-sensitive benchmarks — e.g. `flow_zero_alloc`
  asserts 0 heap allocations across the execute path. Add one for each hot-path invariant.
- Tests are deterministic and exit-code-gated (0 = pass), mirroring Quark samples.

## Style

- 4-space indent, `snake_case` for functions/vars, `PascalCase` for types, `kName` for constants.
- Comment the *why* and the spec reference (`(004 §4)`, `(I3)`), not the *what*.
- Match the surrounding file's density and idiom.
