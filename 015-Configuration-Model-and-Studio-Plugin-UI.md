# 015 — Configuration Model and Studio Plugin UI

> Draft v0.1. Every transport/driver/protocol plugin (MQTT, OPC UA, Modbus, …) must expose its
> **full, protocol-specific configuration**, and each needs its **own config UI**. That surface is
> genuinely huge. This spec makes it scale: one Studio host + a **plugin UI-contribution** model in
> two tiers, driven by a config schema that the runtime — not the UI — owns as source of truth.

## 1. The requirement, and why the naive approach fails

Each protocol's *complete* config is large and idiosyncratic:

- **MQTT** — broker URL(s), TLS/mTLS + cert chains, client id, clean vs persistent session,
  keepalive, Last-Will-and-Testament, per-topic QoS, topic filters/mappings, MQTT5 user
  properties / topic aliases / flow control, auth (user-pass / cert / token), bridging.
- **OPC UA** — endpoint discovery, security policy + message mode, **certificate trust-list
  management**, user auth (anon/user/cert), session + subscription params, **monitored items with
  data-change filters**, **address-space browsing**, historical access.
- **Modbus** — TCP vs RTU (serial: baud/parity/stop/flow), unit/slave ids, **register maps**
  (coils / discrete / holding / input), function codes, address ranges, **data types + byte/word
  order (endianness)**, per-point scaling/offset, polling groups + intervals, gap optimization.

**The naive approach — the Studio hard-codes a form per protocol — fails**: it doesn't scale to an
open plugin set (built-in + native + WASM + third-party, 008), it couples every protocol's release
to the Studio's release, and it bloats the host. So config UI must be **contributed by the plugin**,
not baked into the Studio.

> Reframe of "each must have its own UI system": **not N separate apps** — one Studio host that
> *hosts* per-plugin UI contributions. The weight lives in each plugin package, loaded on demand.

## 2. The config schema is the source of truth — owned by the runtime

Every plugin declares a **config schema** describing its full config surface: fields, types,
nesting, enums, defaults, per-field validation, conditional visibility, and which fields are
**secrets** (020). This schema:

- is part of the plugin's descriptor (005 `NodeDescriptor` / 006 `DriverDescriptor` / 014 transport
  adapter), and travels in the signed bundle (008 §5);
- lives in `aero-schema` (013 §4) as canonical, codegen'd to TS (Studio) + C++ (runtime) so the two
  **cannot drift** (013 T3);
- is what the runtime's `configure()` (005 §1 / 006 §5) validates against — **the runtime is the
  authority**. The UI is convenience; a config the runtime rejects is invalid no matter what the UI
  showed.

This single-source rule is what keeps a huge, evolving protocol config from getting out of sync
between "what the form offers" and "what the driver accepts."

## 3. Two tiers of config UI

### Tier 1 — schema-driven auto-forms (default, covers most fields)

The Studio renders a form **directly from the config schema** (dynamic/JSON-Schema-style forms):
text/number/enum/boolean/nested-object/array fields, validation, conditional visibility. A plugin
author who only needs ordinary fields ships **no UI code at all** — just the schema. This handles
the bulk of every protocol's config (connection strings, credentials, timeouts, simple options).

### Tier 2 — custom UI contribution (for full-feature, rich-interaction protocols)

The parts of a protocol that a generic form *cannot* express well get a **custom UI module** the
plugin ships and the Studio host mounts:

| Protocol | Needs a custom UI for | Interaction a plain form can't do |
|---|---|---|
| OPC UA | address-space **browser**, cert **trust manager**, monitored-item picker | live tree browse of the server, trust/reject certs |
| Modbus | **register-map editor** | grid of points: address, type, endianness, scale — with import/export |
| MQTT | topic-tree + subscription designer, cert manager | live topic discovery, per-topic QoS matrix |

This custom module **is** the "own UI system" the requirement asks for — but it plugs into one host,
reuses the shared UI SDK (§4), and is scoped to the rich parts; the mundane fields still come from
Tier 1. A plugin mixes both: schema fields for the simple 80%, a custom component for the hard 20%.

## 4. The Studio UI SDK — so authors don't rebuild, and UIs stay consistent

`aero-studio-sdk` (a published front-end package, part of the `AeroStudio` repo, 013) gives UI-plugin
authors:

- the **design system / component library** (inputs, tables, tree, modal, validation display) so
  every protocol UI looks and behaves consistently;
- the **host API** injected into a contribution: read/write the config object, report validation
  state, request **secrets** handling (020), and — critically — call **runtime-assisted actions**
  (§5);
- the **contribution contract**: how a plugin registers its custom component against a
  `(plugin type_id, config section)` slot.

Authors build a protocol UI *from* the SDK, not from scratch — that is what keeps "huge × many
protocols" from becoming "inconsistent chaos."

## 5. Runtime-assisted configuration (the non-obvious essential)

A protocol config UI is useless in a vacuum: browsing an OPC UA address space, testing an MQTT
connection, or reading back a Modbus register **requires talking to the actual device** — and only
the **runtime on the edge node** can reach it (the operator's browser cannot, and must not, dial the
plant network). So the Studio never touches devices directly:

```text
Studio UI plugin ──"browse address space"──▶ aero-api ──▶ aero-runtime ──▶ driver.discover()/test() ──▶ device
       ▲                                                                                                   │
       └────────────────────────── results (nodes / tags / test status) ◀───────────────────────────────┘
```

- The `IDriver`/transport contract gains optional **discovery/test hooks** (`test_connect`,
  `browse`, `read_probe`) the runtime exposes through `aero-api` (REST + WS/SSE for streamed browse
  results, 013 decision).
- The Studio's config UI drives those hooks — so an OPC UA browser is *live* against the real
  server, via the runtime, not a mock. This makes "full feature" real: discovery is a first-class
  part of config, mediated by the runtime.
- It also preserves the security boundary (013 T2, 020): all device contact goes through the audited
  API + the runtime's authorization, never the browser.

## 6. Packaging — one signed bundle, weight partitioned per plugin

An extension bundle (008 §5) is extended to carry, together and versioned as one unit:

```text
bundle <name>@<version> (signed):
  runtime:   IDriver/ITransport/INode artifact (.so / .wasm / built-in ref)   # 005/006/008/014
  schema:    config schema (→ aero-schema, codegen)                            # §2
  ui:        optional custom UI contribution (Tier 2 micro-frontend)           # §3
  caps:      requested capabilities (device access, secrets)                   # 008/020
```

- **The Studio host stays small.** Each protocol's large UI ships in its own bundle and is
  **lazy-loaded** only when an operator configures that protocol. "Huge" is partitioned across
  plugins and loaded on demand, never a monolith.
- **Runtime + schema + UI move together**, so a protocol can't present a UI for a feature its
  runtime driver doesn't implement (they're one signed, version-pinned unit — 008 §2, 013 T3).

## 7. Validation, secrets, defaults

- **Two-stage validation.** Tier-1 schema validation runs client-side for instant feedback; the
  **runtime `configure()` is the authority** and re-validates at deploy (005/006) — a mismatch fails
  the deploy, never a running actor (009 P1).
- **Secrets never sit in config JSON.** Fields marked secret (broker passwords, cert private keys,
  OPC user creds) are handed to Quark 020 secret storage; the Application definition references them
  by handle (009 §7 / 012 M5), the UI shows write-only inputs.
- **Defaults + profiles.** A schema ships sensible defaults; a plugin may ship named **profiles**
  (e.g. "OPC UA — Basic256Sha256 + user auth") so an operator starts from a working preset, not a
  blank full-feature form.

## 8. This model is uniform across ALL plugin kinds

The same config-UI contribution mechanism serves transports (014), drivers (006), nodes (005), and
MES adapters (012) — one extensibility surface, not a special case per protocol. A node's config
(a `ScaleNode`'s factor, an `ExprRuleNode`'s expression editor) is just a smaller instance of the
same Tier-1/Tier-2 split. This is the elegant payoff: "every plugin configures itself" is one
mechanism, reused.

## 9. Invariants (normative)

- **U1** — a plugin's config schema (in `aero-schema`) is the single source of truth; Studio UI is
  generated/validated from it and the runtime `configure()` is the authority (013 T3).
- **U2** — config UI is contributed by the plugin, never hard-coded in the Studio host; the host
  hosts, it does not know protocols.
- **U3** — simple config uses Tier-1 schema forms (no UI code); only rich interactions ship a Tier-2
  custom module, built on `aero-studio-sdk` for consistency.
- **U4** — all device contact for discovery/test goes through `aero-api` → runtime → driver; the
  Studio/browser never dials a device (013 T2, 020).
- **U5** — runtime artifact + config schema + UI contribution ship as one signed, version-pinned
  bundle (008); a UI can't offer what its runtime can't do.
- **U6** — secret fields go to Quark 020 storage and are referenced by handle; never stored in the
  Application/config JSON.
- **U7** — the Studio host stays small; per-protocol UI weight is lazy-loaded per bundle, not
  monolithic.

## 10. Open questions

- **Custom-UI technology (Tier 2).** Web Components / custom elements (framework-agnostic, strong
  isolation, works for untrusted plugins) vs Vite **Module Federation** (tighter React integration,
  shared runtime, weaker isolation) vs sandboxed iframe (max isolation for untrusted, clumsier UX).
  **Leaning Web Components + injected host API** as the contribution boundary, with iframe-sandboxing
  for *untrusted* third-party UI; confirm with the first Tier-2 plugin (OPC UA browser is the hard
  case). Trusted signing (008) narrows the isolation need.
- **Runtime-assisted discovery scope** — how much (`browse`, `test_connect`, `read_probe`, live tag
  subscribe-preview) is standard on the driver contract vs per-driver optional; grow from the OPC
  UA / Modbus needs.
- **Offline vs live config** — configuring against a real device (via runtime) vs authoring offline
  and validating at deploy; both needed (commissioning vs bulk authoring). Studio should support a
  "no live device" mode that skips §5 hooks.
- **Config schema expressiveness** — plain JSON Schema vs an AeroEdge superset (conditional
  visibility, cross-field validation, register-map/grid field types). Keep it standard where
  possible; extend only where protocol config demands it.
- **Versioning a protocol's config across upgrades** — a driver v2 that adds/renames config fields
  needs a config migration (mirrors 016/009 P5 state migration) so saved Applications still load.
