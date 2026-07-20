// AeroEdge runtime daemon (spec 013 §2 aero-runtime).
//
// Boots a Runtime + the REST/SSE management API. Optionally deploys an Application from `--app
// path.json` at startup; otherwise it waits for a `POST /apps`. Optionally wires the MES gateway
// (016 §2.3) from `--mes-host`/`--mes-port` — omit them and `/mes/outbox` just reports
// `{"configured": false}`. This is the deployable edge-node binary — one per node. All control logic
// lives in Runtime/RestApi; main is just wiring + args.
//
//   aero-runtime [--app path.json] [--host 0.0.0.0] [--port 8080] [--mes-host H --mes-port P]
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "aero/api/rest_api.hpp"
#include "aero/mes/mes.hpp"
#include "aero/runtime/runtime.hpp"
#include "httplib.h"

namespace {
httplib::Server* g_server = nullptr;
void on_signal(int) {
    if (g_server) g_server->stop();
}
}  // namespace

int main(int argc, char** argv) {
    std::string app_path;
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string mes_host;
    int mes_port = 0;
    std::string mes_path = "/production";
    std::string mes_token;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--app" && i + 1 < argc) {
            app_path = argv[++i];
        } else if (a == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--mes-host" && i + 1 < argc) {
            mes_host = argv[++i];
        } else if (a == "--mes-port" && i + 1 < argc) {
            mes_port = std::atoi(argv[++i]);
        } else if (a == "--mes-path" && i + 1 < argc) {
            mes_path = argv[++i];
        } else if (a == "--mes-token" && i + 1 < argc) {
            mes_token = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: aero-runtime [--app path.json] [--host H] [--port P] "
                         "[--mes-host H --mes-port P [--mes-path PATH] [--mes-token TOKEN]]\n");
            return 2;
        }
    }

    aero::runtime::Runtime rt;

    if (!mes_host.empty()) {
        aero::mes::MesConfig mes_cfg;
        mes_cfg.endpoint = mes_host;
        mes_cfg.port = mes_port;
        mes_cfg.report_path = mes_path;
        mes_cfg.token = mes_token;
        auto r = rt.configure_mes(mes_cfg);
        if (!r) {
            std::fprintf(stderr, "MES gateway configure failed: %s\n", r.error().c_str());
            return 3;
        }
        std::printf("MES gateway wired to %s:%d%s\n", mes_host.c_str(), mes_port, mes_path.c_str());
    }

    if (!app_path.empty()) {
        std::ifstream f(app_path);
        if (!f) {
            std::fprintf(stderr, "cannot open '%s'\n", app_path.c_str());
            return 2;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        auto r = rt.deploy_json(ss.str());
        if (!r) {
            std::fprintf(stderr, "deploy failed: %s\n", r.error().c_str());
            return 3;
        }
        std::printf("deployed '%s' from %s\n", rt.status().value("name", std::string{}).c_str(),
                    app_path.c_str());
    }

    httplib::Server svr;
    aero::api::RestApi api(rt);
    api.install(svr);

    g_server = &svr;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("aero-runtime listening on %s:%d\n", host.c_str(), port);
    std::fflush(stdout);
    if (!svr.listen(host, port)) {
        std::fprintf(stderr, "failed to listen on %s:%d\n", host.c_str(), port);
        return 4;
    }
    return 0;
}
