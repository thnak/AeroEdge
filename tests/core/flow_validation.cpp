// AeroEdge Phase-5 gate — the Flow Compiler validates at deploy (spec 009 §3 P1, 004 §2.1).
//
// A bad Application must be REJECTED at deploy as a value (std::expected error) — never a crash, never
// a half-deploy: after a rejected deploy the Runtime must still be un-deployed. This drives several
// invalid Applications (unknown node type_id, empty flow, no Source, no Output, bad node config) and
// one valid one, asserting each invalid is refused with a clear error and the valid one is accepted.
// Exit code 0 = OK.
#include <cstdio>
#include <string>

#include "aero/runtime/runtime.hpp"
#include "aero/schema/application.hpp"

namespace {

int failures = 0;

// A deploy_json that MUST be rejected, leaving the runtime un-deployed (no half-deploy, P1).
void expect_reject(const char* label, const std::string& json) {
    aero::runtime::Runtime rt;
    auto r = rt.deploy_json(json);
    const bool rejected = !r.has_value();
    const bool clean = !rt.deployed();  // rejected deploy left NOTHING deployed
    std::printf("  %-22s : %s%s  %s\n", label, rejected ? "rejected" : "ACCEPTED(!)",
                clean ? "" : " +LEFT-DEPLOYED(!)", rejected ? r.error().c_str() : "");
    if (!rejected || !clean) ++failures;
}

}  // namespace

int main() {
    std::printf("flow validation (009 §3, reject invalid at deploy):\n");

    // 1. Unknown node type_id — registry miss (decode ok, then a bogus type).
    expect_reject("unknown-type", R"({
      "name":"bad","version":"1","flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.nope"},
        {"type_id":"aero.output.sum"}]})");

    // 2. scale missing 'factor' — invalid node config.
    expect_reject("scale-no-factor", R"({
      "name":"bad","version":"1","flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale"},
        {"type_id":"aero.output.sum"}]})");

    // 3. moving_average window 0 — invalid node config.
    expect_reject("mavg-window-0", R"({
      "name":"bad","version":"1","flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.moving_average","config":{"window":0}},
        {"type_id":"aero.output.sum"}]})");

    // 4. No Output node — category shape check.
    expect_reject("no-output", R"({
      "name":"bad","version":"1","flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale","config":{"factor":2}}]})");

    // 5. No Source node — category shape check.
    expect_reject("no-source", R"({
      "name":"bad","version":"1","flow":[
        {"type_id":"aero.transform.scale","config":{"factor":2}},
        {"type_id":"aero.output.sum"}]})");

    // 6. Empty flow — built programmatically (load_application rejects empty at JSON shape; this
    //    proves the compiler's own empty guard on the deploy(app) path). Must not crash.
    {
        aero::schema::Application app;
        app.name = "empty";
        app.version = "1";  // app.flow left empty
        aero::runtime::Runtime rt;
        auto r = rt.deploy(app);
        const bool rejected = !r.has_value() && !rt.deployed();
        std::printf("  %-22s : %s  %s\n", "empty-flow", rejected ? "rejected" : "ACCEPTED(!)",
                    r.has_value() ? "" : r.error().c_str());
        if (!rejected) ++failures;
    }

    // 7. A VALID Application — must be accepted and actually deployed.
    {
        aero::runtime::Runtime rt;
        auto r = rt.deploy_json(R"({
          "name":"good","version":"1","actor":{"kind":"edge","key":3},"flow":[
            {"type_id":"aero.source.decode"},
            {"type_id":"aero.transform.scale","config":{"factor":2}},
            {"type_id":"aero.output.sum"}]})");
        const bool ok = r.has_value() && rt.deployed();
        std::printf("  %-22s : %s  %s\n", "valid", ok ? "accepted" : "REJECTED(!)",
                    r.has_value() ? "" : r.error().c_str());
        if (!ok) ++failures;
    }

    std::printf("%s\n", failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}
