# AeroEdge Studio

React + Vite web app to **configure, build, deploy, and monitor** AeroEdge flows — the Studio
plane of spec [013](../013-Solution-Topology-and-Studio.md) and the plugin-UI config model of
[015](../015-Configuration-Model-and-Studio-Plugin-UI.md). It talks **only** to `aero-api` (013 T2);
it never touches a device directly.

## Run

```bash
cd studio
npm install
npm run dev        # dev server on http://localhost:5173, proxies /api → the daemon
```

Point it at a running `aero-runtime` daemon (Phase 4):

```bash
# in the AeroEdge repo root, after cmake --build build
./build/aero-runtime --port 8080
# then, if the daemon isn't on 127.0.0.1:8080:
VITE_API_URL=http://<host>:<port> npm run dev
```

## What's here

- **Flow Designer** (`src/FlowDesigner.tsx`) — assemble a linear flow from the node catalog and emit
  a canonical Application JSON that deploys to the runtime unchanged.
- **Tier-1 schema-driven config** (`src/ConfigForm.tsx`, `src/catalog.ts`) — node config forms
  rendered from field descriptors, no per-node hardcoding (015 §3).
- **Tier-2 custom UI** (`src/tier2/ModbusRegisterMap.tsx`) — a rich Modbus register-map editor, the
  kind of protocol config a plain form can't express (015 §3).
- **API client** (`src/api.ts`) — the single audited path to the runtime; deploy/status/reload/
  rollback + SSE metrics. Runtime-assisted device discovery (015 §5) is gated offline (the browser
  never dials a device).

## Test / build

```bash
npm test -- --run   # vitest; the load-bearing test round-trips ../examples/hello_flow.json
npm run build       # tsc typecheck + vite build
```

## End-to-end (Studio ↔ daemon)

`scripts/e2e.sh` boots the `aero-runtime` daemon + the Vite dev server and drives the exact
requests the Studio's `api.ts` makes THROUGH the `/api` proxy (deploy → status → list → undeploy),
proving the full browser → Studio → proxy → daemon loop with no CORS. Requires the daemon built
(`cmake --build build`):

```bash
bash studio/scripts/e2e.sh   # prints "E2E OK" on success
```

## Gated

Live device discovery and a live-daemon end-to-end require a running runtime + device; those are
exercised manually against the daemon, not in unit tests. The node catalog is currently hardcoded to
the built-ins registered in `include/aero/runtime/runtime.hpp`; a future `GET /catalog` lets the
runtime serve it so the Studio can't drift (015 U1).
