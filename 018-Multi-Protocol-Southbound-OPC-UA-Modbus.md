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

**Explicitly deferred** (M9.1, §8): writes (FC06 Write Single Register, FC16 Write Multiple
Registers — `IDriver::write()` stays at its default `Unsupported`), other register/data types
(coils FC01, discrete inputs FC02, input registers FC04), Modbus RTU/serial transport (TCP only).

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

**Explicitly deferred** (M9.1, §8): security policies (Sign/SignAndEncrypt, certificate-based
client auth), Subscriptions/MonitoredItems (OPC-UA's native push mechanism — v1 is pure poll,
matching the pull-driver shape 006 §6 already defines and keeping parity with the Modbus driver),
address-space browsing (v1 reads only configured NodeIds, no discovery), method calls.

## 6. Capability table

| Capability | Status | Where |
|---|---|---|
| Modbus-TCP: Read Holding Registers (FC03) | **shipped** | M9a, `ModbusTcpDriver` |
| Modbus-TCP: writes, other register types, RTU/serial | backlog | M9.1 |
| OPC-UA: no-security client, poll configured NodeIds | **shipped** | M9b, `OpcUaDriver` |
| OPC-UA: security policies, Subscriptions, browsing, method calls | backlog | M9.1 |
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
- **Modbus writes (FC06/FC16) and other register/data types** (coils, discrete inputs, input
  registers) — `IDriver::write()` stays `Unsupported` until an actuator/setpoint use case exists.
- **Modbus RTU / serial transport.** v1 is TCP-only; RTU needs a serial PAL this codebase doesn't
  have yet.
- **Multi-frame chunking** for register-maps/NodeId-lists wider than the 128-byte payload cap
  (§3) — would need either a bigger `kMaxFramePayload` (blocked on Quark's mailbox cell size,
  upstream QuarkCpp work) or splitting one wide poll into multiple `Frame`s per cycle. Revisit if
  a real deployment's tag count exceeds what one frame holds.
- **Device/register-map hot-reload** without a full flow redeploy — today, changing a driver's
  polled register/NodeId list means redeploying the Application (009 §4's hot-reload swaps the
  *flow*, not driver config).
- **OPC-UA address-space browsing + method calls** — v1 requires every NodeId to be known and
  configured up front; no discovery, no calling OPC-UA methods on the server.
