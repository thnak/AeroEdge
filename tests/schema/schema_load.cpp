// AeroEdge Phase-4 gate — the Application schema loader (spec 009 §2, 013 T3).
//
// Parses a well-formed Application JSON string and asserts every field lands where the canonical C++
// model says it should; then feeds malformed / incomplete JSON and asserts a CLEAN error value (no
// throw, no crash) — the API relies on this to answer 4xx instead of aborting. Exit code 0 = OK.
#include <cstdio>

#include "aero/schema/application.hpp"

int main() {
    const char* good = R"({
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

    auto app = aero::schema::load_application(good);
    bool good_ok = app.has_value();
    if (good_ok) {
        const auto& a = *app;
        good_ok = a.name == "hello_flow" && a.version == "0.1.0" && a.actor.kind == "edge" &&
                  a.actor.key == 7 && a.flow.size() == 3 &&
                  a.flow[0].type_id == "aero.source.decode" &&
                  a.flow[1].type_id == "aero.transform.scale" &&
                  a.flow[1].config.value("factor", 0.0) == 2.0 &&
                  a.flow[2].type_id == "aero.output.sum" && a.driver.has_value() &&
                  a.driver->type_id == "aero.driver.generator" &&
                  a.driver->config.value("frame_count", 0) == 100 && !a.persistence.has_value();
    }

    // Malformed JSON → clean error, no throw.
    auto bad = aero::schema::load_application("{ this is not json ");
    const bool bad_ok = !bad.has_value() && !bad.error().empty();

    // Missing required field (no name) → clean error.
    auto missing = aero::schema::load_application(R"({ "version": "1.0.0", "flow": [] })");
    const bool missing_ok = !missing.has_value();

    // Empty flow → rejected (a flow needs at least one node).
    auto empty_flow = aero::schema::load_application(
        R"({ "name": "x", "version": "1", "flow": [] })");
    const bool empty_ok = !empty_flow.has_value();

    std::printf("good parse   : %s\n", good_ok ? "ok" : "BAD");
    std::printf("bad json     : %s (%s)\n", bad_ok ? "rejected" : "BAD",
                bad.has_value() ? "" : bad.error().c_str());
    std::printf("missing name : %s\n", missing_ok ? "rejected" : "BAD");
    std::printf("empty flow   : %s\n", empty_ok ? "rejected" : "BAD");

    const bool pass = good_ok && bad_ok && missing_ok && empty_ok;
    std::printf("%s\n", pass ? "OK" : "FAIL");
    return pass ? 0 : 1;
}
