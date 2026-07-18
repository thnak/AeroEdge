# 006 — Drivers and Sources

> Draft v0.1. How device data physically enters AeroEdge. A **Driver** bridges a device
> protocol (PLC/MQTT/TCP/Serial/Camera) into frames and feeds them into an actor's inbound
> stream; the flow then processes those frames. This spec resolves the `Frame`-lifetime
> question left open in 003 §6 by pinning the design to Quark's streaming primitive (024).

## 1. Driver vs Source node — the bright line

Restating 005 §2 precisely, because everything here depends on it:

| | **Driver** (this spec) | **Source node** (005) |
|---|---|---|
| Lives | outside the flow, on an I/O lane | inside a flow |
| Does | device protocol I/O; produces frames | decodes/shapes an already-arrived frame |
| Touches a socket? | **yes** (via Quark PAL, 019) | **never** (I1) |
| Blocking allowed? | yes — on its own lane (`BlockingHandler`) | no (N1) |
| Feeds | the actor's `StreamChannel` | the next node in the DAG |

A flow never opens a connection. A driver never decodes business meaning. The driver's
whole job is: *bytes off the wire → framed → into the stream*. From there it's a
Command-triggered flow (002/004).

## 2. The ingestion path (normative)

```text
 Device ──▶ Driver (read loop, blocking lane, PAL sockets 019)
                │  frame the bytes  (010/016 own the wire format)
                ▼
        StreamChannel<Frame>::try_push(frame)      ← Quark 024 credit ring (SPSC)
                │  (single-producer token: exactly one driver per stream)
                ▼
        Stream activation drains a StreamBatch      ← Quark 024/002 scheduler
                │  each frame → a ReceivePacket-shaped trigger
                ▼
        CompiledFlow::execute(ctx)  with ctx.frame viewing the live slot   (004)
                │
                ▼  batch.retire()  → returns credit → un-stalls the driver
```

The driver is the **producer**; the actor's stream activation is the **consumer**. This is
exactly Quark sample 06, with the driver as the producer side and a flow as the consumer
body. AeroEdge writes **no** ring, no scheduler, no wakeup — only the driver's read loop and
the per-frame "run the flow" glue.

> **Cross-node streams.** The same StreamChannel also carries frames *across nodes* — the
> producer can be the Transport (024 `open_stream<F>(actor_id, transport_ep)`), so an `EdgeActor`
> can stream normalized samples to an aggregating `LineActor` on a gateway node with FIFO + credit
> backpressure preserved end-to-end — **but only over a flow-controlled transport (TCP/gRPC)**; a
> broker (MQTT) breaks the credit loop. See [014 §7](014-Transport-Interface-and-Pluggable-Transports.md).

## 3. Backpressure — never drop, apply upstream (024 posture)

Quark streaming is **backpressure, not shedding**: `try_push` returns `false` when the ring
has no credit. A driver **must** treat that as a signal to *stop reading the device*, not to
drop the frame:

- **Stream sockets (TCP/Serial):** stop reading; the OS socket buffer fills and TCP flow
  control back-pressures the sender. Resume reading when credit returns (the stream
  activation's drain returns credit via `retire()`).
- **Pull sources (PLC poll):** skip/delay the next poll while credit is exhausted.
- **Lossy sources (some MQTT QoS0, camera):** the driver decides its overflow policy
  *explicitly* (coalesce-latest, or count-and-drop with a metric) — dropping is a declared
  driver property, never an accident of a full ring.

This makes overload behavior a property of the driver, visible and testable, aligning with
Quark 022 governance.

## 4. `Frame` lifetime — resolving 003 §6

Quark's inline slot regime copies a frame (≤ `kStreamInlineMax`, 56 B) *into* the ring slot,
so it is immune to the producer overwriting it. AeroEdge uses this to make `ctx.frame` safe
without a copy in the common case:

- **Small frames (≤56 B):** the frame lives inline in the ring slot. `ctx.frame` **views the
  live slot** for the duration of the flow; the flow runs *before* `batch.retire()`, so the
  slot cannot be reused mid-flow. Zero copy, trivial lifetime (I6 holds).
- **Large payloads (> inline):** the driver stages the payload into a **shard-owned payload
  arena** (Quark 003 `pmr`) and the inline frame carries a `{handle, len}`. `ctx.frame`
  views the arena buffer; the arena slot is reclaimed on `retire()`. The by-reference
  registered-RX / zero-copy DMA path is Quark's `StreamMode::ZeroCopyRetained` seam
  (024 → 019/003), which AeroEdge adopts unchanged when a transport supports it.

**Rule (normative, closes 003 §6):** a node must not retain `ctx.frame` past the flow
execution (N4/I6). If a downstream Command needs frame bytes, it *copies them out* into that
Command's payload — the frame view dies with the flow.

## 5. Driver contract

```cpp
namespace aero {

class IDriver {
public:
    virtual ~IDriver() = default;

    // Connect/authenticate to the device. Called on actor activation and on re-activation
    // after migration (010 §3). May block — runs on the driver's I/O lane, never a flow.
    virtual DriverStatus open(const DriverConfig& cfg) noexcept = 0;

    // Producer loop for PUSH drivers (TCP/Serial/MQTT/Camera): read → frame → try_push into
    // the bound StreamChannel, honoring backpressure (§3). Returns when the actor stops or
    // the connection drops (→ ConnectionLost event, reconnect policy).
    virtual DriverStatus run(StreamSink<Frame> sink, StopToken stop) noexcept = 0;

    // One read for PULL drivers (PLC poll): triggered by a Command/Timer (§6.1). Produces
    // zero or more frames into the sink. Non-looping.
    virtual DriverStatus poll(StreamSink<Frame> sink) noexcept { return DriverStatus::Unsupported; }

    // Write toward the device (actuators, OTA transfer §7). Fenced + authorized (§7).
    virtual DriverStatus write(const DeviceCommand& cmd) noexcept { return DriverStatus::Unsupported; }

    virtual void close() noexcept = 0;
    virtual const DriverDescriptor& descriptor() const noexcept = 0;   // capabilities (§7)
};

} // namespace aero
```

`StreamSink<Frame>` is a thin handle over the actor's `StreamChannel<Frame>` producer side
(the single-producer token, 024). Exactly **one** driver binds one stream — a second bind is
a typed error (mirrors Quark 024's SPSC precondition).

## 6. Push vs pull drivers

### 6.1 Pull (PLC / polled registers)

Polled devices don't push; AeroEdge polls them on a timer:

```text
Quark timer (011) ─every N ms─▶ PollPLC Command ─▶ EdgeActor.handle(PollPLC)
        └─▶ driver.poll(sink)  →  frames  →  stream  →  PollFlow
```

The timer is Quark's (no AeroEdge timer, 004 §5). `poll` runs on the driver's blocking lane
if the device read blocks; results enter the same stream as push drivers, so the flow layer
is identical.

### 6.2 Push (TCP / Serial / MQTT / Camera)

A continuous `run` loop reads and pushes frames as they arrive. The loop lives on a
`BlockingHandler` lane (Quark ADR-015) so a slow/blocking read never stalls a scheduler
worker (I1). Connection loss → emit `ConnectionLost`, apply the reconnect policy (§8).

## 7. Write-capable drivers (actuators, OTA transfer)

Some drivers write *to* the device: actuator setpoints, and the OTA image transfer (011).
These are safety-critical, so:

- **Fenced.** Only the device's owning `EdgeActor` may drive writes; migration is fenced
  (010 §3) so two nodes never write one device concurrently. This is a hard industrial-safety
  requirement, delegated to Quark 021, not re-implemented.
- **Authorized.** A `write` originating from a Command carries a Quark principal (020); the
  driver/actor checks authorization before actuating. Actuator and OTA commands may require
  elevated authorization (012 §7, 011 §7).
- **Capability-gated.** `descriptor()` advertises what the driver supports
  (`readonly` / `writable` / `ab-slots` / max frame size / protocol). The Flow Compiler and
  OTA state machine (011) read these capabilities; a flow that writes to a `readonly` driver
  fails validation, not at runtime.

## 8. Lifecycle, reconnect, and migration

- **`open` on activation.** When an `EdgeActor` activates (including after a fenced migration
  to a new node, 010 §3), it `open`s its driver before resuming flows. Placement affinity
  (010 §2.1) ensures the new node has line-of-sight to the device.
- **Reconnect policy.** On connection loss the driver emits `ConnectionLost` and retries with
  bounded backoff (a driver config knob); Quark 022 rate-limits reconnect storms. Persistent
  failure escalates through Quark supervision (007).
- **Graceful stop.** On actor stop/drain, `run` observes the `StopToken`, finishes the
  in-flight frame, and `close`s — no frames lost mid-batch (024 exactly-once).

## 9. Drivers as extensions

Like nodes (005 §6), drivers are a seam with three implementations behind one `IDriver`:

| Kind | What | Status |
|---|---|---|
| Built-in | compiled-in `IDriver` (TCP, MQTT, Modbus, OPC UA) | v0.1 targets |
| Native | vendor `.so`/`.dll` implementing `IDriver` (proprietary PLC/camera protocols) | planned (008) |
| WASM | sandboxed protocol driver | future (008) |

Vendor binary protocols (the "Socket → Vendor Binary → RawFrame" chain in the original
sketch) are exactly what a Native driver encapsulates: it owns the vendor decoding down to a
`RawFrame`, and the flow's Source/Transform nodes take it from there.

## 10. Invariants (normative)

- **D1** — a driver is the sole producer of its actor's stream (single-producer token, 024).
- **D2** — a driver never drops silently; overflow behavior is a declared driver property (§3).
- **D3** — all socket/serial/timer I/O goes through the Quark PAL (019); no raw POSIX in a
  driver's logic.
- **D4** — device writes are fenced (single owning actor) and authorized (§7).
- **D5** — `ctx.frame` views a live slot/arena buffer only for the flow's duration; bytes that
  must outlive the flow are copied into a Command (§4, I6).
- **D6** — a driver runs on an I/O/blocking lane, never on a scheduler worker inline (I1).

## 11. Open questions

- **Frame framing ownership** — where protocol framing (length-prefix, delimiters, vendor
  headers) sits: in the driver vs a dedicated framing Source node. Leaning driver for stream
  protocols (it needs framing to produce discrete frames at all), Source node for
  application-level sub-framing. Confirm with the first real TCP driver.
- **Camera / large-frame path** — whether v0.1 needs the `ZeroCopyRetained` DMA seam (024→019)
  or the arena-handle copy (§4) suffices for the target frame rates; hardware-dependent,
  benchmark before committing.
- **Multi-device drivers** — one driver managing many devices (e.g. one OPC UA server, many
  tags) vs one driver per device actor. Affects the single-producer model; likely one
  *gateway actor* fans discrete device streams. Resolve with 010's device→capability model.
- **Driver config schema** — connection strings, poll intervals, auth; align with Quark 013
  configuration and the device registry referenced in 010 §5 / 011 §7.
