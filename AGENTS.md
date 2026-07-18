# AGENTS.md — Rules for AI agents working on AeroEdge

Every agent (orchestrator or subagent) working in this repo MUST follow these rules. They exist so
parallel work cannot drift from the architecture. Read this and [CONVENTIONS.md](CONVENTIONS.md)
before touching code. The specs (`001`–`015`, `ArchitectureSpecification.md`) and
[IMPLEMENTATION-PLAN.md](IMPLEMENTATION-PLAN.md) are the source of truth.

## R0 — The prime directive: thin over Quark

AeroEdge builds **on** QuarkCpp (`../QuarkCpp`), never reimplements it. You may NOT write a mailbox,
scheduler, timer, thread pool, lock-based queue, persistence engine, stream ring, transport, or
membership/cluster protocol. Those are Quark's — use the Quark seam (see the mapping table in `001`).
If a task seems to need one, STOP and surface it: the requirement pushes *into Quark as a policy*, not
into an AeroEdge fork. Never fork, vendor, or edit QuarkCpp.

## R1 — Stay in your lane

- Work only within the module/phase you were assigned. Do not modify another module's **public
  headers** (`include/aero/**`) or the shared `aero-sdk` / `aero-schema` contract without it being
  the explicit task — those are frozen contracts (013 T1/T3). If you need a change there, report it
  as a blocking finding; don't make it unilaterally.
- Dependencies flow one way (`aero-sdk → aero-core → nodes/drivers/... → aero-api → cli`). Never
  introduce an upward or cyclic dependency.

## R2 — Obey the invariants

Node/flow/context invariants are normative, not suggestions:
- `INode::process` is `noexcept`, non-blocking, **0-alloc on the steady path** (N1–N6, 005 §7).
- `ProcessingContext` is passed by reference, reused, never escapes a flow, never serialized (I6).
- Output nodes **stage**; they never do I/O inline. Flows are sync-first; blocking I/O hands off to a
  dedicated actor via `tell` (I1, 004 §4).
- Commands mutate, Events are immutable and published post-commit (I5).
- Device writes are fenced + authorized (006 §7). Secrets go to Quark 020, never into config JSON.
Cite the spec section (`(004 §4)`, `(I3)`) in comments for any non-obvious decision.

## R3 — Verification is mandatory (no green, no done)

Before you report a task complete, ALL of these must pass — run them, paste the evidence:
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure
CXX=clang++ cmake -S . -B build-clang && cmake --build build-clang -j && ctest --test-dir build-clang   # 2nd compiler
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DAEROEDGE_SANITIZER=address,undefined && cmake --build build-asan -j && ctest --test-dir build-asan
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DAEROEDGE_SANITIZER=thread && cmake --build build-tsan -j && ctest --test-dir build-tsan
```
- Two compilers (GCC + Clang) green, three sanitizers (ASan+UBSan, TSan) clean.
- Add a **deterministic, exit-code-gated test** (0 = pass) for the behavior you built, and a
  **pass/fail invariant gate** for any hot-path claim (see `tests/flow_zero_alloc.cpp` for the shape).
- NEVER report done with a failing/skipped test, a partial implementation, or an unresolved error.
  If blocked, keep the task in progress and report the blocker precisely.

## R4 — Use the real Quark API (don't invent it)

Ground every Quark call in a real header or sample. The verified bring-up pattern (from Quark
sample 01/03):
```cpp
detail::MessagePool pool(1024);
auto act = std::make_unique<Activation>(&actor, Actor::dispatch_table(), pool.sink());
Engine<> eng(EngineConfig{/*workers*/1, /*shards*/1, /*budget*/64, 64});
register_actor<Actor>(eng, /*key*/K, *act);
LocalRouter router(eng.post_courier(), pool);
auto ref = router.get<Actor>(K);
eng.start(); ref.tell(Msg{...}); auto r = block_on(ref.ask<R>(Query{})); eng.stop();
```
Read the relevant Quark spec + header + sample before using a subsystem (streaming → 024 +
`stream_channel.hpp` + sample 06; persistence → 012 + `persistence.hpp`/`snapshot.hpp`; transport →
010 + `transport.hpp`). If a Quark subsystem you need is spec'd but not implemented (e.g. real socket
transport 019/021), use its reference/loopback double for tests and report the gate — do not fake it.

## R5 — Honesty and scope

- If the plan's exit criteria can't be met with what Quark currently provides, deliver the honest
  subset, gate the rest, and say so explicitly. Do not overstate completion.
- Prefer the smallest change that satisfies the phase. No speculative abstractions beyond what a spec
  calls for. Match the surrounding code's style and density.
- Keep the specs in sync: if implementation reveals a spec gap, note it (and update the spec if that
  is the task).

## R6 — Report format (subagent → orchestrator)

Your final message is data for the orchestrator, not prose for a human. Return:
1. **Files changed** (paths, one line each on what/why).
2. **Verification evidence** — the exact ctest lines for GCC, Clang, ASan, TSan (pass/fail counts) and
   any invariant-gate output.
3. **Spec/plan deltas** — anything that diverged from the spec and why; any gate hit.
4. **Blockers** — anything unresolved, with the precise error.
Do not claim a gate passed without pasting its output.

## R7 — Git

Commit only whole, green milestones. One commit per phase/task, imperative subject, body explaining
what + the verification result. End every commit message with the Co-Authored-By trailer the
orchestrator uses. Never commit a red tree.
