// AeroEdge Phase-10 LOAD-BEARING gate #2 (spec 011 §3/§4, O1/O2/O4/O5): Firmware OTA.
//
//   ACT 1 — a GOOD signed image drives fetch→verify→transfer→verify→activate→health→COMMIT: the device
//           version advances 1.0 → 2.0 (happy path, canary commit).
//   ACT 2 — a forced HEALTH-CHECK FAILURE after activation auto-ROLLS BACK: the device stays on 1.0 and
//           the rollback target is recorded (O2 — every update has a rollback, a failed health check
//           triggers it).
//   ACT 3 — an UNSIGNED and a TAMPERED image are REJECTED before ANY byte is transferred (O1 — no
//           unverified image reaches the device): the device stays on 1.0 and received 0 bytes.
//   ACT 4 — O4 resume: a crash left the device Activated-but-not-Committed; resume_ota finishes the
//           health gate from durable progress instead of restarting (no half-updated brick).
//   ACT 5 — FLEET rollout: a good image commits every wave (Completed); a failing canary AUTO-PAUSES
//           the rollout so later waves' devices keep their firmware (O5).
//
// Exit code 0 = OK; prints "FAIL" on any mismatch. No socket, deterministic.
#include <cstdint>
#include <cstdio>
#include <string>

#include "aero/ota/fleet.hpp"
#include "aero/ota/ota.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"

using namespace aero::ota;
using quark::ActorId;
using quark::InMemoryStore;
using quark::TypeKey;

// TEST-ONLY EC P-256 keypair (never used in production — same convention as every other checked-in
// *_TEST_CERTS_DIR material in this tree). Generated with:
//   openssl ecparam -name prime256v1 -genkey -noout -out signing_key.pem
//   openssl ec -in signing_key.pem -pubout -out public_key.pem
static const std::string kSigningKeyPem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIOI8Hgmq+AGZP7mjSfqsxVBRsXuC/ffSUOzWnaSYOj9coAoGCCqGSM49\n"
    "AwEHoUQDQgAEQVB6J9oo1Z+/PPaYwJuwXSUmrZv5+U21d34+EsXvO9IyOx0sTSqv\n"
    "XyET1vSUqgc71FfeYkXkVbum6q9pUDzoMg==\n"
    "-----END EC PRIVATE KEY-----\n";
static const std::string kTrustRootPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEQVB6J9oo1Z+/PPaYwJuwXSUmrZv5\n"
    "+U21d34+EsXvO9IyOx0sTSqvXyET1vSUqgc71FfeYkXkVbum6q9pUDzoMg==\n"
    "-----END PUBLIC KEY-----\n";

static OtaImage good_image() {
    OtaImage img{"2.0", "firmware-payload-v2", ""};
    img.signature = sign_image(img, kSigningKeyPem);  // real ECDSA-P256/SHA-256 signature
    return img;
}

int main() {
    bool ok = true;

    // ===== ACT 1 — good image → COMMIT, version advances ==========================================
    {
        InMemoryStore store;
        MockOtaDriver dev("1.0");
        const auto oc = run_ota(dev, good_image(), kTrustRootPem, store, ActorId{TypeKey{0x07A1}, 1});
        const bool pass = oc.result == OtaResult::Committed && oc.final_version == "2.0" &&
                          dev.current_version() == "2.0" && oc.reached == OtaPhase::Committed;
        ok &= pass;
        std::printf("[act1] good image: result=Committed version 1.0->%s %s\n",
                    dev.current_version().c_str(), pass ? "ok" : "FAIL");
    }

    // ===== ACT 2 — health-check FAILURE → auto-ROLLBACK ===========================================
    {
        InMemoryStore store;
        MockOtaDriver dev("1.0");
        dev.set_force_health_fail(true);  // activation "succeeds" but the device fails its health probe
        const auto oc = run_ota(dev, good_image(), kTrustRootPem, store, ActorId{TypeKey{0x07A2}, 1});
        const bool pass = oc.result == OtaResult::RolledBack && dev.current_version() == "1.0" &&
                          oc.rollback_target == "1.0" && oc.reached == OtaPhase::RolledBack;
        ok &= pass;
        std::printf("[act2] health-fail: result=RolledBack version stays %s, rollback_target=%s %s\n",
                    dev.current_version().c_str(), oc.rollback_target.c_str(), pass ? "ok" : "FAIL");
    }

    // ===== ACT 3 — unsigned + tampered images REJECTED before transfer (O1) =======================
    {
        InMemoryStore store;
        MockOtaDriver dev("1.0");
        OtaImage unsigned_img{"2.0", "firmware-payload-v2", ""};  // no signature
        const auto oc = run_ota(dev, unsigned_img, kTrustRootPem, store, ActorId{TypeKey{0x07A3}, 1});
        const bool pass = oc.result == OtaResult::Rejected && dev.current_version() == "1.0" &&
                          dev.device_staged_hash() == 0 && oc.reached == OtaPhase::Rejected;
        ok &= pass;
        std::printf("[act3a] unsigned: result=Rejected, version stays %s, 0 bytes transferred %s\n",
                    dev.current_version().c_str(), pass ? "ok" : "FAIL");
    }
    {
        InMemoryStore store;
        MockOtaDriver dev("1.0");
        OtaImage img = good_image();
        img.bytes = "TAMPERED-payload";  // content changed but signature kept → verify must fail
        const auto oc = run_ota(dev, img, kTrustRootPem, store, ActorId{TypeKey{0x07A4}, 1});
        const bool pass = oc.result == OtaResult::Rejected && dev.current_version() == "1.0" &&
                          dev.device_staged_hash() == 0;
        ok &= pass;
        std::printf("[act3b] tampered: result=Rejected, version stays %s, 0 bytes transferred %s\n",
                    dev.current_version().c_str(), pass ? "ok" : "FAIL");
    }

    // ===== ACT 4 — O4 resume: Activated-but-not-Committed → resume finishes the gate ==============
    {
        InMemoryStore store;
        const ActorId id{TypeKey{0x07A5}, 1};
        MockOtaDriver dev("1.0");
        // Drive the state machine up to Activated by hand, persisting progress as run_ota would, then
        // "crash" (drop everything) and resume against the SAME durable store.
        {
            aero::DurableState<OtaProgress, InMemoryStore> prog(store, id);
            prog.recover(OtaProgress{});
            prog.state().image_version = "2.0";
            prog.state().rollback_version = "1.0";
            prog.state().phase = static_cast<std::uint8_t>(OtaPhase::Activated);
            (void)prog.checkpoint();
        }
        dev.ota_finish_transfer("2.0");
        dev.ota_activate();  // device is now on 2.0, mid-update, awaiting the health gate
        const auto oc = resume_ota(dev, kTrustRootPem, store, id);
        const bool pass = oc.result == OtaResult::Committed && dev.current_version() == "2.0";
        ok &= pass;
        std::printf("[act4] resume Activated->Committed, version=%s %s\n", dev.current_version().c_str(),
                    pass ? "ok" : "FAIL");
    }

    // ===== ACT 5 — FLEET rollout: full success vs failing-canary auto-pause (O5) ===================
    {
        // Scenario A — good image commits every wave → Completed, all devices updated.
        MockOtaDriver d0("1.0"), d1("1.0"), d2("1.0"), d3("1.0"), d4("1.0");
        FleetActor fleet(/*threshold*/ 1.0, /*canary*/ 1, /*staged*/ 2, /*rate_limit*/ 2);
        fleet.add_device("d0", d0); fleet.add_device("d1", d1); fleet.add_device("d2", d2);
        fleet.add_device("d3", d3); fleet.add_device("d4", d4);
        const auto waves = fleet.run(good_image(), kTrustRootPem);
        const bool pass = fleet.state() == RolloutState::Completed && fleet.devices_updated() == 5 &&
                          d0.current_version() == "2.0" && d4.current_version() == "2.0" &&
                          waves.size() == 3;
        ok &= pass;
        std::printf("[act5a] good rollout: state=Completed updated=%zu waves=%zu %s\n",
                    fleet.devices_updated(), waves.size(), pass ? "ok" : "FAIL");
    }
    {
        // Scenario B — the canary device fails its health check → the wave misses the threshold and the
        // rollout AUTO-PAUSES; the staged/full devices are never touched (keep their firmware) (O5).
        MockOtaDriver d0("1.0"), d1("1.0"), d2("1.0"), d3("1.0"), d4("1.0");
        d0.set_force_health_fail(true);  // canary fails
        FleetActor fleet(/*threshold*/ 1.0, /*canary*/ 1, /*staged*/ 2, /*rate_limit*/ 2);
        fleet.add_device("d0", d0); fleet.add_device("d1", d1); fleet.add_device("d2", d2);
        fleet.add_device("d3", d3); fleet.add_device("d4", d4);
        const auto waves = fleet.run(good_image(), kTrustRootPem);
        const bool pass = fleet.state() == RolloutState::Paused && fleet.devices_updated() == 0 &&
                          fleet.devices_rolled_back() == 1 && waves.size() == 1 && !waves[0].passed &&
                          d0.current_version() == "1.0" && d1.current_version() == "1.0" &&
                          d4.current_version() == "1.0";
        ok &= pass;
        std::printf("[act5b] canary-fail: state=Paused updated=%zu rolled_back=%zu later-devices-untouched=%d %s\n",
                    fleet.devices_updated(), fleet.devices_rolled_back(),
                    (d1.current_version() == "1.0" && d4.current_version() == "1.0") ? 1 : 0,
                    pass ? "ok" : "FAIL");
    }

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
