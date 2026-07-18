# 011 — Edge Firmware OTA

> Draft v0.1. Over-the-air firmware update for the edge devices AeroEdge manages. This is
> AeroEdge-owned functionality (Quark provides the actor/transport substrate; the update
> protocol and safety model are ours). Distinct from *flow* hot-reload (009) — this updates
> the **device firmware**, not the AeroEdge software.

## 1. Scope and the two update surfaces

Do not conflate the two:

| Surface | What updates | Owner | Spec |
|---|---|---|---|
| **Flow / node reload** | the AeroEdge processing graph on a running actor | AeroEdge + Quark live-reconfig | 009 |
| **Firmware OTA** | binary firmware *on the physical device* | **this spec** | 011 |

This spec is Firmware OTA: pushing a signed firmware image to a device managed by an
`EdgeActor`, verifying it, and rolling forward/back safely.

## 2. Actors and messages

OTA is modeled as Commands/Events on the existing actor model (002) — no special runtime:

```text
Operator/MES ──DeployFirmware(image_ref, target)──▶ FleetActor
FleetActor   ──UpdateFirmware(image_ref)──▶ EdgeActor(device)     (fan-out, staged)
EdgeActor    ──(drives OTA state machine over its driver)──▶ Device
EdgeActor    ──FirmwareUpdated / FirmwareFailed (Event)──▶ FleetActor ──▶ MES (012)
```

- **`FleetActor`** — orchestrates a rollout across many devices: staging/canary, rate
  limiting (Quark 022 governance), pause/abort. One per fleet or per line.
- **`EdgeActor`** — runs the per-device OTA state machine as a Flow triggered by
  `UpdateFirmware`. The device-protocol details live in the **driver** (006); the flow
  orchestrates the steps.

## 3. Per-device OTA state machine (a Flow)

`UpdateFirmware` triggers an OTA Flow whose nodes drive the device through:

```text
Fetch image ─▶ Verify signature ─▶ Transfer to device ─▶ Verify on-device hash
   ─▶ Activate/reboot ─▶ Health-check ─▶ Commit  |  Rollback
```

Rules:

- **Signed images only.** The image is signed; the Verify node checks the signature against
  a trust root before any bytes reach the device. Key management uses Quark's secrets/at-rest
  facilities (020) — AeroEdge does not invent a keystore.
- **A/B or staged where the device supports it.** If the device has A/B slots, activate the
  inactive slot and switch on successful health-check; otherwise transfer→verify→reboot with
  a recorded rollback image. The strategy is a **driver capability**, not a flow assumption.
- **Health-check gate.** After activation the flow health-checks (device reports version +
  liveness). Failure → **automatic rollback**; success → **commit** and emit
  `FirmwareUpdated`.
- **Idempotent + resumable.** OTA progress (current phase, image hash) is persisted via
  Quark 012 so a node crash mid-update resumes rather than restarts. Because `EdgeActor`
  migration is fenced (010 §3), no two nodes ever drive the same device's OTA at once.

## 4. Fleet rollout orchestration (`FleetActor`)

- **Canary → staged → full.** Rollout advances in waves; a wave commits only if the prior
  wave's success rate clears a threshold. A failing wave **auto-pauses** the rollout.
- **Rate limiting & bandwidth.** Concurrent transfers are bounded by Quark 022 token buckets
  so OTA never saturates a plant network or the edge nodes' egress.
- **Observability.** Per-device phase, success/failure, and version are surfaced through
  Quark 009 metrics/tracing; a rollout is one traceable operation end-to-end.
- **Abort/resume.** `PauseRollout` / `ResumeRollout` / `AbortRollout` Commands; abort stops
  new devices but lets in-flight devices finish or roll back cleanly.

## 5. Safety invariants (normative)

- **O1** — no unsigned or unverified image is ever transferred to a device.
- **O2** — every update has a defined rollback path; a failed health-check triggers it
  automatically.
- **O3** — OTA is fenced: exactly one controller (the owning `EdgeActor`) drives a device's
  update at any time (010 §3).
- **O4** — OTA progress is durable and resumable; a crash never bricks a device by leaving
  it half-updated with no recorded rollback target.
- **O5** — rollouts are rate-limited and staged; a bad image cannot brick a whole fleet
  before the canary gate catches it.

## 6. What AeroEdge builds vs reuses

| Concern | Owner |
|---|---|
| OTA orchestration state machine (Flow + FleetActor) | **AeroEdge** |
| Device-side transfer/activate protocol | **AeroEdge driver** (006), per device family |
| Image signing/verification keys, secrets | Quark 020 (security, secrets, at-rest) |
| Durable OTA progress | Quark 012 (persistence) |
| Rate limiting / bandwidth control | Quark 022 (governance) |
| Fenced single-controller guarantee | Quark 021 (fenced hand-off) |
| Metrics/tracing of a rollout | Quark 009 (observability) |

## 7. Open questions

- **Device capability model** — how a driver advertises `supports=ab-slots`, max image
  size, transfer protocol; ties to the device→capability registry (010 §5).
- **Delta updates** — whether to support binary-delta images to cut transfer size; a driver
  capability + a Transform node, deferred.
- **Trust root distribution** — how signing keys/roots reach edge nodes and rotate; align
  with Quark 020/021 bootstrap.
- **MES-initiated vs operator-initiated rollouts** — the `DeployFirmware` entry point may
  originate from MES (012); authorization model TBD with 012.
