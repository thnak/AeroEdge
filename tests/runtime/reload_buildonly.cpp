// AeroEdge Phase-5 gate — Live vs BuildOnly classification + enforcement (spec 009 §4 P3).
//
// Not every deploy change can be a live pointer-swap. A Live change (flow graph / node config) hot-
// reloads; a BuildOnly change (actor kind/key, persistence model/mode) CANNOT and must be rejected
// with a clear error — never half-applied. This deploys a base app, then attempts several reloads:
// a Live one (succeeds), and BuildOnly ones (actor key change, persistence mode change) that must be
// refused while the running app stays intact. Exit 0 = OK.
#include <cstdio>
#include <string>

#include "aero/runtime/runtime.hpp"

namespace {
int failures = 0;

void check(const char* label, bool cond, const std::string& detail = "") {
    std::printf("  %-26s : %s  %s\n", label, cond ? "ok" : "FAIL(!)", detail.c_str());
    if (!cond) ++failures;
}
}  // namespace

int main() {
    std::printf("reload classification (009 §4 Live vs BuildOnly):\n");

    aero::runtime::Runtime rt;
    if (auto r = rt.deploy_json(R"({
      "name":"svc","version":"1","actor":{"kind":"edge","key":9},
      "persistence":{"model":"snapshot","mode":"sync"},"flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale","config":{"factor":2}},
        {"type_id":"aero.output.sum"}]})"); !r) {
        std::printf("deploy failed: %s\nFAIL\n", r.error().c_str());
        return 1;
    }

    // Live change: node config (scale factor) only — must succeed and bump the version.
    {
        auto r = rt.reload_json(R"({
          "name":"svc","version":"2","actor":{"kind":"edge","key":9},
          "persistence":{"model":"snapshot","mode":"sync"},"flow":[
            {"type_id":"aero.source.decode"},
            {"type_id":"aero.transform.scale","config":{"factor":5}},
            {"type_id":"aero.output.sum"}]})");
        check("live: config change", r.has_value(), r.has_value() ? "" : r.error());
        check("live: version advanced", rt.status().value("version", std::string{}) == "2");
    }

    // BuildOnly: actor key change — must be rejected, running version unchanged.
    {
        auto r = rt.reload_json(R"({
          "name":"svc","version":"3","actor":{"kind":"edge","key":10},
          "persistence":{"model":"snapshot","mode":"sync"},"flow":[
            {"type_id":"aero.source.decode"},
            {"type_id":"aero.transform.scale","config":{"factor":5}},
            {"type_id":"aero.output.sum"}]})");
        const bool rejected = !r.has_value() &&
                              r.error().find("BuildOnly") != std::string::npos;
        check("buildonly: actor key", rejected, r.has_value() ? "ACCEPTED(!)" : r.error());
        check("buildonly: version held", rt.status().value("version", std::string{}) == "2");
    }

    // BuildOnly: persistence mode change (sync -> async) — must be rejected.
    {
        auto r = rt.reload_json(R"({
          "name":"svc","version":"3","actor":{"kind":"edge","key":9},
          "persistence":{"model":"snapshot","mode":"async"},"flow":[
            {"type_id":"aero.source.decode"},
            {"type_id":"aero.transform.scale","config":{"factor":5}},
            {"type_id":"aero.output.sum"}]})");
        const bool rejected = !r.has_value() &&
                              r.error().find("BuildOnly") != std::string::npos;
        check("buildonly: persistence mode", rejected, r.has_value() ? "ACCEPTED(!)" : r.error());
    }

    // An invalid Live reload (bad config) must also be rejected without disturbing the running flow.
    {
        auto r = rt.reload_json(R"({
          "name":"svc","version":"3","actor":{"kind":"edge","key":9},
          "persistence":{"model":"snapshot","mode":"sync"},"flow":[
            {"type_id":"aero.source.decode"},
            {"type_id":"aero.transform.scale"},
            {"type_id":"aero.output.sum"}]})");
        check("invalid live: rejected", !r.has_value(), r.has_value() ? "ACCEPTED(!)" : r.error());
        check("invalid live: version held", rt.status().value("version", std::string{}) == "2");
    }

    std::printf("%s\n", failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}
