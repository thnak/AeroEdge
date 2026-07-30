// AeroEdge runtime daemon (spec 013 §2 aero-runtime).
//
// Boots a Runtime + the REST/SSE management API. Optionally deploys an Application from `--app
// path.json` at startup; otherwise it waits for a `POST /apps`. Optionally wires the MES gateway
// (016 §2.3) from `--mes-host`/`--mes-port` — omit them and `/mes/outbox` just reports
// `{"configured": false}`. Optionally wires the Fleet/OTA/placement view (016 §2.1/§2.2) from
// repeated `--fleet-device ID[:VERSION]` flags — omit them and `/fleet`/`/ota/rollouts` report
// `{"configured": false}`. Optionally wires the native MQTT broker / southbound termination (017)
// from `--broker-port`/`--broker-bind` — omit them and `/broker/status` reports `{"configured": false}`.
// Optionally wires the broker's TLS listener (M5) from `--broker-tls-cert`/`--broker-tls-key` (both
// required together; `--broker-tls-ca` is optional, enables mTLS) — omit them and the broker stays
// plaintext-only, exactly as before M5. NOTE (intentional v1 gap): Config::authenticate/authorizer
// (aero/broker/acl.hpp) have no CLI/config-file story yet — C++ API only for now.
// This is the deployable edge-node binary — one per node. All control logic lives in Runtime/RestApi;
// main is just wiring + args.
//
//   aero-runtime [--app path.json] [--host 0.0.0.0] [--port 8080] [--mes-host H --mes-port P]
//                [--fleet-device ID[:VERSION] ...] [--fleet-node-flag FLAG ...]
//                [--broker-port P] [--broker-bind H]
//                [--broker-tls-cert PATH --broker-tls-key PATH [--broker-tls-ca PATH]]
//                [--broker-tls-port P]
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "aero/api/rest_api.hpp"
#include "aero/broker/native_broker.hpp"
#include "aero/mes/mes.hpp"
#include "aero/pal/tls.hpp"
#include "aero/runtime/runtime.hpp"
#include "httplib.h"

namespace {
// "id" or "id:version" (default version "1.0") — kept as plain colon-splitting, not a JSON config file
// (016 §6 defers the richer device-registry-source question; this is the minimal honest CLI answer).
aero::runtime::Runtime::FleetDeviceConfig parse_fleet_device(const std::string& spec) {
    const auto colon = spec.find(':');
    aero::runtime::Runtime::FleetDeviceConfig d;
    d.id = colon == std::string::npos ? spec : spec.substr(0, colon);
    d.initial_version = colon == std::string::npos ? "1.0" : spec.substr(colon + 1);
    return d;
}
}  // namespace

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
    aero::runtime::Runtime::FleetConfig fleet_cfg;
    aero::broker::Config broker_cfg;
    bool broker_enabled = false;  // only true if a --broker-* flag was actually given (017)
    std::string broker_tls_cert, broker_tls_key, broker_tls_ca;      // M5
    std::uint16_t broker_tls_port = 8883;                            // M5

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
        } else if (a == "--fleet-device" && i + 1 < argc) {
            fleet_cfg.devices.push_back(parse_fleet_device(argv[++i]));
        } else if (a == "--fleet-node-flag" && i + 1 < argc) {
            fleet_cfg.node_flags.push_back(argv[++i]);
        } else if (a == "--broker-port" && i + 1 < argc) {
            broker_cfg.listen_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
            broker_enabled = true;
        } else if (a == "--broker-bind" && i + 1 < argc) {
            broker_cfg.bind_host = argv[++i];
            broker_enabled = true;
        } else if (a == "--broker-tls-cert" && i + 1 < argc) {
            broker_tls_cert = argv[++i];
        } else if (a == "--broker-tls-key" && i + 1 < argc) {
            broker_tls_key = argv[++i];
        } else if (a == "--broker-tls-ca" && i + 1 < argc) {
            broker_tls_ca = argv[++i];
        } else if (a == "--broker-tls-port" && i + 1 < argc) {
            broker_tls_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else {
            std::fprintf(stderr,
                         "usage: aero-runtime [--app path.json] [--host H] [--port P] "
                         "[--mes-host H --mes-port P [--mes-path PATH] [--mes-token TOKEN]] "
                         "[--fleet-device ID[:VERSION] ...] [--fleet-node-flag FLAG ...] "
                         "[--broker-port P] [--broker-bind H] "
                         "[--broker-tls-cert PATH --broker-tls-key PATH [--broker-tls-ca PATH]] "
                         "[--broker-tls-port P]\n");
            return 2;
        }
    }

    // --broker-tls-cert/--broker-tls-key are required TOGETHER to enable TLS (matches the existing
    // flag-validation posture above: a clear stderr message + non-zero exit, not a silent half-config).
    // --broker-tls-ca is optional (mTLS). Config::authenticate/authorizer have no CLI story yet (v1 gap,
    // noted in this file's banner) — only the TLS material is wireable from the command line today.
    if (broker_tls_cert.empty() != broker_tls_key.empty()) {
        std::fprintf(stderr,
                     "usage: --broker-tls-cert and --broker-tls-key must both be given together "
                     "(only one was provided)\n");
        return 2;
    }
    if (!broker_tls_cert.empty()) {
        aero::pal::tls::ServerConfig tls_cfg;
        tls_cfg.cert_file = broker_tls_cert;
        tls_cfg.key_file = broker_tls_key;
        tls_cfg.ca_file = broker_tls_ca;
        broker_cfg.tls = std::move(tls_cfg);
        broker_cfg.tls_port = broker_tls_port;
        broker_enabled = true;
    }

    aero::runtime::Runtime rt;

    if (!fleet_cfg.devices.empty() || !fleet_cfg.node_flags.empty()) {
        auto r = rt.configure_fleet(fleet_cfg);
        if (!r) {
            std::fprintf(stderr, "fleet configure failed: %s\n", r.error().c_str());
            return 3;
        }
        std::printf("fleet configured: %zu device(s)\n", fleet_cfg.devices.size());
    }

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

    if (broker_enabled) {
        auto r = rt.configure_broker(broker_cfg);
        if (!r) {
            std::fprintf(stderr, "native broker configure failed: %s\n", r.error().c_str());
            return 3;
        }
        std::printf("native broker listening on %s:%d\n", broker_cfg.bind_host.c_str(),
                    static_cast<int>(broker_cfg.listen_port));
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
