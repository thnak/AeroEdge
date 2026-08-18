// AeroEdge Phase-8 follow-up gate (spec 010 §10): `Runtime::configure_fleet()`'s optional REAL
// multi-node membership (`FleetConfig::membership`) over a genuine `quark::SwimMembership` (021) + real
// loopback TCP — proving `/fleet` reflects ACTUAL liveness, not the config-time fiction the single-node
// stand-in reports. Mirrors tests/runtime/runtime_fleet_wiring.cpp's own "reachable through the real
// REST API, not just standalone-testable" posture, extended to two real Runtime instances.
//
// Proves, over real loopback sockets:
//   (1) two nodes configured with each other as a mutual SWIM seed converge to BOTH reporting 2 alive
//       nodes (bounded polling — SWIM's own join + gossip cadence, not instantaneous).
//   (2) once converged, `real_membership` is true and device placement reflects the LIVE node set (a
//       device requiring node B's flag is placed once B is visible, not stuck at the config-time
//       "requires 2 nodes wide, only 1 known so far" state).
//   (3) killing node B (its Runtime destroyed — the real socket goes away, no clean SWIM leave) is
//       eventually detected by node A's SWIM failure detector: `/fleet` on A converges back down to 1
//       alive node (its own), NOT a crash/hang, and NOT a false "still 2" forever.
// Deterministic-enough, exit-code-gated (0 = pass); bounded polling; real (not virtual) SWIM clock —
// this test genuinely waits on real wall-clock ack/suspicion timeouts, so it is not instant.
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "aero/api/rest_api.hpp"
#include "aero/runtime/runtime.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace {

constexpr std::uint16_t kPortA = 48720;  // fixed, not ephemeral — see file banner: both sides seed
constexpr std::uint16_t kPortB = 48721;  // each other mutually at configure_fleet() time

bool wait_ready(httplib::Client& cli) {
    for (int i = 0; i < 300; ++i) {
        auto h = cli.Get("/health");
        if (h && h->status == 200) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

nlohmann::json fleet_json(httplib::Client& cli) {
    auto r = cli.Get("/fleet");
    if (!r || r->status != 200) return nlohmann::json{};
    auto j = nlohmann::json::parse(r->body, nullptr, false);
    return j.is_discarded() ? nlohmann::json{} : j;
}

int alive_count(httplib::Client& cli) {
    const auto j = fleet_json(cli);
    if (!j.value("configured", false)) return -1;
    return static_cast<int>(j.value("nodes", nlohmann::json::array()).size());
}

// Bounded poll until `pred(alive_count(cli))` is true, or `timeout_ms` elapses.
bool wait_until(httplib::Client& cli, int timeout_ms, bool (*pred)(int)) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred(alive_count(cli))) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

}  // namespace

int main() {
    bool ok = true;

    aero::runtime::Runtime rtA;
    auto rtB = std::make_unique<aero::runtime::Runtime>();

    httplib::Server svrA;
    auto svrB = std::make_unique<httplib::Server>();
    aero::api::RestApi apiA(rtA);
    auto apiB = std::make_unique<aero::api::RestApi>(*rtB);
    apiA.install(svrA);
    apiB->install(*svrB);

    const int restPortA = svrA.bind_to_any_port("127.0.0.1");
    const int restPortB = svrB->bind_to_any_port("127.0.0.1");
    if (restPortA <= 0 || restPortB <= 0) {
        std::printf("REST bind failed\nFAIL\n");
        return 1;
    }
    std::thread threadA([&svrA] { svrA.listen_after_bind(); });
    std::thread threadB([&svrB] { svrB->listen_after_bind(); });

    httplib::Client cliA("127.0.0.1", restPortA);
    httplib::Client cliB("127.0.0.1", restPortB);
    cliA.set_connection_timeout(2, 0);
    cliA.set_read_timeout(5, 0);
    cliB.set_connection_timeout(2, 0);
    cliB.set_read_timeout(5, 0);
    if (!wait_ready(cliA) || !wait_ready(cliB)) {
        std::printf("REST server(s) not ready\nFAIL\n");
        svrA.stop();
        svrB->stop();
        threadA.join();
        threadB.join();
        return 1;
    }

    // ----- configure_fleet(): mutual SWIM seeding, one device each requiring the OTHER node's flag ---
    aero::runtime::Runtime::FleetConfig cfgA;
    cfgA.node_flags = {"line-1"};
    cfgA.membership.self = quark::NodeId{101};
    cfgA.membership.listen_port = kPortA;
    cfgA.membership.cluster_id = 42;
    cfgA.membership.tick_interval_ms = 50;
    cfgA.membership.seeds = {{quark::NodeId{102}, "127.0.0.1", kPortB, {"line-3"}}};
    cfgA.devices = {{"d-needs-b", "1.0", {"line-3"}, {}}};  // only eligible once B is visible

    aero::runtime::Runtime::FleetConfig cfgB;
    cfgB.node_flags = {"line-3"};
    cfgB.membership.self = quark::NodeId{102};
    cfgB.membership.listen_port = kPortB;
    cfgB.membership.cluster_id = 42;
    cfgB.membership.tick_interval_ms = 50;
    cfgB.membership.seeds = {{quark::NodeId{101}, "127.0.0.1", kPortA, {"line-1"}}};

    auto rA = rtA.configure_fleet(cfgA);
    auto rB = rtB->configure_fleet(cfgB);
    ok &= rA.has_value() && rB.has_value();
    std::printf("[cfg] A=%s B=%s\n", rA ? "ok" : rA.error().c_str(), rB ? "ok" : rB.error().c_str());

    // ----- (1)+(2): converge to 2 alive nodes on both sides, device placed once B is visible ---------
    const bool converged_a = wait_until(cliA, 10000, [](int n) { return n == 2; });
    const bool converged_b = wait_until(cliB, 10000, [](int n) { return n == 2; });
    ok &= converged_a && converged_b;
    std::printf("[converge] A_sees_2=%s B_sees_2=%s\n", converged_a ? "yes" : "NO",
                converged_b ? "yes" : "NO");

    const auto fj = fleet_json(cliA);
    const bool real_membership = fj.value("real_membership", false);
    bool device_placed = false;
    for (const auto& d : fj.value("devices", nlohmann::json::array())) {
        if (d.value("id", std::string{}) == "d-needs-b") device_placed = d.value("eligible", false);
    }
    ok &= real_membership && device_placed;
    std::printf("[live_placement] real_membership=%s device_placed=%s\n",
                real_membership ? "yes" : "NO", device_placed ? "yes" : "NO");

    // ----- (3): kill B (its Runtime destroyed — no clean SWIM leave) -> A's failure detector converges
    // back down to 1 alive node (itself only) within the real ack+suspicion timeout window ------------
    svrB->stop();
    threadB.join();
    apiB.reset();
    rtB.reset();  // ~Runtime() stops cluster_transport_ — B's socket is genuinely gone, not just idle

    const bool detected_dead = wait_until(cliA, 15000, [](int n) { return n == 1; });
    ok &= detected_dead;
    std::printf("[failure_detection] A_converges_to_1=%s\n", detected_dead ? "yes" : "NO");

    svrA.stop();
    threadA.join();

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
