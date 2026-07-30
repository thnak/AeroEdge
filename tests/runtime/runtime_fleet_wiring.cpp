// AeroEdge Phase-11.2 gate (spec 016 §2.1/§2.2, 010/011): Cluster+Fleet(OTA) wired into Runtime and
// reachable over the real REST API — proves configure_fleet()/fleet_status()/ota_status()/
// start_rollout() are not just standalone-testable (that's cluster_place.cpp/ota_rollout.cpp) but
// actually reachable through the SAME Runtime a deployed Application lives in.
//
//   ACT 0 — before configure_fleet(), GET /fleet and GET /ota/rollouts both report
//           {"configured": false} (never a crash/500); POST /ota/rollouts is a clean 400.
//   ACT 1 — configure_fleet() with 3 devices, no capability constraints (vacuously eligible on the
//           single local node): GET /fleet shows all 3 placed on node 1.
//   ACT 2 — POST /ota/rollouts with a version -> GET /ota/rollouts shows state=Completed, all 3
//           devices updated, 1 wave (canary=1/staged=1/rate_limit=5 folds the rest into "full").
//
// Exit code 0 = OK; prints "FAIL" on any mismatch.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "aero/api/rest_api.hpp"
#include "aero/runtime/runtime.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

static bool wait_ready(httplib::Client& cli) {
    for (int i = 0; i < 300; ++i) {
        auto h = cli.Get("/health");
        if (h && h->status == 200) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int main() {
    bool ok = true;

    aero::runtime::Runtime rt;
    httplib::Server svr;
    aero::api::RestApi api(rt);
    api.install(svr);
    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("bind failed\nFAIL\n");
        return 1;
    }
    std::thread server_thread([&svr] { svr.listen_after_bind(); });

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    if (!wait_ready(cli)) {
        std::printf("server not ready\nFAIL\n");
        svr.stop();
        server_thread.join();
        return 1;
    }

    // ----- ACT 0: unconfigured — honest {"configured": false}, POST is a clean 400 ------------------
    {
        auto fj = cli.Get("/fleet");
        auto oj = cli.Get("/ota/rollouts");
        auto f = fj ? nlohmann::json::parse(fj->body, nullptr, false) : nlohmann::json{};
        auto o = oj ? nlohmann::json::parse(oj->body, nullptr, false) : nlohmann::json{};
        const bool unconfigured = fj && fj->status == 200 && !f.is_discarded() &&
                                  f.value("configured", true) == false && oj && oj->status == 200 &&
                                  !o.is_discarded() && o.value("configured", true) == false;
        ok &= unconfigured;

        auto post = cli.Post("/ota/rollouts", R"({"version":"2.0","bytes":"x"})", "application/json");
        const bool clean_400 = post && post->status == 400;
        ok &= clean_400;
        std::printf("[act0] unconfigured GET x2 -> configured=false, POST -> 400: %s\n",
                    (unconfigured && clean_400) ? "ok" : "FAIL");
    }

    // ----- configure_fleet(): 3 devices, no capability constraints ----------------------------------
    aero::runtime::Runtime::FleetConfig cfg;
    cfg.ota_threshold = 1.0;
    cfg.ota_canary = 1;
    cfg.ota_staged = 1;
    cfg.ota_rate_limit = 5;
    cfg.devices = {
        {"d0", "1.0", {}, {}},
        {"d1", "1.0", {}, {}},
        {"d2", "1.0", {}, {}},
    };
    auto cfg_r = rt.configure_fleet(cfg);
    ok &= cfg_r.has_value();
    std::printf("[cfg] configure_fleet -> %s\n", cfg_r.has_value() ? "ok" : cfg_r.error().c_str());

    // ----- ACT 1: GET /fleet — all 3 devices eligible + placed on node 1 ----------------------------
    {
        auto r = cli.Get("/fleet");
        auto j = r && r->status == 200 ? nlohmann::json::parse(r->body, nullptr, false) : nlohmann::json{};
        bool all_placed = !j.is_discarded() && j.value("configured", false) &&
                          j.value("devices", nlohmann::json::array()).size() == 3;
        if (all_placed) {
            for (const auto& d : j["devices"]) {
                all_placed &= d.value("eligible", false) && d.value("node", 0) == 1;
            }
        }
        ok &= all_placed;
        std::printf("[act1] GET /fleet: %s %s\n", j.dump().c_str(), all_placed ? "ok" : "FAIL");
    }

    // ----- ACT 2: POST /ota/rollouts -> Completed, 3 devices updated, 3 waves (canary=1/staged=1/
    // full=1 — the remaining device count, not folded together just because rate_limit=5) ------------
    {
        auto post = cli.Post("/ota/rollouts", R"({"version":"2.0","bytes":"firmware-payload-v2"})",
                             "application/json");
        ok &= (post && post->status == 200);

        auto r = cli.Get("/ota/rollouts");
        auto j = r && r->status == 200 ? nlohmann::json::parse(r->body, nullptr, false) : nlohmann::json{};
        const bool completed = !j.is_discarded() && j.value("configured", false) &&
                               j.value("state", std::string{}) == "Completed" &&
                               j.value("devices_updated", 0) == 3 && j.value("devices_rolled_back", 0) == 0 &&
                               j.value("waves", nlohmann::json::array()).size() == 3;
        ok &= completed;
        std::printf("[act2] rollout: %s %s\n", j.dump().c_str(), completed ? "ok" : "FAIL");
    }

    svr.stop();
    server_thread.join();

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
