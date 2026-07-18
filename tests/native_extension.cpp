// AeroEdge Phase-6 LOAD-BEARING gate — Native extensions over the C ABI (spec 008 §2, E1/E2/E6).
//
// This dlopens a REAL sample `.so` (examples/ext_native/gain_node.cpp, built as a CMake MODULE with
// the SAME compile/sanitizer flags as the host), verifies its ABI, registers its `aero.ext.gain`
// node, deploys a flow using it (Source -> native gain -> Sum) into a Runtime, drives 100 frames
// through the real Quark ingestion path, and asserts the NATIVE node actually transformed the data
// (E1: the flow can't tell it from a built-in). It then proves the E6 ABI gate: a second sample
// reporting a wrong ABI version is REJECTED cleanly at load (no registration, no crash).
//
// The `.so` paths are injected by CMake (AERO_EXT_GAIN_SO / AERO_EXT_BADABI_SO). Exit code 0 = OK.
#include <cstdio>
#include <string>

#include "aero/core/registry.hpp"
#include "aero/ext/native_loader.hpp"
#include "aero/runtime/runtime.hpp"

#ifndef AERO_EXT_GAIN_SO
#error "AERO_EXT_GAIN_SO (path to the sample gain .so) must be defined by CMake"
#endif
#ifndef AERO_EXT_BADABI_SO
#error "AERO_EXT_BADABI_SO (path to the bad-ABI sample .so) must be defined by CMake"
#endif

int main() {
    bool pass = true;
    auto check = [&](const char* what, bool ok) {
        std::printf("%-42s : %s\n", what, ok ? "ok" : "BAD");
        pass = pass && ok;
    };

    // --- 1. Pure ABI-gate predicate (E6) -----------------------------------------------------------
    check("abi_compatible(host version)", aero::ext::abi_compatible(AERO_EXT_ABI_VERSION));
    check("abi_compatible(host+999) rejected", !aero::ext::abi_compatible(AERO_EXT_ABI_VERSION + 999u));

    // --- 2. Load the good `.so` directly and inspect its manifest ----------------------------------
    auto ext = aero::ext::NativeExtension::load(AERO_EXT_GAIN_SO);
    check("load sample gain .so", ext.has_value());
    if (ext) {
        const auto& m = (*ext)->manifest();
        check("manifest abi_version matches host", m.abi_version == AERO_EXT_ABI_VERSION);
        check("manifest provides one node", m.node_count == 1);
        check("manifest node is aero.ext.gain",
              m.node_count == 1 && std::string(m.nodes[0].type_id) == "aero.ext.gain");
    }

    // --- 3. E6: the wrong-ABI `.so` is rejected at load, cleanly (no crash, no registration) --------
    auto bad = aero::ext::NativeExtension::load(AERO_EXT_BADABI_SO);
    check("bad-ABI .so rejected at load", !bad.has_value());
    if (!bad) {
        const bool mentions_abi = bad.error().find("ABI mismatch") != std::string::npos;
        std::printf("   reject reason: %s\n", bad.error().c_str());
        check("reject reason cites ABI mismatch", mentions_abi);
    }
    // Registering the bad `.so` into a registry must add NOTHING.
    {
        aero::NodeRegistry reg;
        auto r = aero::ext::register_native_extension(reg, AERO_EXT_BADABI_SO);
        check("register bad-ABI .so fails", !r.has_value());
        check("registry left empty after bad load", !reg.contains("aero.ext.gain"));
    }

    // --- 4. E1: deploy a flow using the native node through the full Runtime + Quark path -----------
    const char* app = R"({
      "name": "gain_flow",
      "version": "0.1.0",
      "actor": { "kind": "edge", "key": 42 },
      "flow": [
        { "type_id": "aero.source.decode" },
        { "type_id": "aero.ext.gain", "config": { "gain": 3 } },
        { "type_id": "aero.output.sum" }
      ],
      "driver": { "type_id": "aero.driver.generator", "config": { "frame_count": 100 } }
    })";

    aero::runtime::Runtime rt;
    // The wrong-ABI bundle is refused by the Runtime loader too (value error, not a throw).
    check("runtime rejects bad-ABI extension", !rt.load_native_extension(AERO_EXT_BADABI_SO).has_value());

    auto loaded = rt.load_native_extension(AERO_EXT_GAIN_SO);
    check("runtime loads gain extension", loaded.has_value());

    auto deployed = rt.deploy_json(app);
    check("deploy flow with native gain node", deployed.has_value());
    if (!deployed) std::printf("   deploy error: %s\n", deployed.error().c_str());

    rt.await_driver_drain();
    auto st = rt.status();
    const long frames = st.value("frames_processed", 0L);
    const long events = st.value("events_published", 0L);
    const double last = st.value("last_output", 0.0);

    // Frame raw=n -> decode tag "raw"=n -> NATIVE gain x3 -> sum = 3n. Last frame raw=99 -> 297.
    // If the native node did NOT run, sum would be n (last=99). 297 proves the C-ABI transform ran.
    std::printf("frames=%ld events=%ld last_output=%.1f (expect 100/100/297.0)\n", frames, events, last);
    check("native node transformed data (last==297)", last == 297.0);
    check("all 100 frames processed", frames == 100);
    check("one event per frame", events == 100);

    auto u = rt.undeploy();
    check("clean undeploy + teardown", u.has_value() && !rt.status().value("deployed", true));

    std::printf("%s\n", pass ? "OK" : "FAIL");
    return pass ? 0 : 1;
}
