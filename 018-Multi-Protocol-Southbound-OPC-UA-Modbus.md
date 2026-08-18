# 018 — Multi-Protocol Southbound: Modbus-TCP and OPC-UA

> Draft v0.1. Spec 017 gave AeroEdge its own MQTT broker so it terminates one southbound
> (device-facing) protocol locally instead of depending on external broker infrastructure. This
> spec extends the same posture to two more device-facing protocols that are common on the
> factory floor and were explicitly out of 017's scope: **Modbus-TCP** and **OPC-UA**. Unlike
> 017, these are not broker features — they are `IDriver` implementations under spec 006's
> existing driver/source-node contract. 017 itself flagged this as "likely its own spec (018?)"
> (017 §10); this is that spec.

## Status (updated as milestones ship)

| Milestone | What | Status |
|---|---|---|
| M9a | `Frame`/driver-pipeline byte-payload plumbing (shared prerequisite) + `ModbusTcpDriver` (FC03 poll, no new dependency) | **Shipped** |
| M9b | open62541 vendoring + `OpcUaDriver` (no-security client, poll configured NodeIds) | **Shipped** |
| M9.1 PR A | `ModbusTcpDriver::write()` — FC06 Write Single Register | **Shipped** |
| M9.1 PR B | `ModbusTcpDriver` — FC04 Read Input Registers (`register_type` config selector) | **Shipped** |
| M9.1 PR C | `ModbusTcpDriver::write()` — FC16 Write Multiple Registers (`"addr,v1,v2,..."` target form) | **Shipped** |
| M9.1 PR D | `ModbusTcpDriver` — FC01/FC02 Read Coils/Discrete Inputs + `ModbusBitsDecodeNode` | **Shipped** |
| M9.1 PR E | `OpcUaDriver::write()` — scalar write via `UA_Client_writeValueAttribute` | **Shipped** |
| M9.1 PR F | `OpcUaDriver::write()` — single-argument method call via `UA_Client_call` (`"object\|method"` target form) | **Shipped** |
| M9.1 PR G | `OpcUaDriver` — address-space browse mode (`UA_Client_Service_browse`, `browse_root` config) | **Shipped** |
| M9.1 PR H | `ModbusRtuDriver` + `aero::pal::serial` — Modbus RTU/serial transport (FC01/02/03/04/06/16) | **Shipped** |
| M9.2 | `Runtime` poll-timer wiring (006 §6.1) — makes `ModbusTcpDriver`/`ModbusRtuDriver`/`OpcUaDriver` actually deployable via a real Application, not just unit-testable | **Shipped** |
| M9.3 | `OpcUaSubscriptionDriver` — OPC-UA Subscriptions/MonitoredItems, the PUSH counterpart to `OpcUaDriver` | **Shipped** |
| M9.4 | OPC-UA security policies — Sign/SignAndEncrypt over a client certificate, cert-based client auth (`OpcUaSecurityConfig`, shared by `OpcUaDriver`/`OpcUaSubscriptionDriver`) | **Shipped** |

## 1. Why

AeroMes and prior AeroEdge deployment discussions descoped OPC-UA/Modbus gateway work the same
way EMQX's license forced 017's MQTT work — see 017 §1 and 017 §10's original "Southbound
OPC-UA/Modbus termination... a separate, later spec" entry. Modbus-TCP and OPC-UA are the two
most common PLC/SCADA-facing protocols this project's target deployments (industrial edge nodes
talking to existing field equipment) actually encounter; MQTT (017) covers modern/IIoT-native
devices, but a large fraction of installed industrial equipment only speaks one of these two
older, PLC-native protocols. Without a driver AeroEdge owns, ingesting from that equipment means
either a third-party gateway box (defeats the "AeroEdge is the edge node" pitch) or a licensed
SCADA/OPC-UA middleware layer (the same vendor-lock-in problem 017 solved for MQTT).

## 2. Model — this is a driver seam, not a broker feature

Spec 006 already defines the seam this work fills: `IDriver::open/run/poll/write/close/descriptor`
(`include/aero/sdk/driver.hpp`), registered by type_id into a `DriverRegistry`
(`include/aero/runtime/runtime.hpp`'s `register_builtins()`), and a push-vs-pull split (006 §6):
push drivers (TCP/Serial/MQTT/Camera) run a continuous `run()` loop; pull drivers (PLC/register
devices) implement `poll()`, triggered by a timer/Command, non-looping. **Both Modbus-TCP and
OPC-UA are pull drivers** — AeroEdge is the *client*, dialing into the PLC's Modbus-TCP server or
the device's OPC-UA server, matching how a SCADA system would normally poll these devices. This
is the opposite connection direction from 017's MQTT broker (which is a *server* devices dial
into) — southbound termination doesn't mean one connection shape, it means AeroEdge owns the
protocol endpoint locally either way.

006's own bright line (006 §1) — a Driver produces raw `Frame`s, a Source *node* decodes them
into `Tag`s — holds exactly here, and this spec's drivers deliberately do **no protocol-specific
decoding of their own**:
- `ModbusTcpDriver` delivers raw big-endian register bytes into a `Frame`'s payload; the existing
  `ModbusDecodeNode` (`include/aero/nodes/compute_nodes.hpp`, `aero.source.modbus`, shipped long
  before this spec as a testable decode-only node with no live transport) decodes them into
  `reg0/reg1/...` tags. **No changes to that node.**
- `OpcUaDriver` serializes its polled NodeId→value results as a flat JSON object into a `Frame`'s
  payload; the existing `JsonParseNode` (`aero.source.json`) decodes it into tags. **No changes
  to that node either.**

Both drivers exist specifically to fill the gap those two Source nodes were built ahead of:
`compute_nodes.hpp`'s own file header says real socket transports were "GATED" pending exactly
this kind of driver work. This spec is that transport layer landing.

**M9.2 correction**: from M9a through M9.1 PR H, "triggered by a timer/Command" above was
aspirational, not actual — `runtime.hpp`'s driver-ingestion path unconditionally spawned a
producer thread calling `drv->run(...)`, and every driver in this spec has `run()` as a hard
`Unsupported` (§6.1: they are `poll()`-only). `poll()` was reachable ONLY from each driver's own
test harness (a throwaway `StreamActivation` per call), never from a real deployed Application.
M9.2 closes this: `Deployment::poller` (`runtime.hpp`) is a thread that, on the configured
`rate_hz` cadence, stands up a small per-tick `StreamActivation` (mirroring the exact pattern the
test harnesses already used — `poll()`'s `StreamSink`-by-value contract has no way to hand a
producer token back for reuse across calls), calls `driver.poll(sink)` once, and `tell()`s
whatever frame(s) that call produced into the `FlowActor` — the real "timer/Command → poll(sink)
→ frames → stream" path §6.1 describes, finally implemented. `DriverDescriptor` gained a
`poll_driven` flag (`aero/sdk/driver.hpp`) so `Runtime::deploy()` can pick the poll lane vs the
existing push lane (producer+bridge threads) per driver, without probing `run()`/`poll()` at
runtime to find out which one a driver supports.

## 3. Architecture — the shared prerequisite

Before either driver could deliver a single real byte, `aero::Frame` (`aero/sdk/processing_context.hpp`)
carried only a scalar (`std::int64_t raw`) — the entire driver→actor pipeline (`ReceiveFrame` in
`aero/runtime/flow_actor.hpp`, the bridge thread in `aero/runtime/runtime.hpp`,
`ProcessingContext::reset()`) had no path for bytes to reach `ctx.payload`, the field both decode
nodes above read. M9a's first deliverable fixed this, addit ively:

```cpp
inline constexpr std::size_t kMaxFramePayload = 128;  // capped by Quark's mailbox message-pool
                                                        // cell (192B total struct budget), NOT
                                                        // an arbitrary choice — see the header.
struct Frame {
    std::int64_t raw = 0;
    std::uint16_t payload_len = 0;
    std::array<std::byte, kMaxFramePayload> payload{};
};
```

Kept trivially copyable (fixed inline array, no heap) so it stays cheap through a Quark 024
stream-ring slot and an actor mailbox `tell()`, exactly like the scalar-only `Frame` always was —
no new allocation or lifetime seam. **128 bytes, not the 512 originally sketched**: `ReceiveFrame`
travels through `quark::detail::MessagePool`'s inline cell, a hard, non-configurable ~192-byte
budget enforced by a compile-time `static_assert` at the message's send site. This is a real,
binding constraint on both drivers' config: `ModbusTcpDriver` caps `register_count` and
`OpcUaDriver` caps its `node_ids` list length so a single poll's serialized payload never exceeds
128 bytes — checked at `open()` time, a clean config-time rejection rather than a truncated or
corrupted frame at runtime. In practice this means **a handful of registers or NodeIds per poll
config**, not a large batch; a deployment needing a wider register/tag map runs multiple driver
instances (multiple flow deployments) rather than one wide poll. Multi-frame chunking to lift
this cap is backlog (§8, M9.1).

Both drivers also share a bounded exponential-backoff reconnect posture (200ms → 5s cap) inside
their `poll()` retry path on connection loss — the first drivers in this codebase to actually
implement spec 006 §8's "reconnect on ConnectionLost with bounded backoff" requirement (every
prior dial-out path in this repo, e.g. `MqttClientTransport::start()`, fails closed on the first
attempt with no retry at all).

The duplicated non-blocking-connect-with-timeout idiom (`getaddrinfo` → `quark::pal::net::tcp_connect`
→ `aero::pal::wait_writable` → `connect_result`), previously copy-pasted across
`mqtt_client_transport.hpp`, `broker/bridge.hpp`, and a test harness, is now a single shared
helper: `aero::pal::dial_tcp()` (`include/aero/pal/net_dial.hpp`), used by those three existing
call sites plus `ModbusTcpDriver`.

## 4. Scope — M9a: Modbus-TCP

`ModbusTcpDriver` (`include/aero/drivers/modbus_tcp_driver.hpp`, type_id `aero.driver.modbus_tcp`)
is a **hand-rolled** Modbus-TCP client — no third-party dependency. Modbus-TCP's wire format (an
8-byte MBAP header + function code + payload) is simple enough that vendoring a library (e.g.
libmodbus, LGPL-2.1) wasn't worth the license/build surface, matching 017's own stated preference
to avoid new dependencies where avoidable (017 §6).

**v1 scope**: function code `0x03` (Read Holding Registers) only. Config: `host`, `port` (default
502), `unit_id`, `start_address`, `register_count`. `poll()` sends one FC03 request per call,
validates the response (transaction id match, function code, byte count; a Modbus exception
response — high bit set on the function code — is a clean `DriverStatus::Error`, never a crash),
and delivers the raw register bytes to `ModbusDecodeNode` via the `Frame` payload described in §3.

**M9.1 PR A** shipped `IDriver::write()`: FC06 (Write Single Register) only. `DeviceCommand.target`
is the register address as a decimal string (`DeviceCommand` has no dedicated address field, 006
§7); `DeviceCommand.value` is the register value (rejected at `write()` if outside `0..65535`).
Same connect/reconnect/backoff path as `poll()` — a failed write drops the connection exactly like
a failed read does, and a well-formed Modbus exception response (0x86) is a clean `DriverStatus::Error`
with `last_exception_code()` set, never a crash.

**M9.1 PR B** shipped FC04 (Read Input Registers) alongside FC03: a `ReadFunction` enum
(`HoldingRegisters` | `InputRegisters`) selected at construction, defaulting to FC03 for backward
compatibility; the deploy-time JSON config selects it via `"register_type": "holding" | "input"`
(runtime.hpp factory). FC03 and FC04 are wire-identical register reads, so both share
`ModbusDecodeNode` unchanged — no new decode path needed.

**M9.1 PR C** shipped FC16 (Write Multiple Registers) — without widening `DeviceCommand` (006 §7,
still a shared SDK type also used by OTA): `write()` reads `cmd.target` as either a bare decimal
address (FC06, `cmd.value` supplies the value, unchanged from PR A) or a comma-separated
`"addr,v1,v2,..."` list (FC16, `cmd.value` unused in this form), up to Modbus's own FC16
implementation limit of 123 registers per call. Same connect/reconnect/backoff and exception
(0x90) posture as the other write paths.

**M9.1 PR D** shipped FC01 (Read Coils) and FC02 (Read Discrete Inputs) — bit-packed, not
word-packed like FC03/FC04, so they get their own decode node: `ModbusBitsDecodeNode`
(`aero.source.modbus_bits`) decodes `ctx.payload` into `"bit0","bit1",…` tags, LSB-first per byte
(Modbus's own packing rule), the FC01/FC02 counterpart to `ModbusDecodeNode`. A byte-packed
response can carry more bits than were actually requested (padding fills out the last byte); rather
than guess, `ModbusTcpDriver` stashes the exact requested count in `Frame::raw` for bit-packed
reads (otherwise unused by any Modbus decode path — see §3's `Frame` shape), and the decode node
trims to it, falling back to decoding every available bit when there's no `Frame` behind `ctx`
(e.g. driving the node directly in a test). `ReadFunction` gained `Coils`/`DiscreteInputs`
alongside `HoldingRegisters`/`InputRegisters`; the deploy-time JSON selects them via
`"register_type": "coils" | "discrete_inputs"` (paired with `"aero.source.modbus_bits"`
downstream, not `"aero.source.modbus"`). The open()-time payload-cap check and `do_transaction()`'s
byte-count math are both now function-of-`ReadFunction` (word: `count*2`; bit: `ceil(count/8)`)
rather than hardcoded to the word case.

**M9.1 PR H** shipped `ModbusRtuDriver` (`include/aero/drivers/modbus_rtu_driver.hpp`, type_id
`aero.driver.modbus_rtu`) — the serial counterpart, over a NEW `aero::pal::serial` PAL
(`include/aero/pal/serial.hpp`, this tree's first serial I/O of any kind: `CreateFileA`/`DCB`/
`SetCommState`/`SetCommTimeouts` on Windows, `termios`/`VMIN`+`VTIME` on POSIX). Same FC01/02/03/04
read + FC06/FC16 write surface and `cmd.target` encoding as `ModbusTcpDriver`, but NOT a refactor of
it — RTU framing (`[addr:1][PDU][CRC16-LE:2]`, no length field, no transaction id, the request/
response pairing implicit in half-duplex serial ordering) is different enough from MBAP+TCP that
`ModbusRtuDriver` duplicates the short FC-byte-encoding shapes with RTU framing instead, rather than
touching the shipped, working TCP driver. Modbus's own CRC-16 (poly `0xA001` reflected, init
`0xFFFF`) is a distinct algorithm from `CrcNode`'s CCITT-FALSE — implemented once in the driver,
verified against the Modbus spec's own worked example in the test suite. Same bounded-backoff
reconnect posture as every other driver here (opening a serial port can fail exactly like a TCP
dial can). Config: `port`, `baud_rate`, `slave_address`, `start_address`, `register_count`,
`register_type`, `parity` (`N`/`E`/`O`), `stop_bits` (1/2) — mirrors `aero.driver.modbus_tcp`'s
selector fields with serial-specific fields swapped in for `host`/`port`/`unit_id`.

**Testability**: unlike a TCP loopback server, there is no portable way to fake a COM port without
real hardware or a third-party virtual-COM driver — so `ModbusRtuDriver` talks to a small
`ISerialTransport` seam instead of `aero::pal::serial` directly. Production code gets
`RealSerialTransport` (a thin PAL wrapper); the test suite injects an in-memory fake that echoes
canned RTU frames built independently (its own CRC16, not the driver's). This is the only
abstraction this PR adds beyond duplicating `ModbusTcpDriver`'s shape, and exists purely because
hardware-free testing has no other option.

## 5. Scope — M9b: OPC-UA

`OpcUaDriver` (`include/aero/drivers/opcua_driver.hpp`, type_id `aero.driver.opcua`) wraps
**open62541** (C99, Mozilla Public License 2.0), the standard open-source OPC-UA SDK — unlike
Modbus-TCP, OPC-UA's binary secure-channel protocol is not realistically hand-rollable. Vendored
via CMake `FetchContent`, the same shape as 017 M5's mbedTLS vendoring (`AERO_ENABLE_OPCUA`
option, forced-off noisy cache vars, wrapped as an `aero-thirdparty-opcua` INTERFACE library).

**v1 scope**: a `UA_Client` connecting with **no security policy** by default
(`UA_ClientConfig_setDefault`, the simplest open62541 client path) — M9.4 (below) later added an
OPT-IN `OpcUaSecurityConfig` for Sign/SignAndEncrypt over a client certificate; the vendored build
itself now compiles with `UA_ENABLE_ENCRYPTION=MBEDTLS`, sharing 017 M5's existing mbedTLS
vendoring rather than a second copy. Config: `endpoint` (`opc.tcp://host:port`), `node_ids` (a short list
of NodeId strings — bounded by the 128-byte payload cap, §3). `poll()` reads each configured
NodeId's value via `UA_Client_readValueAttribute`, converts numeric variant types to `double`,
serializes the NodeId→value map as flat JSON into the `Frame` payload for `JsonParseNode`.

**M9.1 PR E** shipped `IDriver::write()` via `UA_Client_writeValueAttribute` — the OPC-UA
counterpart to `ModbusTcpDriver`'s FC06. `cmd.target` is a NodeId string, parsed the same way as a
configured `node_ids` entry (`UA_NodeId_parse`, checked BEFORE touching the connection, so a
malformed target never dials); `cmd.value` is written as a `UA_Double`, matching this driver's own
read-side convention of normalizing every value to `double`. Same connection-loss detection/
teardown posture as `poll()` (006 §8): a genuine channel/session loss tears the session down for
the next `poll()`/`write()` to reconnect, while a well-formed OPC-UA error over a healthy
connection (bad NodeId, read-only node, type mismatch) is a clean `DriverStatus::Error` with the
connection left up. `kDesc.writable` flipped to `true`.

**M9.1 PR F** shipped a v1 method call via `UA_Client_call` — reusing `write()` rather than adding
new `IDriver` surface, the same "encode structure into `cmd.target`" move as
`ModbusTcpDriver`'s FC16 (018 PR C). A `"|"` in `cmd.target` switches from PR E's bare-NodeId write
form to `"objectNodeId|methodNodeId"`: calls the method with **exactly one** scalar `UA_Double`
input argument (`cmd.value`); any output arguments are freed unread. Both NodeId halves are parsed
before the connection is touched, same as PR E. 0-arg/multi-arg calls and reading outputs are v1.1
backlog (§8) — `DeviceCommand`'s single-value shape can't carry an argument list without a wider
SDK change, the same constraint that bounded Modbus FC16 to a comma-separated `target`.

**M9.1 PR G** shipped a v1 address-space browse mode via `UA_Client_Service_browse` — a THIRD,
mutually-exclusive constructor mode (`browse_root`, alongside the scalar `node_ids` poll mode),
not another `write()`-reuse trick: a browse result (child NodeId + BrowseName pairs) doesn't fit
the `{nodeId: value}` JSON shape the scalar path emits, so `poll()` itself branches into
`browse_once()` when the driver was constructed with a non-empty `browse_root` (`open()` rejects a
config that sets both `node_ids` and `browse_root`). Browses exactly the configured root's
HierarchicalReferences children (Organizes/HasComponent/HasProperty/...; `HasTypeDefinition` and
other non-hierarchical references are excluded — the conventional "list an object's children"
restriction), one level, no continuation points: `requestedMaxReferencesPerNode` is capped at
`kMaxBrowseResults` (3) so the server itself truncates, keeping the JSON array
(`[{"id":...,"name":...}, ...]`) inside the 128-byte payload cap by construction. No dedicated
decode node ships for this shape — v1 puts the raw JSON array straight into the `Frame` payload for
a caller/flow to consume directly (`JsonParseNode`, the scalar path's own decoder, does not
understand this shape).

**M9.3** shipped `OpcUaSubscriptionDriver` (`opcua_subscription_driver.hpp`, type_id
`aero.driver.opcua_subscribe`) — OPC-UA Subscriptions/MonitoredItems, closing the "Subscriptions"
half of the deferral above. A SEPARATE class/type_id from `OpcUaDriver`, not a mode flag on it:
push vs pull is a different `IDriver` invocation contract (`run()` vs `poll()`,
`DriverDescriptor::poll_driven`, M9.2), and `DriverDescriptor` is `static constexpr` per-CLASS, not
per-instance — every driver in this tree already commits to exactly one invocation model
(`GeneratorDriver` push-only, `ModbusTcpDriver`/`ModbusRtuDriver`/`OpcUaDriver` pull-only), and this
follows that precedent. `run()` connects, creates ONE Subscription (open62541 defaults — 500ms
publishing interval) with one MonitoredItem (data-change, `UA_ATTRIBUTEID_VALUE`) per configured
NodeId, then pumps `UA_Client_run_iterate()` until stopped; open62541's client has no internal
thread, so the data-change callback fires synchronously on `run()`'s own thread — safe to push into
the bound sink directly, no cross-thread handoff. Each Frame carries exactly ONE
`{"nodeId": value}` entry (one per notification), NOT a batched multi-NodeId object like poll
mode's own payload — so subscription mode has no analogue to poll mode's "~3 NodeIds per poll"
batching cap (§3); the only per-Frame constraint is that ONE NodeId-string+value pair fits under
128 bytes, checked per-entry at `open()`. No write()/method-call surface in v1 (a deployment
needing both notifications and writes to the same endpoint runs an `OpcUaDriver` instance
alongside this one). Reconnect (006 §8) rebuilds the Subscription/MonitoredItems from scratch on
any connection loss — open62541 does not resurrect a Subscription across a fresh session — and
leans on open62541's own `connectivityCheckInterval` (set to 2s, shortened from its 5s-timeout
defaults) for an active liveness check inside `run_iterate()`, rather than passively waiting on
channel/session state alone (a peer that vanishes without a clean TCP FIN can leave that looking
nominally healthy for a long time).

**M9.4** shipped OPC-UA security policies — Sign/SignAndEncrypt over a client certificate, closing
018 §8's last backlog item. `OpcUaSecurityConfig` (`opcua_security.hpp`, shared by both
`OpcUaDriver` and `OpcUaSubscriptionDriver` via an additive, default-constructed-disabled
constructor argument — every pre-M9.4 call site and deploy config keeps connecting at
`MessageSecurityMode::None` unchanged) turns on Sign or SignAndEncrypt over DER certificate/key
files loaded from disk (mirrors `aero/pal/tls.hpp`'s own `cert_file`/`key_file` PEM-file
convention, not inline bytes/base64 in deploy JSON — private key material shouldn't live in a
config file that might get logged or committed). v1 scope, deliberately narrow like every other
slice in this file: ONE trusted peer certificate (`trusted_server_certificate_file`), not a CA
chain/multi-entry trust store; Sign/SignAndEncrypt only, no plaintext-but-signed-elsewhere hybrid;
`application_uri` must be set to match `certificate_file`'s own X.509 URI Subject Alternative Name
byte-for-byte (open62541's mbedTLS plugin verifies a peer's claimed ApplicationUri against its
cert via a raw substring search of the cert's v3 extension bytes — a mismatch is
`BadCertificateUriInvalid`, discovered building this milestone's own test fixture). "Cert-based
client auth" here means the SecureChannel itself is mutually authenticated by X.509 certs — the
client's own cert IS its identity for the channel, verified against the server's trust list — not
a session-level X.509 `UserIdentityToken` (that stays backlog, see below).

Requires the vendored open62541 to be built with `UA_ENABLE_ENCRYPTION=MBEDTLS` (root
`CMakeLists.txt`'s `AERO_ENABLE_OPCUA` block, now gated on `AERO_ENABLE_TLS=ON`) — sharing this
project's own already-vendored mbedTLS (017 M5) rather than fetching/linking a second, independent
copy: two separately-built static libs both exporting `mbedtls_x509_crt_parse` et al. into one
binary is a real duplicate-symbol hazard, not just wasted size (see
`cmake/patch_open62541.cmake`'s banner). Getting there needed two more fixes to open62541's own
vendored CMakeLists.txt, both via the same content-matched `PATCH_COMMAND` mechanism 017 M5's
`cmake/patch_mbedtls.cmake` already established: (1) open62541's `install(EXPORT)`/`export(TARGETS)`
rules for its own target fail CMake's generate-time export-set validation once it links AeroEdge's
mbedTLS targets (this project never installs/exports either, so these rules are dropped, not
worked around); (2) `mbedtls_entropy_self_test()` (called once per SecurityPolicy setup) is
undeclared once `MBEDTLS_SELF_TEST` is disabled project-wide (017 M5's own TSan-race fix) — each
call site is patched to skip the self-test rather than reintroducing that race for a one-time RNG
check.

§8's real answer on the mbedTLS-sharing threading hazard: **yes, a genuine hazard exists**, found
empirically standing up this milestone's own security-enabled test fixture — every open62541
SecurityPolicy failed to initialize (`mbedtls_ctr_drbg_seed` returning
`MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED`, deterministically, every time) until
`aero::pal::tls::detail::ensure_threading_registered()` (017 M5's `MBEDTLS_THREADING_ALT`
registration) ran first. `MBEDTLS_THREADING_ALT` requires `mbedtls_threading_set_alt()` before
**any** other mbedTLS call in the process, full stop — not specific to TLS. `apply_security_config()`
now calls it before every mbedTLS-touching open62541 call (`std::call_once`-guarded, so it's a
no-op if the native broker's own TLS listener already registered it, and vice versa) — one shared
registration for the one shared mbedTLS build, exactly the fix 017 M5's own shim was designed to
generalize to.

**Still deferred** (v2 backlog): a CA-chain/multi-entry trust store (v1 is exactly one trusted
peer cert, matching this driver's existing "one configured endpoint" posture); a session-level
X.509 `UserIdentityToken` for user (not channel) authentication (`UA_ClientConfig_
setAuthenticationCert`, a related but separate OPC-UA mechanism M9.4 didn't need); certificate
rotation/expiry handling (v1 loads cert/key files once, at `open()`); and — within browsing itself
— continuation-point paging beyond the first `kMaxBrowseResults` children, and reading nested
subtrees (v1 is exactly one browse level per `poll()` call). Within Subscriptions (M9.3): Events
(only DataChange notifications), server-requested-parameter renegotiation (accepts whatever the
server revises), and resubscribing to an EXISTING subscription id after a reconnect (v1 always
creates a fresh one).

## 6. Capability table

| Capability | Status | Where |
|---|---|---|
| Modbus-TCP: Read Holding Registers (FC03) | **shipped** | M9a, `ModbusTcpDriver` |
| Modbus-TCP: Write Single Register (FC06) | **shipped** | M9.1 PR A, `ModbusTcpDriver::write()` |
| Modbus-TCP: Read Input Registers (FC04) | **shipped** | M9.1 PR B, `ModbusTcpDriver::ReadFunction` |
| Modbus-TCP: Write Multiple Registers (FC16) | **shipped** | M9.1 PR C, `ModbusTcpDriver::write()` |
| Modbus-TCP: Read Coils / Discrete Inputs (FC01/FC02) | **shipped** | M9.1 PR D, `ModbusBitsDecodeNode` |
| Modbus RTU/serial transport (FC01/02/03/04/06/16) | **shipped** | M9.1 PR H, `ModbusRtuDriver` + `aero::pal::serial` |
| OPC-UA: no-security client, poll configured NodeIds | **shipped** | M9b, `OpcUaDriver` |
| OPC-UA: scalar write to a configured NodeId | **shipped** | M9.1 PR E, `OpcUaDriver::write()` |
| OPC-UA: single-argument method call | **shipped** | M9.1 PR F, `OpcUaDriver::write()` |
| OPC-UA: address-space browse (one root, one level, capped result count) | **shipped** | M9.1 PR G, `OpcUaDriver` browse mode |
| OPC-UA: Subscriptions/MonitoredItems (data-change push) | **shipped** | M9.3, `OpcUaSubscriptionDriver` |
| OPC-UA: security policies — Sign/SignAndEncrypt, cert-based client auth | **shipped** | M9.4, `OpcUaSecurityConfig` |
| OPC-UA: CA-chain/multi-entry trust store, X.509 user identity token, browse paging/nested subtrees, Subscription Events | backlog | M9.1/M9.3/M9.4 |
| Frame byte-payload plumbing (driver → `ctx.payload`) | **shipped** | M9a, shared prerequisite |
| Multi-frame chunking beyond the 128B payload cap | backlog | M9.1 |
| Bounded-backoff reconnect on connection loss (006 §8) | **shipped** | all drivers |
| `Runtime::deploy()` actually drives PULL drivers (006 §6.1 timer/poll wiring) | **shipped** | M9.2, `Deployment::poller` |

## 7. Security

Both drivers dial *out* from AeroEdge to field equipment — the reverse exposure direction from
017's MQTT broker (which accepts inbound device connections and therefore needed TLS/ACL, 017
M5). Modbus-TCP does no connection authentication or encryption in either direction — the base
protocol has no such concept (this is normal for the protocol, not a gap this spec introduces).
OPC-UA now has a REAL security option (M9.4): a deployment can opt an `OpcUaDriver`/
`OpcUaSubscriptionDriver` instance into Sign/SignAndEncrypt over a client certificate via
`OpcUaSecurityConfig` (§5), authenticating the SecureChannel in both directions (the client's cert
against the server's trust list, and — via `trusted_server_certificate_file` — the server's cert
against the client's own single-entry trust). It stays OPT-IN, not the default: a config that
doesn't set `certificate_file` connects at `MessageSecurityMode::None` exactly as before M9.4, and
that remains an accepted posture for a trusted OT network segment where the operational cost of
managing certs isn't justified. Neither driver claims to be safe dialing an untrusted network
without security enabled — revisit which posture is the DEFAULT if a deployment target changes.

## 8. Open questions (M9.1/M9.2/M9.3/M9.4)

- **OPC-UA security policies v1 limits (M9.4).** See §5's own writeup for the shipped scope and
  the "still deferred" list (CA-chain/multi-entry trust store, X.509 user identity token, cert
  rotation). Revisit alongside 017 §10's M5.1 (cluster-link TLS) if a deployment needs any of
  those.
- **OPC-UA Subscriptions v1 limits (M9.3).** `OpcUaSubscriptionDriver` ships DataChange
  notifications only (no Events), open62541's default subscription parameters (no per-deployment
  tuning of publishing interval/queue size yet), and always creates a FRESH Subscription on
  reconnect rather than resubscribing to an existing id (open62541 doesn't resurrect one across a
  new session anyway, so there's nothing to resume). Also: this driver's own test found that
  detecting a peer that vanishes WITHOUT a clean TCP FIN can be very slow via passive channel/
  session-state checks alone (30+ seconds observed) — `connectivityCheckInterval` (2s) mitigates
  this for real network failures, but a test harness that tears a UA_Server down via
  `UA_Server_delete()` mid-session (not a real-world "cable unplugged" scenario) may not exercise
  it reliably; revisit if a real deployment shows slow reconnect after a genuine network drop.
- **M9.2 poll-timer v1 limits.** `Deployment::poller`'s cadence is a plain `sleep`-based loop
  (20ms wake-up granularity to check `stop_flag`), not a real Quark timer/scheduler primitive
  (006 §6.1's original diagram sketched "Quark timer (011)" — 011 doesn't exist as a reusable
  primitive yet, so this is a Runtime-owned thread instead); a fresh `StreamActivation` per tick
  has real per-tick overhead that would matter at a much higher `rate_hz` than any current PLC/
  OPC-UA polling target uses. Revisit alongside 011 if/when a real Quark timer facility lands, or
  if a deployment needs sub-20ms poll jitter.
- **Modbus RTU serial-config breadth.** PR H ships the common case (8 data bits fixed per spec,
  configurable baud/parity/stop-bits, no RS-485 driver-enable/DE-RTS toggling — assumes the
  serial adapter or a transceiver handles half-duplex turnaround itself). Revisit if a real
  deployment's RS-485 hardware needs the driver to control DE/RE lines directly.
- **Multi-frame chunking** for register-maps/NodeId-lists wider than the 128-byte payload cap
  (§3) — would need either a bigger `kMaxFramePayload` (blocked on Quark's mailbox cell size,
  upstream QuarkCpp work) or splitting one wide poll into multiple `Frame`s per cycle. Revisit if
  a real deployment's tag count exceeds what one frame holds.
- **Device/register-map hot-reload** without a full flow redeploy — today, changing a driver's
  polled register/NodeId list means redeploying the Application (009 §4's hot-reload swaps the
  *flow*, not driver config).
- **OPC-UA method calls: 0-arg / multi-arg, reading outputs.** PR F's v1 is exactly one scalar
  input, outputs discarded — see PR F's note above on why (`DeviceCommand`'s single-value shape).
- **OPC-UA browse paging + nested subtrees.** PR G ships one root, one level, capped at
  `kMaxBrowseResults` (3) children via `requestedMaxReferencesPerNode` — a root with more children
  than that is silently truncated server-side (documented v1 constraint, not a bug). A v2 would
  consume the browse response's own (currently unread) `continuationPoint` via
  `UA_Client_Service_browseNext` to page through the rest, and/or recurse into child NodeIds for a
  full subtree walk — both out of scope for "smallest independent slice."
