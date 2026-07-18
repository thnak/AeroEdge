// AeroEdge api — the management/control REST surface (spec 013 §2/§5/§9, 009).
//
// A THIN shell over Runtime (013 T2: the Studio and CLI reach the runtime ONLY through this API; all
// logic lives in Runtime, so it is testable without HTTP). Decided in 013 §9: REST+JSON for
// request/response, SSE for live streams. Endpoints:
//   GET    /health          → readiness probe (used by clients to wait-for-ready)
//   POST   /apps            → body = Application JSON → deploy → 200 status / 4xx error
//   GET    /status          → Runtime.status()
//   GET    /apps            → deployed Applications
//   DELETE /apps/{name}     → undeploy
//   GET    /metrics/stream  → SSE stream of status snapshots (live metrics)
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
