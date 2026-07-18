// AeroEdge Firmware OTA — the per-device state machine + a mock OTA-capable driver (spec 011 §3, O1/O2/O4).
//
// UpdateFirmware drives a device through: fetch → VERIFY SIGNATURE → transfer → verify on-device hash →
// activate → health-check → COMMIT | ROLLBACK (011 §3). Safety invariants made concrete here:
//   * O1 — no unsigned/unverified image is ever transferred: verify_image() runs BEFORE the first
//     driver.write(); a tampered image (content changed) or a bad/absent signature is REJECTED, and the
//     runner returns before ANY byte reaches the device.
//   * O2 — every update has a rollback path: the rollback target (the currently-active version) is
//     recorded + persisted BEFORE activation; a failed health-check auto-invokes driver rollback.
//   * O4 — OTA progress is durable + resumable: each phase transition is Sync-checkpointed to a Quark
//     012 Store, so a crash mid-update resumes (resume_ota) rather than bricking the device.
// THIN-OVER-QUARK (R0): progress durability reuses aero::DurableState over the Quark 012 seam; the
// device write goes through the fenced IDriver::write() (006 §7 / 011 §5). REAL image signing is a
// Quark 020 secret/crypto concern (011 §6) and is GATED — verify_image() is a real integrity check (a
// keyed FNV hash over the content + a checked signature field), NOT production crypto (Ed25519 etc.).
#pragma once

#include <cstdint>
#include <string>

#include "aero/core/persistent_actor.hpp"   // aero::DurableState — Quark 012 durable progress (O4)
#include "aero/sdk/driver.hpp"              // IDriver / DeviceCommand — the fenced device-write seam
#include "quark/core/ids.hpp"
#include "quark/core/serialize.hpp"

namespace aero::ota {

// ---- Image + signature (011 §3) ------------------------------------------------------------------
struct OtaImage {
    std::string version;    // firmware version this image installs
    std::string bytes;      // firmware payload
    std::string signature;  // hex signature over the content, checked before transfer (O1)
};

// FNV-1a 64: the content hash used for both on-device integrity and signature derivation.
[[nodiscard]] inline std::uint64_t fnv1a64(const std::string& s) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

// A keyed hash "signature" over the image content. GATED stand-in for real asymmetric signing (011 §6,
// Quark 020): a production adapter verifies an Ed25519/RSA signature against a trust root. This keyed
// FNV is a REAL, checked integrity+authenticity gate (tamper the bytes or the key and it fails) without
// pulling a crypto lib offline (R4/R5) — enough to prove O1 end-to-end.
[[nodiscard]] inline std::string sign_image(const OtaImage& img, std::uint64_t trust_key) noexcept {
    const std::uint64_t sig = fnv1a64(std::to_string(trust_key) + ":" + std::to_string(fnv1a64(img.bytes)));
    return std::to_string(sig);
}

// Verify the signature against the trust root BEFORE any transfer (O1). Recomputes the content hash
// from the CURRENT bytes, so a tampered payload no longer matches its signature. A blank/wrong
// signature also fails. Returns false → the runner rejects the update and transfers nothing.
[[nodiscard]] inline bool verify_image(const OtaImage& img, std::uint64_t trust_key) noexcept {
    if (img.signature.empty()) return false;               // unsigned — O1
    return img.signature == sign_image(img, trust_key);    // tamper/forgery → mismatch — O1
}

// ---- Durable OTA progress (011 §3 idempotent+resumable, O4) --------------------------------------
enum class OtaPhase : std::uint8_t {
    Idle, Fetched, Verified, Transferred, DeviceVerified, Activated, HealthChecked,
    Committed, RolledBack, Rejected, Failed,
};

struct OtaProgress {
    std::uint8_t phase = 0;         // OtaPhase
    std::string image_version;      // the version being installed
    std::uint64_t image_hash = 0;   // expected on-device content hash
    std::string rollback_version;   // the version to roll back to (recorded BEFORE activation, O2)
};

// QUARK_SERIALIZE in the type's OWN namespace so the generated quark_describe is found by ADL from the
// Quark 012 snapshot path (the Described concept resolves it via the type's associated namespace).
QUARK_SERIALIZE(OtaProgress, (1, phase), (2, image_version), (3, image_hash), (4, rollback_version))

enum class OtaResult : std::uint8_t { Committed, RolledBack, Rejected, Failed };

struct OtaOutcome {
    OtaResult result = OtaResult::Failed;
    std::string final_version;      // the device's active version after the run
    std::string rollback_target;    // the version rolled back to (on RolledBack)
    OtaPhase reached = OtaPhase::Idle;
};

// ---- Mock OTA-capable driver (011 §3 A/B-slot device) --------------------------------------------
// An observable device with A/B slots: transfer stages the image into the INACTIVE slot (via the fenced
// write() path), activate switches slots, rollback switches back. A `force_health_fail` flag simulates a
// bad activation so the test can drive the auto-rollback path (O2). NOT a socket — fully deterministic.
class MockOtaDriver final : public aero::IDriver {
public:
    explicit MockOtaDriver(std::string initial_version) : active_(std::move(initial_version)) {}

    // The generic fenced device-write seam (006 §7). The OTA transfer streams the image THROUGH this so
    // the fenced write path is genuinely exercised (011 §5): each call appends one byte to the inactive
    // slot's staging buffer. Fencing/authorization happen at the owning EdgeActor before this is reached.
    aero::DriverStatus write(const aero::DeviceCommand& cmd) noexcept override {
        staging_.push_back(static_cast<char>(cmd.value & 0xFF));
        return aero::DriverStatus::Ok;
    }

    // --- OTA capability (011 §3 driver capability): the A/B-slot protocol over write() ---
    void ota_begin_transfer() noexcept { staging_.clear(); }
    void ota_finish_transfer(const std::string& version) {
        inactive_version_ = version;
        inactive_hash_ = fnv1a64(staging_);   // the device's own computed hash of what it received
    }
    [[nodiscard]] std::uint64_t device_staged_hash() const noexcept { return inactive_hash_; }

    void ota_activate() {                      // switch to the inactive slot (011 §3 A/B activate)
        rollback_to_ = active_;                // remember the slot we can fall back to
        active_ = inactive_version_;
    }
    void ota_rollback() { active_ = rollback_to_; }  // O2: fall back to the recorded slot

    [[nodiscard]] bool health_check() const noexcept { return !force_health_fail_; }
    void set_force_health_fail(bool v) noexcept { force_health_fail_ = v; }

    [[nodiscard]] const std::string& current_version() const noexcept { return active_; }

    aero::DriverStatus open(const aero::DriverConfig&) noexcept override { return aero::DriverStatus::Ok; }
    aero::DriverStatus run(aero::StreamSink<aero::Frame>, aero::StopToken) noexcept override {
        return aero::DriverStatus::Unsupported;  // OTA driver is write-only, not a frame producer
    }
    void close() noexcept override {}
    const aero::DriverDescriptor& descriptor() const noexcept override { return kDesc; }
    static constexpr aero::DriverDescriptor kDesc{"aero.driver.ota_mock", /*writable*/ true};

private:
    std::string active_;             // the currently-active firmware version
    std::string inactive_version_;   // the version staged in the inactive slot
    std::string rollback_to_;        // the slot to roll back to after an activate
    std::uint64_t inactive_hash_ = 0;
    std::string staging_;            // bytes received via write() for the inactive slot
    bool force_health_fail_ = false;
};

// ---- The per-device OTA state machine (011 §3) ---------------------------------------------------
// Drives one device through the full flow, Sync-checkpointing progress to `store` at each transition
// (O4). Verify runs BEFORE any transfer (O1); the rollback target is persisted BEFORE activation and a
// failed health-check auto-rolls-back (O2). Returns the outcome + the device's final version.
template <class Store>
OtaOutcome run_ota(MockOtaDriver& driver, const OtaImage& image, std::uint64_t trust_key,
                   Store& store, quark::ActorId id) {
    aero::DurableState<OtaProgress, Store> prog(store, id);
    prog.recover(OtaProgress{});
    auto set_phase = [&](OtaPhase p) noexcept {
        prog.state().phase = static_cast<std::uint8_t>(p);
        (void)prog.checkpoint();  // Sync durable (O4): the phase is on the store before we proceed
    };

    // 1. Fetch (image is provided by the caller / fleet).
    prog.state().image_version = image.version;
    set_phase(OtaPhase::Fetched);

    // 2. VERIFY SIGNATURE — before a single byte reaches the device (O1).
    if (!verify_image(image, trust_key)) {
        set_phase(OtaPhase::Rejected);
        return OtaOutcome{OtaResult::Rejected, driver.current_version(), "", OtaPhase::Rejected};
    }
    prog.state().image_hash = fnv1a64(image.bytes);
    set_phase(OtaPhase::Verified);

    // 3. Transfer to the device's inactive slot THROUGH the fenced write() path (011 §5).
    driver.ota_begin_transfer();
    for (const unsigned char b : image.bytes) {
        (void)driver.write(aero::DeviceCommand{"firmware", static_cast<std::int64_t>(b)});
    }
    driver.ota_finish_transfer(image.version);
    set_phase(OtaPhase::Transferred);

    // 4. Verify the on-device hash matches what we signed (catches transfer corruption). No activation
    //    has happened, so a mismatch just fails safe — the device stays on its current version.
    if (driver.device_staged_hash() != prog.state().image_hash) {
        set_phase(OtaPhase::Failed);
        return OtaOutcome{OtaResult::Failed, driver.current_version(), "", OtaPhase::Failed};
    }
    set_phase(OtaPhase::DeviceVerified);

    // 5. Activate — record the rollback target (the current active version) BEFORE the switch, and
    //    persist it, so a crash after activation still knows where to fall back (O2/O4).
    prog.state().rollback_version = driver.current_version();
    (void)prog.checkpoint();
    driver.ota_activate();
    set_phase(OtaPhase::Activated);

    // 6. Health-check gate → COMMIT or auto-ROLLBACK (O2).
    if (!driver.health_check()) {
        driver.ota_rollback();  // fall back to the recorded slot
        set_phase(OtaPhase::RolledBack);
        return OtaOutcome{OtaResult::RolledBack, driver.current_version(), prog.state().rollback_version,
                          OtaPhase::RolledBack};
    }
    set_phase(OtaPhase::HealthChecked);

    // 7. Commit.
    set_phase(OtaPhase::Committed);
    return OtaOutcome{OtaResult::Committed, driver.current_version(), "", OtaPhase::Committed};
}

// Resume an interrupted OTA (O4): if a crash left the device Activated but not yet Committed, re-run the
// health-check gate (commit | rollback) instead of restarting the whole transfer. Demonstrates that the
// durable progress makes the update resumable and never leaves the device half-updated with no rollback
// target. Other mid-phases (pre-activation) safely restart from the top via run_ota.
template <class Store>
OtaOutcome resume_ota(MockOtaDriver& driver, std::uint64_t /*trust_key*/, Store& store,
                      quark::ActorId id) {
    aero::DurableState<OtaProgress, Store> prog(store, id);
    prog.recover(OtaProgress{});
    if (static_cast<OtaPhase>(prog.state().phase) != OtaPhase::Activated) {
        return OtaOutcome{OtaResult::Failed, driver.current_version(), prog.state().rollback_version,
                          static_cast<OtaPhase>(prog.state().phase)};
    }
    auto set_phase = [&](OtaPhase p) noexcept {
        prog.state().phase = static_cast<std::uint8_t>(p);
        (void)prog.checkpoint();
    };
    if (!driver.health_check()) {
        driver.ota_rollback();
        set_phase(OtaPhase::RolledBack);
        return OtaOutcome{OtaResult::RolledBack, driver.current_version(), prog.state().rollback_version,
                          OtaPhase::RolledBack};
    }
    set_phase(OtaPhase::Committed);
    return OtaOutcome{OtaResult::Committed, driver.current_version(), "", OtaPhase::Committed};
}

}  // namespace aero::ota
