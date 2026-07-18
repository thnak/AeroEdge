// AeroEdge Phase-4 gate — the management API over a real socket (spec 013 §5/§9).
//
// Starts the RestApi on an EPHEMERAL port on a background thread, then uses httplib::Client to POST an
// Application and GET /status, asserting the deployed app is reported — then shuts the server down
// cleanly. Robustness: wait-for-ready by POLLING GET /health with bounded retries (no fixed foreground
// wait for the server to come up); the server is stopped and joined at the end so the run is clean
// under the sanitizers. The in-process runtime_deploy test is the load-bearing one; this proves the
// thin HTTP shell wires through to the same Runtime. Exit code 0 = OK.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "aero/api/rest_api.hpp"
#include "aero/runtime/runtime.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

int main() {
    aero::runtime::Runtime rt;
    httplib::Server svr;
    aero::api::RestApi api(rt);
    api.install(svr);

    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("bind_to_any_port failed\nFAIL\n");
        return 1;
    }
    std::thread server_thread([&svr] { svr.listen_after_bind(); });

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);

    // Wait-for-ready: poll /health with bounded retries (≤ 300 × 10ms = 3s cap).
    bool ready = false;
    for (int i = 0; i < 300 && !ready; ++i) {
        auto h = cli.Get("/health");
        if (h && h->status == 200) {
            ready = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!ready) {
        std::printf("server did not become ready\nFAIL\n");
        svr.stop();
        server_thread.join();
        return 1;
    }

    const char* app = R"({
      "name": "hello_flow",
      "version": "0.1.0",
      "actor": { "kind": "edge", "key": 7 },
      "flow": [
        { "type_id": "aero.source.decode" },
        { "type_id": "aero.transform.scale", "config": { "factor": 2 } },
        { "type_id": "aero.output.sum" }
      ],
      "driver": { "type_id": "aero.driver.generator", "config": { "frame_count": 100 } }
    })";

    auto post = cli.Post("/apps", app, "application/json");
    const bool post_ok = post && post->status == 200;

    auto stat = cli.Get("/status");
    const bool stat_ok = stat && stat->status == 200;

    std::string name;
    bool deployed = false;
    if (stat_ok) {
        auto j = nlohmann::json::parse(stat->body, nullptr, false);
        if (!j.is_discarded()) {
            name = j.value("name", std::string{});
            deployed = j.value("deployed", false);
        }
    }

    // A malformed deploy must be a clean 400, not a crash.
    auto bad = cli.Post("/apps", "{ not json", "application/json");
    const bool bad_ok = bad && bad->status == 400;

    // Shut down BEFORE tearing down the runtime (ordered, clean under the sanitizers).
    svr.stop();
    server_thread.join();

    std::printf("ready=%d post=%d status=%d deployed=%d name=%s bad_deploy_400=%d\n", ready ? 1 : 0,
                post_ok ? 1 : 0, stat_ok ? 1 : 0, deployed ? 1 : 0, name.c_str(), bad_ok ? 1 : 0);

    const bool pass = post_ok && stat_ok && deployed && name == "hello_flow" && bad_ok;
    std::printf("%s\n", pass ? "OK" : "FAIL");
    return pass ? 0 : 1;
}
