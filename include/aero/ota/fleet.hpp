// AeroEdge Firmware OTA — the FleetActor staged rollout orchestrator (spec 011 §4, O5).
//
// Orchestrates an image rollout across many devices in WAVES: canary → staged → full. A wave advances
// ONLY if its success rate clears a threshold; a failing wave AUTO-PAUSES the rollout so a bad image
// cannot brick a whole fleet before the canary gate catches it (O5). Concurrent transfers are bounded
// by a rate limit (Quark 022 token-bucket governance in a full runtime — here a batch cap, since the
// deterministic test runs sequentially). ADDS ONLY POLICY on top of the per-device OTA state machine
// (ota.hpp) + Quark — NO scheduler, NO transport (R0), mirroring aero-cluster's placement-policy shape.
//
// In a full runtime FleetActor is a Quark actor that fans `UpdateFirmware` out to each device's
// EdgeActor and consumes FirmwareUpdated/FirmwareFailed events (011 §2); Phase-10 implements the
// wave/gate/pause POLICY as a driveable class so it is a deterministic unit under ota_rollout.cpp.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "aero/ota/ota.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"   // quark::InMemoryStore for per-device OTA progress

namespace aero::ota {

enum class RolloutState : std::uint8_t { Idle, Running, Paused, Completed };

// One device under fleet management: its id + its OTA-capable driver (observable version).
struct FleetDevice {
    std::string id;
    MockOtaDriver* driver = nullptr;
};

struct WaveResult {
    std::string name;
    std::size_t attempted = 0;
    std::size_t succeeded = 0;
    double success_rate = 0.0;
    bool passed = false;   // cleared the threshold
};

class FleetActor {
public:
    // threshold: min wave success rate to advance (e.g. 1.0 = every canary must succeed).
    // canary/staged: device counts for the first two waves; the rest form the "full" wave.
    // rate_limit: max concurrent transfers per wave batch (Quark 022 bandwidth cap, 011 §4).
    FleetActor(double threshold, std::size_t canary, std::size_t staged, std::size_t rate_limit)
        : threshold_(threshold), canary_(canary), staged_(staged),
          rate_limit_(rate_limit == 0 ? 1 : rate_limit) {}

    void add_device(std::string id, MockOtaDriver& driver) {
        devices_.push_back(FleetDevice{std::move(id), &driver});
    }

    // Run the rollout wave-by-wave (011 §4). Stops + PAUSES at the first wave that misses the threshold
    // (O5) — later waves are NOT started, so their devices keep their current firmware. A fully-passing
    // rollout ends Completed. Returns the per-wave results for observability (011 §4).
    std::vector<WaveResult> run(const OtaImage& image, std::uint64_t trust_key) {
        state_ = RolloutState::Running;
        updated_ = 0;
        rolled_back_ = 0;
        std::vector<WaveResult> results;

        std::size_t offset = 0;
        const std::size_t n = devices_.size();
        struct Wave { const char* name; std::size_t count; };
        const Wave waves[3] = {{"canary", canary_},
                               {"staged", staged_},
                               {"full", n > (canary_ + staged_) ? n - (canary_ + staged_) : 0}};

        for (const auto& w : waves) {
            if (offset >= n) break;
            const std::size_t count = std::min(w.count, n - offset);
            if (count == 0) continue;
            WaveResult wr = run_wave(w.name, offset, count, image, trust_key);
            results.push_back(wr);
            offset += count;
            if (!wr.passed) {
                state_ = RolloutState::Paused;   // O5: auto-pause; do not advance to the next wave
                return results;
            }
        }
        state_ = RolloutState::Completed;
        return results;
    }

    [[nodiscard]] RolloutState state() const noexcept { return state_; }
    [[nodiscard]] std::size_t devices_updated() const noexcept { return updated_; }
    [[nodiscard]] std::size_t devices_rolled_back() const noexcept { return rolled_back_; }
    [[nodiscard]] std::size_t rate_limit() const noexcept { return rate_limit_; }

private:
    WaveResult run_wave(const char* name, std::size_t offset, std::size_t count, const OtaImage& image,
                        std::uint64_t trust_key) {
        WaveResult wr;
        wr.name = name;
        wr.attempted = count;
        // Rate limit (011 §4): process the wave in batches of at most rate_limit_ concurrent transfers.
        for (std::size_t done = 0; done < count; done += rate_limit_) {
            const std::size_t batch = std::min(rate_limit_, count - done);
            for (std::size_t k = 0; k < batch; ++k) {
                FleetDevice& d = devices_[offset + done + k];
                // Each device gets its own durable OTA progress keyed by index (fenced, single-driver).
                const quark::ActorId id{quark::TypeKey{0x0DA0}, static_cast<std::uint32_t>(offset + done + k)};
                const OtaOutcome oc = run_ota(*d.driver, image, trust_key, store_, id);
                if (oc.result == OtaResult::Committed) { ++wr.succeeded; ++updated_; }
                else if (oc.result == OtaResult::RolledBack) { ++rolled_back_; }
            }
        }
        wr.success_rate = count == 0 ? 0.0 : static_cast<double>(wr.succeeded) / static_cast<double>(count);
        wr.passed = wr.success_rate >= threshold_;
        return wr;
    }

    double threshold_;
    std::size_t canary_;
    std::size_t staged_;
    std::size_t rate_limit_;
    std::vector<FleetDevice> devices_;
    quark::InMemoryStore store_;   // per-device durable OTA progress (011 §3 O4)
    RolloutState state_ = RolloutState::Idle;
    std::size_t updated_ = 0;
    std::size_t rolled_back_ = 0;
};

}  // namespace aero::ota
