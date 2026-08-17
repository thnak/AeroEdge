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

**Explicitly deferred** (M9.1, §8): Modbus RTU/serial transport (TCP only).

## 5. Scope — M9b: OPC-UA

`OpcUaDriver` (`include/aero/drivers/opcua_driver.hpp`, type_id `aero.driver.opcua`) wraps
**open62541** (C99, Mozilla Public License 2.0), the standard open-source OPC-UA SDK — unlike
Modbus-TCP, OPC-UA's binary secure-channel protocol is not realistically hand-rollable. Vendored
via CMake `FetchContent`, the same shape as 017 M5's mbedTLS vendoring (`AERO_ENABLE_OPCUA`
option, forced-off noisy cache vars, wrapped as an `aero-thirdparty-opcua` INTERFACE library).

**v1 scope**: a `UA_Client` connecting with **no security policy** (`UA_ClientConfig_setDefault`,
the simplest open62541 client path — the vendored build itself is compiled with
`UA_ENABLE_ENCRYPTION=OFF`, deliberately not coupling this driver's crypto needs to 017 M5's
existing mbedTLS vendoring). Config: `endpoint` (`opc.tcp://host:port`), `node_ids` (a short list
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

**Explicitly deferred** (M9.1, §8): security policies (Sign/SignAndEncrypt, certificate-based
client auth), Subscriptions/MonitoredItems (OPC-UA's native push mechanism — v1 is pure poll,
matching the pull-driver shape 006 §6 already defines and keeping parity with the Modbus driver),
and — within browsing itself — continuation-point paging beyond the first `kMaxBrowseResults`
children, and reading nested subtrees (v1 is exactly one browse level per `poll()` call).

## 6. Capability table

| Capability | Status | Where |
|---|---|---|
| Modbus-TCP: Read Holding Registers (FC03) | **shipped** | M9a, `ModbusTcpDriver` |
| Modbus-TCP: Write Single Register (FC06) | **shipped** | M9.1 PR A, `ModbusTcpDriver::write()` |
| Modbus-TCP: Read Input Registers (FC04) | **shipped** | M9.1 PR B, `ModbusTcpDriver::ReadFunction` |
| Modbus-TCP: Write Multiple Registers (FC16) | **shipped** | M9.1 PR C, `ModbusTcpDriver::write()` |
| Modbus-TCP: Read Coils / Discrete Inputs (FC01/FC02) | **shipped** | M9.1 PR D, `ModbusBitsDecodeNode` |
| Modbus-TCP: RTU/serial transport | backlog | M9.1 |
| OPC-UA: no-security client, poll configured NodeIds | **shipped** | M9b, `OpcUaDriver` |
| OPC-UA: scalar write to a configured NodeId | **shipped** | M9.1 PR E, `OpcUaDriver::write()` |
| OPC-UA: single-argument method call | **shipped** | M9.1 PR F, `OpcUaDriver::write()` |
| OPC-UA: address-space browse (one root, one level, capped result count) | **shipped** | M9.1 PR G, `OpcUaDriver` browse mode |
| OPC-UA: security policies, Subscriptions, browse paging/nested subtrees | backlog | M9.1 |
| Frame byte-payload plumbing (driver → `ctx.payload`) | **shipped** | M9a, shared prerequisite |
| Multi-frame chunking beyond the 128B payload cap | backlog | M9.1 |
| Bounded-backoff reconnect on connection loss (006 §8) | **shipped** | both drivers |

## 7. Security

Both drivers dial *out* from AeroEdge to field equipment — the reverse exposure direction from
017's MQTT broker (which accepts inbound device connections and therefore needed TLS/ACL, 017
M5). Neither Modbus-TCP nor OPC-UA v1 here does connection authentication or encryption: Modbus-TCP
has no such concept in the base protocol (this is normal for the protocol, not a gap this spec
introduces); OPC-UA's security policies are explicitly deferred (§5, M9.1). This is an accepted v1
posture matching how these protocols are actually deployed today (a trusted OT network segment),
not a claim that either driver is safe to dial across an untrusted network — revisit if a
deployment needs that.

## 8. Open questions (M9.1)

- **OPC-UA security policies + cert-based auth.** Sign/SignAndEncrypt, client certificates —
  deferred because it would couple this driver's crypto needs to (or duplicate) 017 M5's mbedTLS
  vendoring, and no current deployment target needs it yet. Revisit alongside 017 §10's M5.1
  (cluster-link TLS) if/when either becomes a real requirement.
- **OPC-UA Subscriptions/MonitoredItems.** The OPC-UA-native push equivalent of MQTT's
  subscribe-and-get-notified — would let `OpcUaDriver` become a push (`run()`) driver instead of
  poll, lower latency and lower request volume than v1's poll loop. Revisit once poll-interval
  latency is a measured problem, not preemptively.
- **Modbus RTU / serial transport.** v1 is TCP-only; RTU needs a serial PAL this codebase doesn't
  have yet.
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
