// AeroEdge Phase-11.1 gate (spec 016 §2.3, 012 §3): the MES gateway wired into Runtime + reachable
// over the real REST API — proves configure_mes()/mes_stage()/mes_drain()/mes_outbox_stats() are not
// just standalone-testable (that's mes_outbox.cpp) but actually reachable through the SAME Runtime a
// deployed Application lives in, and through the SAME RestApi a browser/Studio would hit.
//
//   ACT 0 — before configure_mes(), GET /mes/outbox reports {"configured": false} (never a crash/500).
//   ACT 1 — configure_mes() against a local mock MES (initially down) + a report staged via
//           Runtime::mes_stage(): GET /mes/outbox shows it RETAINED (staged=1, pending=1) — the outage
//           delays, never drops (M3), exactly like mes_outbox.cpp's ACT1 but reached over HTTP.
//   ACT 2 — MES comes up, POST /mes/outbox/drain nudges the drain: GET /mes/outbox shows delivered=1,
//           pending=0.
//   ACT 3 — a second configure_mes() call is rejected (400) — the gateway is a daemon-lifetime
//           singleton, not silently reconfigurable underneath an in-flight outbox.
//
// Exit code 0 = OK; prints "FAIL" on any mismatch.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "aero/api/rest_api.hpp"
#include "aero/mes/mes.hpp"
#include "aero/runtime/runtime.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

// A controllable mock MES: 503 when "down", 200 + dedup-by-idempotency-key when "up" (mirrors
// tests/mes_outbox.cpp's MockMes so the two tests assert the same adapter behavior).
struct MockMes {
    std::atomic<bool> up{false};
    std::mutex mu;
    std::set<std::string> unique_keys;

    void install(httplib::Server& svr) {
        svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("ok", "text/plain");
        });
        svr.Post("/production", [this](const httplib::Request& req, httplib::Response& res) {
            if (!up.load(std::memory_order_acquire)) {
                res.status = 503;
                return;
            }
            std::lock_guard<std::mutex> g(mu);
            unique_keys.insert(req.get_header_value("Idempotency-Key"));
            res.status = 200;
        });
    }
    std::size_t unique_count() {
        std::lock_guard<std::mutex> g(mu);
        return unique_keys.size();
    }
};

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

    // ----- Local mock MES on an ephemeral port (initially DOWN) -------------------------------------
    MockMes mes;
    httplib::Server mes_svr;
    mes.install(mes_svr);
    const int mes_port = mes_svr.bind_to_any_port("127.0.0.1");
    if (mes_port <= 0) {
        std::printf("mes bind failed\nFAIL\n");
        return 1;
    }
    std::thread mes_thread([&mes_svr] { mes_svr.listen_after_bind(); });
    {
        httplib::Client probe("127.0.0.1", mes_port);
        probe.set_connection_timeout(2, 0);
        if (!wait_ready(probe)) {
            std::printf("mock MES not ready\nFAIL\n");
            mes_svr.stop();
            mes_thread.join();
            return 1;
        }
    }

    // ----- Runtime + RestApi on a second ephemeral port (the daemon under test) ---------------------
    aero::runtime::Runtime rt;
    httplib::Server api_svr;
    aero::api::RestApi api(rt);
    api.install(api_svr);
    const int api_port = api_svr.bind_to_any_port("127.0.0.1");
    if (api_port <= 0) {
        std::printf("api bind failed\nFAIL\n");
        mes_svr.stop();
        mes_thread.join();
        return 1;
    }
    std::thread api_thread([&api_svr] { api_svr.listen_after_bind(); });

    httplib::Client cli("127.0.0.1", api_port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    if (!wait_ready(cli)) {
        std::printf("api server not ready\nFAIL\n");
        api_svr.stop();
        api_thread.join();
        mes_svr.stop();
        mes_thread.join();
        return 1;
    }

    // ----- ACT 0: unconfigured — never a crash, always an honest {"configured": false} --------------
    {
        auto r = cli.Get("/mes/outbox");
        const bool got = r && r->status == 200;
        auto j = got ? nlohmann::json::parse(r->body, nullptr, false) : nlohmann::json{};
        const bool unconfigured = got && !j.is_discarded() && j.value("configured", true) == false;
        ok &= unconfigured;
        std::printf("[act0] unconfigured GET /mes/outbox -> configured=false %s\n",
                    unconfigured ? "ok" : "FAIL");
    }

    // ----- configure_mes() wires the gateway to the mock MES (still DOWN) ---------------------------
    aero::mes::MesConfig cfg;
    cfg.endpoint = "127.0.0.1";
    cfg.port = mes_port;
    cfg.report_path = "/production";
    auto cfg_r = rt.configure_mes(cfg);
    ok &= cfg_r.has_value();
    std::printf("[cfg] configure_mes -> %s\n", cfg_r.has_value() ? "ok" : cfg_r.error().c_str());

    // ----- ACT 1: MES down, stage a report -> retained (staged=1, pending=1, delivered=0) -----------
    aero::mes::MesReport report{aero::mes::MesReport::Kind::Production, "line-7", "produced", 42.0, 0, ""};
    auto stage_r = rt.mes_stage(report);
    ok &= stage_r.has_value();
    {
        auto r = cli.Get("/mes/outbox");
        auto j = r && r->status == 200 ? nlohmann::json::parse(r->body, nullptr, false) : nlohmann::json{};
        const bool retained = !j.is_discarded() && j.value("configured", false) &&
                              j.value("staged", 0) == 1 && j.value("pending", 0) == 1 &&
                              j.value("delivered", 0) == 0;
        ok &= retained;
        std::printf("[act1] MES down: %s %s\n", j.dump().c_str(), retained ? "ok" : "FAIL");
    }

    // ----- ACT 2: MES up, POST /mes/outbox/drain -> delivered=1, pending=0 --------------------------
    mes.up.store(true, std::memory_order_release);
    {
        auto r = cli.Post("/mes/outbox/drain", "", "application/json");
        ok &= (r && r->status == 200);
        // The drain runs async on the gateway's own actor lane; poll briefly for it to land.
        bool drained = false;
        nlohmann::json j;
        for (int i = 0; i < 300 && !drained; ++i) {
            auto g = cli.Get("/mes/outbox");
            j = g && g->status == 200 ? nlohmann::json::parse(g->body, nullptr, false) : nlohmann::json{};
            drained = !j.is_discarded() && j.value("pending", 1) == 0 && j.value("delivered", 0) == 1;
            if (!drained) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ok &= drained && mes.unique_count() == 1;
        std::printf("[act2] MES up, drained: %s mes_unique=%zu %s\n", j.dump().c_str(), mes.unique_count(),
                    (drained && mes.unique_count() == 1) ? "ok" : "FAIL");
    }

    // ----- ACT 3: a second configure_mes() is rejected, not silently accepted -----------------------
    {
        auto again = rt.configure_mes(cfg);
        const bool rejected = !again.has_value();
        ok &= rejected;
        std::printf("[act3] second configure_mes rejected: %s\n", rejected ? "ok" : "FAIL");
    }

    api_svr.stop();
    api_thread.join();
    mes_svr.stop();
    mes_thread.join();

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
