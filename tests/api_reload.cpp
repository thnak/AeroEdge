// AeroEdge Phase-5 gate — hot-reload + rollback over the REST surface (spec 009 §4/§6, 013 §5).
//
// Proves the thin HTTP shell wires PUT /apps/{name} (reload) and POST /apps/{name}/rollback through to
// the same Runtime that the in-process hot_reload test exercises. Robustness mirrors api_integration:
// ephemeral port, wait-for-ready by polling /health with bounded retries, clean server stop+join. A
// BuildOnly reload must come back a clean 400 (never applied). Exit 0 = OK.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "aero/api/rest_api.hpp"
#include "aero/runtime/runtime.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace {
std::string ver_of(httplib::Client& cli) {
    auto s = cli.Get("/status");
    if (!s || s->status != 200) return "";
    auto j = nlohmann::json::parse(s->body, nullptr, false);
    return j.is_discarded() ? "" : j.value("version", std::string{});
}
}  // namespace

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

    bool ready = false;
    for (int i = 0; i < 300 && !ready; ++i) {
        auto h = cli.Get("/health");
        if (h && h->status == 200) ready = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!ready) {
        std::printf("server did not become ready\nFAIL\n");
        svr.stop();
        server_thread.join();
        return 1;
    }

    const char* v1 = R"({"name":"api_app","version":"1","actor":{"kind":"edge","key":6},"flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale","config":{"factor":2}},
        {"type_id":"aero.output.sum"}]})";
    const char* v2 = R"({"name":"api_app","version":"2","actor":{"kind":"edge","key":6},"flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale","config":{"factor":10}},
        {"type_id":"aero.output.sum"}]})";
    // BuildOnly: actor key changed → must be a 400, never applied.
    const char* v_bad = R"({"name":"api_app","version":"9","actor":{"kind":"edge","key":99},"flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale","config":{"factor":10}},
        {"type_id":"aero.output.sum"}]})";

    auto dep = cli.Post("/apps", v1, "application/json");
    const bool dep_ok = dep && dep->status == 200 && ver_of(cli) == "1";

    auto rel = cli.Put("/apps/api_app", v2, "application/json");
    const bool reload_ok = rel && rel->status == 200 && ver_of(cli) == "2";

    auto badrel = cli.Put("/apps/api_app", v_bad, "application/json");
    const bool buildonly_400 = badrel && badrel->status == 400 && ver_of(cli) == "2";

    auto rb = cli.Post("/apps/api_app/rollback", "", "application/json");
    const bool rollback_ok = rb && rb->status == 200 && ver_of(cli) == "1";

    svr.stop();
    server_thread.join();

    std::printf("deploy=%d reload=%d buildonly_400=%d rollback=%d\n", dep_ok, reload_ok,
                buildonly_400, rollback_ok);
    const bool pass = dep_ok && reload_ok && buildonly_400 && rollback_ok;
    std::printf("%s\n", pass ? "OK" : "FAIL");
    return pass ? 0 : 1;
}
