// AeroEdge — the WASM extension seam (spec 008 §3, invariants E1/E3/E4). INTERFACE + GATE ONLY.
//
// HONEST SCOPE (R5): AeroEdge's WASM story is a sandboxed, capability-gated, fuel-bounded node that
// runs UNTRUSTED / marketplace code (008 §3). Executing WASM needs an embedded runtime (wasmtime /
// wasm3 / WAMR behind this seam — 008 §8 open question). No such runtime is available in this
// offline build, so this header defines the CONTRACT a real backend must satisfy and ships a
// `NullWasmRuntime` that returns a clear, documented gate error. NOTHING here fakes WASM execution —
// the seam is real, the backend is gated. Vendoring a runtime is a later phase.
//
// WHY the seam is defined now: it pins the shapes a backend and the rest of AeroEdge must agree on —
//   * E1: a WASM node is an ordinary INode (WasmNode below) — the flow executor can't tell it apart;
//   * E3: memory + capability isolation — the guest reaches the host ONLY through host functions the
//         deployment granted, keyed to declared capabilities (Quark 020 trust root);
//   * E4: fuel/step bounds — a runaway guest fails (NodeResult::Error), it never hangs a worker (I1).
// A real backend implements IWasmRuntime; WasmNode and the capability model do not change when it lands.
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aero/sdk/node.hpp"
#include "aero/sdk/processing_context.hpp"

namespace aero::ext {

// The gate message every stubbed path returns, verbatim, so a caller (and tests/wasm_seam.cpp) can
// recognise "no WASM backend configured" distinctly from a real runtime error.
inline constexpr std::string_view kWasmGateError =
    "WASM runtime not configured (008 gate: no offline runtime)";

// --- Capability model (008 §3/§5/§7 E3) ------------------------------------------------------------
// A WASM node's manifest DECLARES the capabilities it needs; the deployment GRANTS a set; the runtime
// exports only the host functions whose capability is in the granted set (least privilege). Native
// extensions ignore this (they run with full trust, E6) — it is the WASM sandbox's authorization axis.
enum class Capability : std::uint32_t {
    ReadTags    = 0,  // read the working-set tags (context view in)
    WriteTags   = 1,  // mutate/append tags (context view out)
    EmitEvents  = 2,  // stage Events
    StageOutput = 3,  // stage egress values
    MesReport   = 4,  // call the MES hook (012)
    OpenSocket  = 5,  // network egress — high privilege, rarely granted
};

// A small, allocation-light capability set. A real deployment builds the granted set from policy; a
// module's `requires` set comes from its manifest (008 §5).
class CapabilitySet {
public:
    CapabilitySet() = default;
    CapabilitySet(std::initializer_list<Capability> caps) {
        for (auto c : caps) add(c);
    }
    void add(Capability c) { bits_ |= (std::uint64_t{1} << static_cast<std::uint32_t>(c)); }
    [[nodiscard]] bool has(Capability c) const {
        return (bits_ & (std::uint64_t{1} << static_cast<std::uint32_t>(c))) != 0;
    }
    // True iff every capability in `required` is present here (grant covers request).
    [[nodiscard]] bool covers(const CapabilitySet& required) const {
        return (required.bits_ & ~bits_) == 0;
    }
    [[nodiscard]] std::uint64_t raw() const noexcept { return bits_; }

private:
    std::uint64_t bits_ = 0;
};

// The resource bound a guest runs under (008 §3 E4). `fuel` is the wasmtime-style consumption budget;
// `max_steps` a fuel-less interpreter's instruction cap. Exceeding either fails the node, never hangs.
struct FuelBudget {
    std::uint64_t fuel = 0;       // 0 => backend default
    std::uint64_t max_steps = 0;  // 0 => backend default
};

// A host function the runtime may export into the guest, gated by a capability (E3). Registration is
// how the host grants controlled reach; the backend wires `fn` behind the guest import table only if
// `required` is in the granted set.
struct HostFunction {
    std::string_view name;
    Capability required;
    // Opaque backend-specific thunk pointer (a real backend defines the signature). Kept as void* at
    // the seam so this header needs no runtime type.
    void* thunk = nullptr;
};

// Opaque handles a backend hands back. Kept as typed integers so the seam carries no backend types.
struct WasmModuleHandle { std::uint64_t id = 0; [[nodiscard]] bool valid() const noexcept { return id != 0; } };
struct WasmInstance     { std::uint64_t id = 0; [[nodiscard]] bool valid() const noexcept { return id != 0; } };

// The runtime seam a real WASM backend implements (008 §3). Every method returns std::expected so a
// gate/backend failure is a value, never a throw — matching the deploy path's error discipline (P1).
class IWasmRuntime {
public:
    virtual ~IWasmRuntime() = default;

    // Load a `.wasm` module, binding the capabilities the deployment grants it (E3).
    virtual std::expected<WasmModuleHandle, std::string>
    load_module(std::span<const std::byte> wasm, const CapabilitySet& granted) = 0;

    // Export a capability-gated host function into a module's import space (E3).
    virtual std::expected<void, std::string>
    register_host_function(WasmModuleHandle module, const HostFunction& fn) = 0;

    // Instantiate a loaded module under a resource bound (E4).
    virtual std::expected<WasmInstance, std::string>
    instantiate(WasmModuleHandle module, const FuelBudget& budget) = 0;

    // Call the guest's exported process(): marshal the needed slice of `ctx` into guest memory, run
    // (fuel-bounded), copy results back (008 §3). Pointers never cross — that is the sandbox (E3).
    virtual std::expected<NodeResult, std::string>
    call_process(WasmInstance instance, ProcessingContext& ctx) = 0;

    virtual void unload(WasmModuleHandle module) noexcept = 0;
};

// The GATE (R5): a fully-wired IWasmRuntime that executes nothing and says so, verbatim
// (kWasmGateError). Deploying a WASM node offline fails HERE, cleanly, at deploy — never with a
// pretend result. Swapping in a real backend is the only change needed to lift the gate.
class NullWasmRuntime final : public IWasmRuntime {
public:
    std::expected<WasmModuleHandle, std::string>
    load_module(std::span<const std::byte>, const CapabilitySet&) override {
        return std::unexpected(std::string(kWasmGateError));
    }
    std::expected<void, std::string>
    register_host_function(WasmModuleHandle, const HostFunction&) override {
        return std::unexpected(std::string(kWasmGateError));
    }
    std::expected<WasmInstance, std::string>
    instantiate(WasmModuleHandle, const FuelBudget&) override {
        return std::unexpected(std::string(kWasmGateError));
    }
    std::expected<NodeResult, std::string>
    call_process(WasmInstance, ProcessingContext&) override {
        return std::unexpected(std::string(kWasmGateError));
    }
    void unload(WasmModuleHandle) noexcept override {}
};

// The WASM node adapter (E1): an ordinary INode that WOULD forward process() across the runtime seam.
// It binds its module + capabilities at configure() (deploy time); with a NullWasmRuntime that bind
// FAILS with kWasmGateError, so an offline deploy of a WASM node is rejected honestly at deploy, not
// at runtime. With a real backend, `configure()` loads+instantiates and process() calls call_process.
class WasmNode final : public INode {
public:
    WasmNode(IWasmRuntime& runtime, std::vector<std::byte> module_bytes,
             CapabilitySet requires_caps, CapabilitySet granted_caps, FuelBudget budget) noexcept
        : runtime_(&runtime),
          module_bytes_(std::move(module_bytes)),
          requires_(requires_caps),
          granted_(granted_caps),
          budget_(budget) {}

    // Deploy-time bind: enforce capability least-privilege (E3), then load + instantiate under the
    // fuel bound (E4). Returns the backend/gate error as a value (P1) — the runtime never throws.
    [[nodiscard]] std::expected<void, std::string> configure() {
        if (!granted_.covers(requires_)) {
            return std::unexpected("WASM node requires capabilities the deployment did not grant (E3)");
        }
        auto module = runtime_->load_module(std::as_bytes(std::span(module_bytes_)), granted_);
        if (!module) return std::unexpected(module.error());
        module_ = *module;
        auto inst = runtime_->instantiate(module_, budget_);
        if (!inst) return std::unexpected(inst.error());
        instance_ = *inst;
        return {};
    }

    NodeResult process(ProcessingContext& ctx) noexcept override {
        if (!instance_.valid()) return NodeResult::Error;  // not bound (gate/backend failure at deploy)
        auto r = runtime_->call_process(instance_, ctx);
        return r ? *r : NodeResult::Error;  // a fuel-exhausted / trapped guest fails, never hangs (E4)
    }

    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr NodeDescriptor kDesc{NodeCategory::Transform, "aero.wasm.node"};

private:
    IWasmRuntime* runtime_;
    std::vector<std::byte> module_bytes_;
    CapabilitySet requires_;
    CapabilitySet granted_;
    FuelBudget budget_;
    WasmModuleHandle module_{};
    WasmInstance instance_{};
};

}  // namespace aero::ext
