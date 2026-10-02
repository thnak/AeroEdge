# 028 — Device Protocol v4 and Device Commands

> Draft v0.1. The AeroEdge-native protocol for ESP32-class devices, a generic **device command**
> channel with acknowledgement (for every device type), and the **request/response bridge**
> operator terminals use.
>
> Fleet decision: the existing AeroOEE ESP32 fleet is **re-flashed** to protocol v4. The legacy
> payload generations (CEDGE v1/v2/v3, heartbeat v1) aren't carried into AeroEdge.

## 1. Scope

| Part | Applies to |
|---|---|
| §2–4 Protocol v4 (topics, connection, payloads) | AeroEdge-native MQTT devices (ESP32 counters, IO boxes, HMI terminals) |
| §5 Device commands | **all** device types. For PLCs the codec maps commands to register/node writes |
| §6 Request/response bridge | terminals and devices that need answers from upstream (job plan, defect codes) |
| §7 OTA over v4 | v4 devices. Implements the 011 driver side |
| §8 Firmware conformance | anyone building v4 firmware |

## 2. Topics

All under the device namespace of 023 §4.2 (`aero/{site}/{device_id}/…`). Client-id =
`device_id` (023 IA6).

| Direction | Topic suffix | Content |
|---|---|---|
| up | `up/hello` | on every boot: `boot_id`, firmware, model, capabilities, restored counters |
| up | `up/hb` | heartbeat (§4.3) |
| up | `up/di/{input}` | counter report (§4.2) |
| up | `up/ai/{input}` | analog value (on change / deadband) |
| up | `up/log/{level}` | device log line |
| up | `up/cmdres` | command result (§5) |
| up | `up/req/{name}` | request (§6) |
| up | `up/ota` | OTA progress / result (§7) |
| up | `up/lwt` | last will: `offline` |
| down | `down/cmd` | command (§5) |
| down | `down/res/{name}` | response to a request (§6) |
| down | `down/ota` | OTA instruction (§7) |

## 3. Connection behavior

- TLS required. MQTT 5 preferred (response topic / correlation data, §6), with MQTT 3.1.1
  accepted.
- Keep-alive 30 s. The LWT is set to `up/lwt` = `offline`.
- Reconnect with **jittered exponential backoff** (1 s → 60 s, ±30% jitter), so a node restart
  doesn't cause a synchronized reconnect storm (023 §2).
- Device offline buffering: reports produced while disconnected are queued in RAM (bounded) and
  sent on reconnect with their original `seq`. Counters make overflow harmless: the next report
  carries the current cumulative value.

## 4. Payloads

JSON, compact keys, UTF-8. A CBOR encoding with the same schema is optional (§10). Every `up`
message carries:

| Field | Meaning |
|---|---|
| `b` | `boot_id`: random 64-bit value generated at boot |
| `s` | `seq`: monotonic per boot, across all `up` messages |
| `t` | device time (UTC ms), **only** if SNTP-synced. Otherwise omitted (025 §3.3) |

### 4.1 Hello

`{b, s, fw, model, caps:[…], counters:{"<input>":{"c":<value>,"e":<counter_epoch>}}}`. Sent
after every (re)connect that follows a boot.

### 4.2 Counter report

`up/di/{input}`: `{b, s, t?, c, e, n, cy:{min,avg,max,last}}`

| Field | Meaning |
|---|---|
| `c` | cumulative counter, **persisted on the device** (NVS, write-amortized) |
| `e` | `counter_epoch`: incremented **only** when the counter genuinely resets (operator `counter.reset`, configured daily reset, factory reset). A reboot with a restored counter keeps the same epoch |
| `n` | pulses since the previous report |
| `cy` | cycle time (ms) statistics over those `n` pulses (024 §4.2) |

- Reported on change, batched (default ≤ 1 s or every 10 pulses).
- AeroEdge's `counter_delta` (024 §4.1) treats a change of `e` as a reset (new baseline, no
  delta). A reboot with an unchanged `e` and a counter at or above the last value continues
  normally, so **no counts are lost across a reboot**.
- The NVS write interval bounds the loss on power failure. The firmware declares it in `caps`,
  and AeroEdge reports it in health.

### 4.3 Heartbeat

`up/hb`: `{b, s, t?, up, rssi, heap, fw, rr}`, where `up` = uptime in seconds and `rr` = reboot
reason (on the first heartbeat after boot). The interval is configurable by command (default
30 s). Only models declaring the `heartbeat` capability are judged by it (022 §5).

## 5. Device commands

A generic, acknowledged command channel. It replaces AeroOEE's `cmd/{id}/*` endpoints.

```text
caller (Studio / upstream service) ── SendDeviceCommand ──▶ node gateway ──▶ EdgeActor ──▶ codec ──▶ device
device ── up/cmdres {id, status, detail} ──▶ EdgeActor ──▶ CommandResult event (025 §3.2) + audit
```

- `SendDeviceCommand{command_id, device_id, name, args, timeout}`. `command_id` is
  caller-generated and is the idempotency key. The device remembers its last 16 ids and
  re-acknowledges duplicates without executing them again.
- Status flow: `accepted` → `done` | `failed` | `rejected`. If nothing arrives within `timeout`,
  the result is `timeout`.
- Every command is **audited** (who, what, result) and **authorized per command name** for the
  calling user or service (023).

Standard command set (a model declares which it supports):

| Command | Args | Notes |
|---|---|---|
| `reboot` | | |
| `identify` | `seconds` | blink LED / beep to find the unit |
| `config.get` / `config.set` | `heartbeat_s`, `report_ms`, `log_level`, `counter_reset_hour`, … | `config.set` result echoes applied values |
| `counter.reset` | `input` | increments `counter_epoch`. Audited, and emits `CountGap{reason: manual_reset}` if a window was open |
| `calibrate` | `input`, two-point values | 4–20 mA / analog calibration |
| `tare` | `input` | scales |
| `wifi.reset` | | returns the device to its setup mode |
| `params.write` | name/value list | e.g. PLC program number (PTN) per product. For PLCs the codec maps it to register/node writes |

Commands are addressed by **device**. Machine-level outputs remain addressed by machine + role
(024 §6). A device command never bypasses 024's output authorization for bound output points.

## 6. Request/response bridge

Operator terminals and some devices ask upstream for data: the current job plan, defect code
lists, downtime reasons. AeroOEE serves 14 such `edge/*` request/response topics today. When the
broker moves to AeroEdge, they need a path.

```text
terminal ── up/req/{name} (MQTT 5 response topic + correlation data) ──▶ broker (AeroEdge)
  ──▶ UpstreamGateway ── IUpstreamAdapter::request(name, payload, principal) ──▶ AeroOEE / AeroMes
  ◀── response ── down/res/{name} (same correlation data) ◀──
```

- Each request `name` is registered to exactly **one** upstream adapter (e.g. `job.plan` →
  AeroMes, `defect.codes` → AeroOEE).
- **Authorization:** the device model declares which request names it may use. The adapter
  receives the calling **principal** so the upstream can scope its answer (which station, which
  line).
- Limits: timeout (default 5 s → error response), max payload size, per-device rate limit.
- AeroEdge doesn't cache or interpret request/response content. It's a routed pipe. The business
  logic stays upstream.
- Requests aren't queued in the outbox. If the upstream is unreachable, the device gets an
  immediate `unavailable` response.

## 7. OTA over v4

Implements the device side of 011.

- `down/ota` `{version, url, sha256, sig, size}`. The `url` points at the control-plane firmware
  catalog (022 §2), with HTTP range download for resumable transfer on weak Wi-Fi.
- The device **verifies the signature** against the trust root compiled into the firmware
  before activating. It uses an A/B partition with rollback on failed health check (011 §3).
- Progress and result go to `up/ota` (`downloading %`, `verifying`, `activating`, `done` /
  `failed:<reason>`). These feed the 011 state machine and `FirmwareUpdated`/`FirmwareFailed`.
- Unsigned URL-pull OTA (AeroOEE `cmd/{id}/ota`) is not supported.

## 8. Firmware conformance (v4)

A v4 device MUST:

1. Use client-id = `device_id`, TLS, and either a per-unit claim key in secure storage (023
   §3.1) or an imported per-device credential (023 §2.1).
2. Send `boot_id` + `seq` on every `up` message, and `hello` on boot.
3. Persist counters in NVS with `counter_epoch` semantics (§4.2), and declare the NVS write
   interval.
4. Set the LWT. Use jittered reconnect backoff.
5. Send heartbeats with the §4.3 fields if it declares `heartbeat`.
6. Omit device time unless SNTP-synced.
7. Acknowledge commands with idempotent handling of `command_id`.
8. Support signed A/B OTA with rollback.
9. Have **no hard-coded secrets**. The setup access point password is per unit (derived from the
   label claim key). The broker credential is in encrypted storage, not plaintext NVS.

## 9. Re-flash migration

Re-flashing is done station by station, per line:

1. **Flash station**: write v4 firmware and generate and burn the per-unit claim key. Print the
   label QR. Register `{device_id, claim key hash}` with the control plane in bulk.
2. If the unit had a **unique** AeroOEE credential, import it (023 §2.1) instead of claiming.
3. Install. The device connects, claims (or authenticates with the imported credential) and
   shows up `Provisioned`. Apply the machine's binding template (024 §2.3).
4. Verify with the live I/O tap (016) and run the line with `test` bindings for one shift before
   switching to production.

## 10. Invariants (normative)

- **DV1**: every v4 `up` message carries `boot_id` and `seq`. AeroEdge deduplicates on them.
- **DV2**: counters are cumulative and device-persisted. Only a `counter_epoch` change resets a
  baseline.
- **DV3**: every device command has a caller `command_id`, an acknowledgement or timeout result,
  an audit record, and per-name authorization.
- **DV4**: the request/response bridge routes each request name to exactly one upstream adapter
  and passes the caller principal. AeroEdge holds no request business logic.
- **DV5**: firmware images are signed and verified on the device before activation.

## 11. Implementation status

Not started. Needs: the v4 codec in `aero-drivers`, the `EdgeActor` command path, the
`IUpstreamAdapter::request` extension (025), and reference ESP32 firmware (a separate firmware
repo).

## 12. Open questions

- JSON only, or also CBOR for constrained links?
- Catalog of request names: map AeroOEE's 14 `edge/*` topics one-to-one, or consolidate?
- NVS write interval default: trade flash wear against the power-loss count window (every 10
  pulses / 5 s?).
