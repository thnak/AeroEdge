// AeroEdge Phase-5 gate — versioning + rollback (spec 009 §6).
//
// The Runtime keeps the previously-deployed Application so a rollback re-applies it (a hot-reload back
// to the prior flow). Deploy A (Scale x2), reload to B (Scale x10), then rollback → behavior returns
// to A. Behavior is observed by driving one frame (raw=1) after each transition and reading the
// committed output through the status ask (FIFO after the frame on the Sequential actor). Exit 0 = OK.
#include <cstdio>
#include <string>

#include "aero/runtime/runtime.hpp"

namespace {
int failures = 0;

// Drive one raw=1 frame, then read the committed last_output (a status ask is FIFO after the tell).
double drive_one(aero::runtime::Runtime& rt) {
    (void)rt.tell_frame(1);
    return rt.status().value("last_output", -1.0);
}

void check(const char* label, bool cond, const std::string& detail = "") {
    std::printf("  %-22s : %s  %s\n", label, cond ? "ok" : "FAIL(!)", detail.c_str());
    if (!cond) ++failures;
}
}  // namespace

int main() {
    std::printf("versioning + rollback (009 §6):\n");

    const char* flow_a = R"({
      "name":"app","version":"1.0.0","actor":{"kind":"edge","key":4},"flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale","config":{"factor":2}},
        {"type_id":"aero.output.sum"}]})";
    const char* flow_b = R"({
      "name":"app","version":"2.0.0","actor":{"kind":"edge","key":4},"flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale","config":{"factor":10}},
        {"type_id":"aero.output.sum"}]})";

    aero::runtime::Runtime rt;

    // Rollback with no prior version is a clean error (nothing to roll back to yet).
    if (auto r = rt.rollback(); r.has_value()) {
        std::printf("rollback with nothing deployed should fail\nFAIL\n");
        return 1;
    }

    if (auto r = rt.deploy_json(flow_a); !r) {
        std::printf("deploy A failed: %s\nFAIL\n", r.error().c_str());
        return 1;
    }
    check("A: output x2", drive_one(rt) == 2.0);
    check("A: version 1.0.0", rt.status().value("version", std::string{}) == "1.0.0");

    if (auto r = rt.reload_json(flow_b); !r) {
        std::printf("reload B failed: %s\nFAIL\n", r.error().c_str());
        return 1;
    }
    check("B: output x10", drive_one(rt) == 10.0);
    check("B: version 2.0.0", rt.status().value("version", std::string{}) == "2.0.0");

    if (auto r = rt.rollback(); !r) {
        std::printf("rollback failed: %s\nFAIL\n", r.error().c_str());
        return 1;
    }
    check("rollback: output x2", drive_one(rt) == 2.0);
    check("rollback: version 1.0.0", rt.status().value("version", std::string{}) == "1.0.0");

    std::printf("%s\n", failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}
