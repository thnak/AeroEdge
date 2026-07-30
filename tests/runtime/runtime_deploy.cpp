// AeroEdge Phase-4 LOAD-BEARING gate — declarative deploy → running flow, end to end (spec 009).
//
// IN-PROCESS, NO SOCKET: build a Runtime, deploy an Application from JSON (a FlowActor running
// Source → Scale(2) → Sum, fed by a generator driver of 100 frames), let the bounded driver drain, and
// assert the status snapshot reports the expected frames processed and last output. This proves the
// whole Phase-4 thesis — a declarative Application compiles (009 §3) into a running Quark actor whose
// flow executes every ingested frame — without any HTTP in the loop. Exit code 0 = OK.
#include <cstdio>
#include <string>

#include "aero/runtime/runtime.hpp"

int main() {
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

    aero::runtime::Runtime rt;
    auto r = rt.deploy_json(app);
    if (!r) {
        std::printf("deploy failed: %s\nFAIL\n", r.error().c_str());
        return 1;
    }

    // Bounded driver: 100 frames (seq 0..99) → producer finishes, bridge tells all 100, both join.
    rt.await_driver_drain();

    // A status ask is FIFO after all 100 frame tells on a Sequential actor → observes the full count.
    auto st = rt.status();
    const std::string name = st.value("name", std::string{});
    const long frames = st.value("frames_processed", 0L);
    const long events = st.value("events_published", 0L);
    const double last = st.value("last_output", 0.0);

    // Frame raw=n → decode tag "raw"=n → scale ×2 → sum = 2n. Last frame raw=99 → 198. One event/frame.
    const bool ok = name == "hello_flow" && frames == 100 && events == 100 && last == 198.0;

    std::printf("deployed name    : %s   (expected hello_flow)\n", name.c_str());
    std::printf("frames processed : %ld  (expected 100)\n", frames);
    std::printf("events published : %ld  (expected 100)\n", events);
    std::printf("last output      : %.1f (expected 198.0)\n", last);

    // Undeploy tears everything down cleanly; a second status must report not-deployed.
    auto u = rt.undeploy();
    const bool torn_down = u.has_value() && !rt.status().value("deployed", true);
    std::printf("undeploy + teardown: %s\n", torn_down ? "clean" : "BAD");

    const bool pass = ok && torn_down;
    std::printf("%s\n", pass ? "OK" : "FAIL");
    return pass ? 0 : 1;
}
