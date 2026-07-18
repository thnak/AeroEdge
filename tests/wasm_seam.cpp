// AeroEdge Phase-6 gate — the WASM extension seam (spec 008 §3, E1/E3/E4). THE HONEST-GATE TEST.
//
// There is no WASM runtime in this offline build (008 §8), so this does NOT execute WASM. It proves
// the SEAM is real and compiles: the IWasmRuntime interface, the capability model (E3), the fuel
// bound (E4), and the WasmNode INode adapter (E1) are all well-formed types; and that the shipped
// `NullWasmRuntime` returns the documented gate error verbatim from every path — i.e. deploying a
// WASM node offline fails cleanly at deploy, never with a faked result (R5). Exit code 0 = OK.
#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

#include "aero/ext/wasm_runtime.hpp"

using namespace aero::ext;

int main() {
    bool pass = true;
    auto check = [&](const char* what, bool ok) {
        std::printf("%-52s : %s\n", what, ok ? "ok" : "BAD");
        pass = pass && ok;
    };

    // --- capability model (E3): grant covers request iff every required cap is granted --------------
    CapabilitySet granted{Capability::ReadTags, Capability::EmitEvents};
    CapabilitySet ok_req{Capability::ReadTags};
    CapabilitySet bad_req{Capability::ReadTags, Capability::OpenSocket};
    check("granted covers a subset request", granted.covers(ok_req));
    check("granted does NOT cover OpenSocket", !granted.covers(bad_req));
    check("has() reflects membership", granted.has(Capability::EmitEvents) && !granted.has(Capability::MesReport));

    // --- NullWasmRuntime returns the gate error verbatim from every path (R5) ----------------------
    NullWasmRuntime rt;
    std::vector<std::byte> module_bytes(8, std::byte{0});  // not a real module — nothing executes it
    FuelBudget budget{/*fuel*/ 1'000'000, /*max_steps*/ 0};

    auto lm = rt.load_module(std::as_bytes(std::span(module_bytes)), granted);
    check("load_module gated", !lm.has_value() && lm.error() == std::string(kWasmGateError));

    HostFunction hf{"aero_read_tag", Capability::ReadTags, nullptr};
    auto rh = rt.register_host_function(WasmModuleHandle{1}, hf);
    check("register_host_function gated", !rh.has_value() && rh.error() == std::string(kWasmGateError));

    auto in = rt.instantiate(WasmModuleHandle{1}, budget);
    check("instantiate gated", !in.has_value() && in.error() == std::string(kWasmGateError));

    aero::ProcessingContext ctx;
    auto cp = rt.call_process(WasmInstance{1}, ctx);
    check("call_process gated", !cp.has_value() && cp.error() == std::string(kWasmGateError));

    // --- WasmNode adapter (E1): configure() fails at deploy with the gate error; process() -> Error -
    {
        WasmNode node(rt, module_bytes, /*requires*/ ok_req, /*granted*/ granted, budget);
        auto cfg = node.configure();
        check("WasmNode.configure() hits the gate", !cfg.has_value() && cfg.error() == std::string(kWasmGateError));
        check("unbound WasmNode.process() -> Error", node.process(ctx) == aero::NodeResult::Error);
        check("WasmNode IS an INode (E1)", node.descriptor().category == aero::NodeCategory::Transform);
    }

    // --- capability least-privilege is enforced BEFORE touching the runtime (E3) -------------------
    {
        WasmNode node(rt, module_bytes, /*requires*/ bad_req, /*granted*/ granted, budget);
        auto cfg = node.configure();
        const bool cap_denied = !cfg.has_value() && cfg.error().find("capabilities") != std::string::npos;
        std::printf("   least-privilege deny: %s\n", cfg ? "(granted?!)" : cfg.error().c_str());
        check("under-granted WasmNode denied on capabilities", cap_denied);
    }

    std::printf("gate message: \"%s\"\n", std::string(kWasmGateError).c_str());
    std::printf("%s\n", pass ? "OK" : "FAIL");
    return pass ? 0 : 1;
}
