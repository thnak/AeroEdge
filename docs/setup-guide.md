# AeroEdge — Setup Guide

Get a working AeroEdge runtime on your machine: build the C++ daemon, verify it, run it, and
(optionally) run the Studio web UI against it. This is a from-source setup — AeroEdge is pre-1.0
(Draft v0.1) and does not yet ship prebuilt binaries or packages.

Once you're running, head to the **[User Guide](user-guide.md)** to deploy your first flow.

## 1. Prerequisites

| Requirement | Version | Why |
|---|---|---|
| A C++23 compiler | GCC 13+ or Clang 17+ (verified: GCC 14.2 / Clang 20) | `std::expected`, deducing-this, etc. |
| CMake | ≥ 3.24 | Build system |
| [QuarkCpp](../../QuarkCpp) | matching checkout | The actor engine AeroEdge builds on — a **sibling** checkout, not vendored |
| Node.js | 18+ / npm | Only if you want to run the **Studio** web UI |

You do **not** need `uv`/Python or an MQTT broker/gRPC stack for basic setup — those are only used
by the transport adapters' own test suite (see [CONVENTIONS.md](../CONVENTIONS.md) if you're
contributing to `aero-transport`).

## 2. Get the code

AeroEdge links QuarkCpp via `add_subdirectory`, so it expects QuarkCpp checked out **next to** this
repo by default:

```bash
cd ~/works                       # any parent directory — just keep them siblings
git clone https://github.com/thnak/AeroEdge.git
git clone https://github.com/thnak/QuarkCpp.git
```

```text
~/works/
├── AeroEdge/     ← you are here
└── QuarkCpp/     ← sibling checkout, never forked or vendored
```

If you keep QuarkCpp somewhere else, point CMake at it explicitly with `-DQUARK_DIR=/path/to/QuarkCpp`
in every `cmake -S` command below.

## 3. Build the runtime

```bash
cd AeroEdge
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces two binaries under `build/`:

- **`build/aero-runtime`** — the edge daemon (hosts your deployed flow + the REST/SSE management API).
- **`build/aero`** — the CLI (a thin `curl`-like client for `aero-runtime`'s API).

## 4. Verify the build

```bash
ctest --test-dir build --output-on-failure
```

Every test is deterministic and exit-code-gated (0 = pass) — you should see `100% tests passed`.
If a test named `mqtt_transport` or `grpc_transport` prints `SKIP` instead of `Passed`, that's
expected and not a failure: those two exercise real MQTT/gRPC backends hosted via `uv`, which is
optional (see the note in §1).

## 5. Run the daemon

```bash
./build/aero-runtime --port 8080
```

```text
aero-runtime listening on 0.0.0.0:8080
```

Leave it running (foreground, or `&`/a separate terminal). In another terminal, confirm it's alive:

```bash
curl http://127.0.0.1:8080/health
# ok
```

Flags: `aero-runtime [--app path.json] [--host 0.0.0.0] [--port 8080]` — `--app` deploys an
Application at startup instead of waiting for a `POST /apps`.

You're set up. Continue to the **[User Guide](user-guide.md)** to deploy and monitor your first flow.

## 6. (Optional) Run the Studio web UI

The Studio is a separate React + Vite app that talks only to `aero-runtime`'s REST API — useful if
you'd rather assemble/deploy/monitor flows visually than by hand-writing JSON + curl.

```bash
cd studio
npm install
npm run dev            # http://localhost:5173, proxies /api -> 127.0.0.1:8080
```

If your daemon isn't on `127.0.0.1:8080`, point the Studio at it:

```bash
VITE_API_URL=http://<host>:<port> npm run dev
```

Open `http://localhost:5173` — you should see the Flow Designer with the `hello_flow` example
pre-loaded, and (once you deploy it, either from here or via the CLI as in §5) live metrics
streaming in under **Deploy & Monitor**:

![AeroEdge Studio — Flow Designer + live Deploy & Monitor](../studio/docs/screenshot.png)

*`deployed: true · frames: 100 · events: 100 · last output: 198` confirms the daemon, the Studio,
and the proxy between them are all wired up correctly.*

See [studio/README.md](../studio/README.md) for what each panel does.

## Troubleshooting

**`QuarkCpp not found at '.../QuarkCpp'. Set -DQUARK_DIR=/path/to/QuarkCpp.`**
CMake couldn't find the sibling checkout. Either clone it next to `AeroEdge/` (§2) or pass
`-DQUARK_DIR=/absolute/path/to/QuarkCpp` on the `cmake -S` line.

**Compiler errors mentioning `std::expected` or concepts you don't recognize**
Your compiler is too old. Check `g++ --version` / `clang++ --version` against the table in §1 —
`CMAKE_CXX_STANDARD_REQUIRED` is ON, so an unsupported compiler fails at configure/compile, not silently.

**`failed to listen on 0.0.0.0:8080`**
Something else is already using that port. Pick another: `./build/aero-runtime --port 8081` (and
match it with `--url`/`VITE_API_URL` on whatever client you're using).

**A test other than `mqtt_transport`/`grpc_transport` fails or `ctest` reports less than 100%**
That's a real regression — please open an issue with the `ctest --output-on-failure` output. (If you're
building on anything other than Linux/x86-64, note that AeroEdge is only verified there today —
see [CONVENTIONS.md](../CONVENTIONS.md).)

**Building for development (sanitizers, second compiler, contributing)**
See [CONVENTIONS.md](../CONVENTIONS.md) and [AGENTS.md](../AGENTS.md) — the verification bar for
changes is stricter than "it builds" (two compilers, ASan+UBSan, TSan, all green).
