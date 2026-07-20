// AeroEdge api — the management/control REST surface (spec 013 §2/§5/§9, 009, 016).
//
// A THIN shell over Runtime (013 T2: the Studio and CLI reach the runtime ONLY through this API; all
// logic lives in Runtime, so it is testable without HTTP). Decided in 013 §9: REST+JSON for
// request/response, SSE for live streams. Endpoints:
//   GET    /health           → readiness probe (used by clients to wait-for-ready)
//   POST   /apps             → body = Application JSON → deploy → 200 status / 4xx error
//   GET    /status           → Runtime.status()
//   GET    /apps             → deployed Applications
//   DELETE /apps/{name}      → undeploy
//   GET    /metrics/stream   → SSE stream of status snapshots (live metrics)
//   GET    /mes/outbox       → Runtime.mes_outbox_stats() (016 §2.3 — real, ungated)
//   POST   /mes/outbox/drain → nudge a stuck outbox drain (M3)
//   GET    /fleet            → Runtime.fleet_status() (016 §2.1 — real, single-daemon scope)
//   GET    /ota/rollouts     → Runtime.ota_status() (016 §2.2 — real orchestration, mock drivers)
//   POST   /ota/rollouts     → body = {"version","bytes"} → Runtime.start_rollout()
//
// httplib (cpp-httplib) is confined to aero-api/aero-cli (R1): it never enters aero-core/aero-sdk.
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>

#include "aero/runtime/runtime.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace aero::api {

class RestApi {
public:
    explicit RestApi(runtime::Runtime& rt) noexcept : rt_(rt) {}

    // Register all routes on a server the caller owns (so the caller controls bind/listen/stop).
    void install(httplib::Server& svr) {
        svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("ok", "text/plain");
        });

        svr.Post("/apps", [this](const httplib::Request& req, httplib::Response& res) {
            auto r = rt_.deploy_json(req.body);
            if (!r) {
                res.status = 400;
                res.set_content(error_json(r.error()), "application/json");
                return;
            }
            res.status = 200;
            res.set_content(rt_.status().dump(), "application/json");
        });

        svr.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(rt_.status().dump(), "application/json");
        });

        svr.Get("/apps", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(rt_.list().dump(), "application/json");
        });

        // Hot-reload the running Application (009 §4): body = new Application JSON. A Live change
        // hot-swaps in place; a BuildOnly change is a clean 400 ("requires redeploy"), never applied.
        // The {name} path segment must match the running app (enforced by reload's classifier).
        svr.Put(R"(/apps/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            auto r = rt_.reload_json(req.body);
            if (!r) {
                res.status = 400;
                res.set_content(error_json(r.error()), "application/json");
                return;
            }
            res.status = 200;
            res.set_content(rt_.status().dump(), "application/json");
        });

        // Rollback to the previous Application version (009 §6): a hot-reload back to the prior flow.
        svr.Post(R"(/apps/([^/]+)/rollback)", [this](const httplib::Request&, httplib::Response& res) {
            auto r = rt_.rollback();
            if (!r) {
                res.status = 400;
                res.set_content(error_json(r.error()), "application/json");
                return;
            }
            res.status = 200;
            res.set_content(rt_.status().dump(), "application/json");
        });

        svr.Delete(R"(/apps/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            const std::string name = req.matches[1];
            auto r = rt_.undeploy(name);
            if (!r) {
                res.status = 404;
                res.set_content(error_json(r.error()), "application/json");
                return;
            }
            nlohmann::json ok;
            ok["undeployed"] = name;
            res.set_content(ok.dump(), "application/json");
        });

        // Live metrics (013 §9): SSE via a chunked content provider. Emits the current status snapshot
        // as an `data: {json}` event, paced, until the client disconnects or a bounded cap is reached.
        // The pacing sleep runs on httplib's own request thread (not any verification foreground).
        svr.Get("/metrics/stream", [this](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider(
                "text/event-stream", [this](std::size_t /*offset*/, httplib::DataSink& sink) {
                    constexpr int kMaxSnapshots = 3600;  // bounded so the provider always terminates
                    for (int i = 0; i < kMaxSnapshots; ++i) {
                        const std::string ev = "data: " + rt_.status().dump() + "\n\n";
                        if (!sink.write(ev.data(), ev.size())) {
                            return false;  // client disconnected
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    sink.done();
                    return true;
                });
        });
        // MES outbox observability (016 §2.3): {"configured": false} until the daemon has been given
        // an MES endpoint to gateway to (see aero_runtime_main.cpp --mes-host/--mes-port).
        svr.Get("/mes/outbox", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(rt_.mes_outbox_stats().dump(), "application/json");
        });

        svr.Post("/mes/outbox/drain", [this](const httplib::Request&, httplib::Response& res) {
            auto r = rt_.mes_drain();
            if (!r) {
                res.status = 400;
                res.set_content(error_json(r.error()), "application/json");
                return;
            }
            res.status = 200;
            res.set_content(rt_.mes_outbox_stats().dump(), "application/json");
        });

        // Fleet/placement observability (016 §2.1): {"configured": false} until configure_fleet().
        svr.Get("/fleet", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(rt_.fleet_status().dump(), "application/json");
        });

        // OTA rollout observability + control (016 §2.2). GET is idempotent status; POST starts a new
        // wave-by-wave rollout (011 §4) against the registered drivers — body carries ONLY the image
        // (version + payload bytes), never a trust key (012 M5 posture, 016 §2.2).
        svr.Get("/ota/rollouts", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(rt_.ota_status().dump(), "application/json");
        });

        svr.Post("/ota/rollouts", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.contains("version")) {
                res.status = 400;
                res.set_content(error_json("body must be JSON with a 'version' field"), "application/json");
                return;
            }
            auto r = rt_.start_rollout(body.value("version", std::string{}), body.value("bytes", std::string{}));
            if (!r) {
                res.status = 400;
                res.set_content(error_json(r.error()), "application/json");
                return;
            }
            res.status = 200;
            res.set_content(rt_.ota_status().dump(), "application/json");
        });
    }

private:
    static std::string error_json(const std::string& msg) {
        nlohmann::json e;
        e["error"] = msg;
        return e.dump();
    }

    runtime::Runtime& rt_;
};

}  // namespace aero::api
