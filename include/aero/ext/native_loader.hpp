// AeroEdge — the HOST side of the Native extension seam (spec 008 §2, invariants E1/E2/E6).
//
// This loads an out-of-tree `.so`, checks its ABI against the host (E6), reads its manifest, and
// registers a factory per provided node into a NodeRegistry (005 §5). Each factory constructs a
// `NativeNode` — a C++ INode adapter that forwards process()/descriptor() across the C vtable
// (ext_abi.h). From the flow executor's view the result is an ordinary INode (E1); the C boundary
// (E2) is entirely inside NativeNode + the host accessor table below.
//
// LIFETIME / dlclose SAFETY (008 §5, 009 §4 "Native = BuildOnly"): the loaded library must outlive
// every node instance it created — calling into an unloaded `.so` is UB. We enforce this by
// ref-counting: NativeExtension owns the dl handle; every NativeNode holds a shared_ptr to it, and
// the registered factory captures one too. dlclose() therefore runs only after the registry entry
// AND every live node are gone (deterministic, no call ever inside an unloaded library).
//
// THIN-OVER-QUARK (R0): this is pure domain glue (dlopen + an INode adapter) — it writes no
// scheduler/mailbox/thread. Native runs in-process with full trust (008 §2); sandboxing is the WASM
// seam's job (wasm_runtime.hpp), not this one.
#pragma once

#include <dlfcn.h>

#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aero/core/registry.hpp"
#include "aero/sdk/ext_abi.h"
#include "aero/sdk/node.hpp"
#include "aero/sdk/processing_context.hpp"
#include "nlohmann/json.hpp"

namespace aero::ext {

// E6 ABI gate as a pure, unit-testable predicate: the host loads a library only when it reports
// exactly the ABI revision the host was built against. Any mismatch fails the load at deploy.
[[nodiscard]] inline bool abi_compatible(std::uint32_t ext_abi_version) noexcept {
    return ext_abi_version == AERO_EXT_ABI_VERSION;
}

// Map the C AeroNodeResult / AeroNodeCategory back to the host enums (the only place they meet).
[[nodiscard]] inline NodeResult from_c(AeroNodeResult r) noexcept {
    switch (r) {
        case AERO_NODE_STOP:  return NodeResult::Stop;
        case AERO_NODE_ERROR: return NodeResult::Error;
        default:              return NodeResult::Continue;
    }
}
[[nodiscard]] inline NodeCategory category_from_c(std::uint32_t c) noexcept {
    switch (c) {
        case AERO_CAT_SOURCE:    return NodeCategory::Source;
        case AERO_CAT_RULE:      return NodeCategory::Rule;
        case AERO_CAT_OUTPUT:    return NodeCategory::Output;
        default:                 return NodeCategory::Transform;
    }
}

// The host accessor table (ext_abi.h AeroHostApi). AeroCtx* IS the host's ProcessingContext — the
// extension only ever sees the opaque pointer, never the C++ type (E2). Defined once, inline.
namespace detail {
[[nodiscard]] inline ProcessingContext* ctx(AeroCtx* c) noexcept {
    return reinterpret_cast<ProcessingContext*>(c);  // opaque handle round-trip (host-owned)
}
inline std::size_t host_tag_count(AeroCtx* c) { return ctx(c)->tags.size(); }
inline int host_tag_get(AeroCtx* c, std::size_t i, const char** name, std::size_t* name_len,
                        double* value) {
    auto& tags = ctx(c)->tags;
    if (i >= tags.size()) return 1;
    *name = tags[i].name.data();
    *name_len = tags[i].name.size();
    *value = tags[i].value;
    return 0;
}
inline int host_tag_set_value(AeroCtx* c, std::size_t i, double value) {
    auto& tags = ctx(c)->tags;
    if (i >= tags.size()) return 1;
    tags[i].value = value;
    return 0;
}
inline void host_tag_push(AeroCtx* c, const char* name, std::size_t name_len, double value) {
    // The extension guarantees `name` lives for the flow's duration (a static literal in the .so,
    // kept loaded by NativeNode's shared_ptr) — so a borrowing string_view is safe (Tag contract).
    ctx(c)->tags.push_back(Tag{std::string_view{name, name_len}, value});
}
inline void host_output_push(AeroCtx* c, double value) { ctx(c)->output.push_back(value); }
inline void host_event_push(AeroCtx* c, const char* type, std::size_t type_len, double value) {
    ctx(c)->events.push_back(Event{std::string_view{type, type_len}, value});
}

inline constexpr AeroHostApi kHostApi{
    &host_tag_count, &host_tag_get,      &host_tag_set_value,
    &host_tag_push,  &host_output_push,  &host_event_push,
};
}  // namespace detail

class NativeExtension;  // fwd

// A C++ INode that forwards across the native C vtable (E1/E2). Holds a shared_ptr to the owning
// library so the `.so` cannot be unloaded while this node (or its static descriptor type_id) lives.
class NativeNode final : public INode {
public:
    NativeNode(AeroExtHandle handle, NodeDescriptor desc,
               std::shared_ptr<const NativeExtension> owner) noexcept
        : handle_(handle), desc_(desc), owner_(std::move(owner)) {}

    NativeNode(const NativeNode&) = delete;
    NativeNode& operator=(const NativeNode&) = delete;

    ~NativeNode() override {
        if (handle_.vtable && handle_.self) {
            handle_.vtable->destroy(handle_.self);  // per-instance teardown before dlclose (E5)
        }
    }

    NodeResult process(ProcessingContext& ctx) noexcept override {
        // Cross the C boundary: the extension must not throw (E2) — this call is noexcept, so a
        // stray throw would std::terminate, which is the honest contract, not silent corruption.
        const AeroNodeResult r =
            handle_.vtable->process(handle_.self, reinterpret_cast<AeroCtx*>(&ctx), &detail::kHostApi);
        return from_c(r);
    }

    const NodeDescriptor& descriptor() const noexcept override { return desc_; }

private:
    AeroExtHandle handle_;
    NodeDescriptor desc_;  // type_id points at a static literal inside `owner_`'s loaded library
    std::shared_ptr<const NativeExtension> owner_;
};

// One loaded native library. Owns the dl handle + the resolved exports; hands out NativeNode
// instances. Non-copyable; always held by shared_ptr so lifetime is ref-counted against its nodes.
class NativeExtension : public std::enable_shared_from_this<NativeExtension> {
public:
    NativeExtension(const NativeExtension&) = delete;
    NativeExtension& operator=(const NativeExtension&) = delete;

    ~NativeExtension() {
        if (handle_) dlclose(handle_);  // safe: every NativeNode holds a shared_ptr to us (above)
    }

    // dlopen the library, resolve + ABI-check the exports (E6). On ANY failure returns an error
    // value with nothing left loaded — a bad/incompatible bundle never half-registers (008 §2).
    [[nodiscard]] static std::expected<std::shared_ptr<NativeExtension>, std::string>
    load(const std::string& path) {
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            const char* e = dlerror();
            return std::unexpected("dlopen('" + path + "') failed: " + (e ? e : "unknown"));
        }
        auto ext = std::shared_ptr<NativeExtension>(new NativeExtension(h, path));

        ext->abi_fn_ = reinterpret_cast<AeroExtAbiVersionFn>(dlsym(h, "aero_ext_abi_version"));
        ext->manifest_fn_ = reinterpret_cast<AeroExtManifestFn>(dlsym(h, "aero_ext_manifest"));
        ext->create_fn_ = reinterpret_cast<AeroExtCreateFn>(dlsym(h, "aero_ext_create"));
        if (!ext->abi_fn_ || !ext->manifest_fn_ || !ext->create_fn_) {
            return std::unexpected("'" + path + "' is not an AeroEdge native extension "
                                   "(missing aero_ext_abi_version/manifest/create)");
        }

        const std::uint32_t ext_abi = ext->abi_fn_();
        if (!abi_compatible(ext_abi)) {
            return std::unexpected("ABI mismatch loading '" + path + "': extension built for ABI " +
                                   std::to_string(ext_abi) + ", host requires ABI " +
                                   std::to_string(AERO_EXT_ABI_VERSION) + " — refusing to load (E6)");
        }
        ext->manifest_ = ext->manifest_fn_();
        if (!ext->manifest_) {
            return std::unexpected("'" + path + "' returned a null manifest");
        }
        // Defensive: the manifest's own abi_version must agree with the export too.
        if (!abi_compatible(ext->manifest_->abi_version)) {
            return std::unexpected("'" + path + "' manifest abi_version " +
                                   std::to_string(ext->manifest_->abi_version) + " != host " +
                                   std::to_string(AERO_EXT_ABI_VERSION));
        }
        return ext;
    }

    [[nodiscard]] const AeroExtManifest& manifest() const noexcept { return *manifest_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // Construct one node by type_id with its JSON config (called by the registry factory at deploy).
    // Never throws: create/configure failures come back as an error value (P1, 009 §3).
    [[nodiscard]] std::expected<std::unique_ptr<INode>, std::string>
    create_node(const std::string& type_id, const nlohmann::json& config) const {
        AeroExtHandle handle = create_fn_(type_id.c_str());
        if (!handle.self || !handle.vtable) {
            return std::unexpected("extension '" + path_ + "' does not provide node '" + type_id + "'");
        }
        const std::string cfg = config.dump();
        if (const int rc = handle.vtable->configure(handle.self, cfg.data(), cfg.size()); rc != 0) {
            handle.vtable->destroy(handle.self);  // reject cleanly — no leak, no half-built node
            return std::unexpected("native node '" + type_id + "' rejected its config (rc=" +
                                   std::to_string(rc) + ")");
        }
        AeroNodeDescriptorC dc{};
        handle.vtable->descriptor(handle.self, &dc);
        // dc.type_id points at a static literal in the loaded library, kept alive by the shared_ptr
        // the NativeNode holds — so the borrowing NodeDescriptor::type_id string_view stays valid.
        NodeDescriptor desc{category_from_c(dc.category),
                            std::string_view{dc.type_id ? dc.type_id : type_id.c_str()}};
        return std::make_unique<NativeNode>(handle, desc, shared_from_this());
    }

private:
    NativeExtension(void* handle, std::string path) noexcept
        : handle_(handle), path_(std::move(path)) {}

    void* handle_ = nullptr;
    std::string path_;
    AeroExtAbiVersionFn abi_fn_ = nullptr;
    AeroExtManifestFn manifest_fn_ = nullptr;
    AeroExtCreateFn create_fn_ = nullptr;
    const AeroExtManifest* manifest_ = nullptr;
};

// Load a native extension and register a factory per provided node into `registry` (005 §5). Each
// factory keeps the library loaded (captured shared_ptr) so it stays available across redeploys and
// unloads only when the registry is destroyed. Returns the loaded extension (kept alive by the
// factories regardless) or a clean error — an incompatible/broken bundle registers NOTHING.
[[nodiscard]] inline std::expected<std::shared_ptr<NativeExtension>, std::string>
register_native_extension(NodeRegistry& registry, const std::string& path) {
    auto ext = NativeExtension::load(path);
    if (!ext) return std::unexpected(ext.error());

    const AeroExtManifest& m = (*ext)->manifest();
    for (std::size_t i = 0; i < m.node_count; ++i) {
        const std::string type_id = m.nodes[i].type_id;
        registry.register_type(type_id, [ext = *ext, type_id](const nlohmann::json& config)
                                            -> std::unique_ptr<INode> {
            auto node = ext->create_node(type_id, config);
            // NodeFactory has no error channel; deploy-time validation (flow_compiler) checks the
            // config first, so a failure here is unexpected — surface it as a null (compile_flow's
            // registry.create wraps the factory; a throwing factory would be caught upstream). We
            // return nullptr and let the caller treat it as "node unavailable".
            return node ? std::move(*node) : nullptr;
        });
    }
    return *ext;
}

}  // namespace aero::ext
