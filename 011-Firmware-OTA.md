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
| Image signing/verification (asymmetric crypto) | **AeroEdge** (`aero/pal/crypto.hpp`, over the same vendored mbedTLS as TLS — Quark 020 hasn't productionized a primitive yet, same precedent as `aero/pal/tls.hpp`) |
| Trust-root key custody, rotation, distribution | Quark 020 (security, secrets, at-rest) — still open, §7 |
| Durable OTA progress | Quark 012 (persistence) |
| Rate limiting / bandwidth control | Quark 022 (governance) |
| Fenced single-controller guarantee | Quark 021 (fenced hand-off) |
| Metrics/tracing of a rollout | Quark 009 (observability) |

## 7. Implementation status (updated as work ships)

- **Real image signing — shipped.** O1's "signed images only" is now enforced with REAL asymmetric
  crypto: `aero::ota::sign_image()`/`verify_image()` (`include/aero/ota/ota.hpp`) call
  `aero::pal::crypto::ecdsa_sign_sha256()`/`ecdsa_verify_sha256()` (`include/aero/pal/crypto.hpp`) — ECDSA
  P-256 over SHA-256, via mbedTLS's `pk` layer, the SAME vendored mbedTLS this project already links for
  TLS (`aero/pal/tls.hpp`) — not a second crypto library. This replaces the earlier keyed-FNV-1a hash
  stand-in. The signed content is `version || bytes` (the claimed version is bound INTO the signature, so
  a validly-signed payload can't be replayed under a different claimed version — a small hardening beyond
  the FNV placeholder's scope). `FleetConfig::ota_signing_key_pem`/`ota_trust_root_public_key_pem`
  (`include/aero/runtime/runtime.hpp`) carry the keypair — PEM, EC P-256 — defaulting to checked-in
  TEST-ONLY material (same convention as every other `*_TEST_CERTS_DIR` in this tree) that a real
  deployment overrides. **Still open**: key custody/rotation/distribution itself (§7 below) — this ships
  the cryptographic PRIMITIVE, not a key-management system.
- **Device-side transfer/activate protocol — still `MockOtaDriver`, not real.** §3's A/B-slot state
  machine and §5's safety invariants are proven against a fully-deterministic in-memory mock device
  (`aero::ota::MockOtaDriver`), not a real firmware-capable driver talking to physical hardware — there is
  no specific device family in scope to build one against yet (§6, "per device family").

## 8. Open questions

- **Device capability model** — how a driver advertises `supports=ab-slots`, max image
  size, transfer protocol; ties to the device→capability registry (010 §5).
- **Delta updates** — whether to support binary-delta images to cut transfer size; a driver
  capability + a Transform node, deferred.
- **Trust root key custody, rotation, and distribution** — §6/§7's crypto PRIMITIVE is real
  (ECDSA P-256/SHA-256); how the signing key is generated/held offline and how a fleet's public
  trust root reaches/rotates on edge nodes is still open — align with Quark 020/021 bootstrap.
- **MES-initiated vs operator-initiated rollouts** — the `DeployFirmware` entry point may
  originate from MES (012); authorization model TBD with 012.
