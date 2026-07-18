// Sample AeroEdge NATIVE extension — `aero.ext.gain` (spec 008 §2 / §3).
//
// A native extension is an out-of-tree shared library compiled ONLY against aero-sdk-native (the C
// ABI, ext_abi.h) — it links nothing from aero-core and shares no C++ type with the host (E2). This
// file is C++ but every boundary function has `extern "C"` linkage; nothing but C scalars, C strings,
// and the flat ABI structs cross. The node multiplies every working-set tag by a gain (default 3.0,
// overridable via `{"gain": N}` config), doing MEANINGFUL work per call (a whole tag sweep) per the
// coarse-granularity rule (008 §4 G1). The `native_extension` test dlopens exactly this library.
#include <cstring>
#include <string>

#include "aero/sdk_native/aero_node.h"

namespace {

constexpr double kDefaultGain = 3.0;

// One node instance. Pure C-compatible state behind an opaque `void* self` — no C++ type escapes.
struct GainNode {
    double gain = kDefaultGain;
};

// A deliberately tiny config reader: find `"gain"` in the JSON text and parse the number after the
// colon. Kept dependency-free ON PURPOSE — a native extension must not assume the host's JSON library
// (E2). A production extension would ship its own parser; a threshold-simple field needs only this.
double read_gain(const char* json, size_t len, double fallback) {
    if (json == nullptr || len == 0) return fallback;
    const std::string s(json, len);
    const std::string key = "\"gain\"";
    const size_t k = s.find(key);
    if (k == std::string::npos) return fallback;
    size_t i = k + key.size();
    while (i < s.size() && (s[i] == ':' || s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size()) return fallback;
    try {
        size_t consumed = 0;
        const double v = std::stod(s.substr(i), &consumed);
        return consumed > 0 ? v : fallback;
    } catch (...) {
        return fallback;  // never throw across the boundary (E2) — fall back to the default
    }
}

// --- vtable implementations (C linkage, no exceptions cross) ---
AeroNodeResult gain_process(void* self, AeroCtx* ctx, const AeroHostApi* host) {
    auto* node = static_cast<GainNode*>(self);
    const size_t n = host->tag_count(ctx);
    for (size_t i = 0; i < n; ++i) {
        const char* name = nullptr;
        size_t name_len = 0;
        double value = 0.0;
        if (host->tag_get(ctx, i, &name, &name_len, &value) != 0) break;
        host->tag_set_value(ctx, i, value * node->gain);  // the meaningful per-call work (G1)
    }
    return AERO_NODE_CONTINUE;
}

int gain_configure(void* self, const char* config_json, size_t len) {
    auto* node = static_cast<GainNode*>(self);
    node->gain = read_gain(config_json, len, kDefaultGain);
    return 0;  // accepts any config (missing/invalid → default gain)
}

void gain_descriptor(void* /*self*/, AeroNodeDescriptorC* out) {
    aero_ext_fill_descriptor(out, AERO_CAT_TRANSFORM, "aero.ext.gain");
}

void gain_destroy(void* self) { delete static_cast<GainNode*>(self); }

constexpr AeroNodeVTable kGainVTable{
    &gain_process, &gain_configure, &gain_descriptor, &gain_destroy,
};

// --- manifest (008 §5): what this library provides. All pointers are static storage. ---
constexpr AeroExtNodeInfo kNodes[]{
    {"aero.ext.gain", AERO_CAT_TRANSFORM},
};
constexpr AeroExtManifest kManifest{
    /*name*/ "aero-ext-gain-sample",
    /*version*/ "0.1.0",
    /*abi_version*/ AERO_EXT_ABI_VERSION,
    /*node_count*/ 1,
    /*nodes*/ kNodes,
    /*capability_count*/ 0,
    /*requires_capabilities*/ nullptr,  // Native = full trust; capabilities are the WASM axis (E6)
};

}  // namespace

// --- the three required exports (ext_abi.h) ---
AERO_EXT_EXPORT uint32_t aero_ext_abi_version(void) { return AERO_EXT_ABI_VERSION; }

AERO_EXT_EXPORT const AeroExtManifest* aero_ext_manifest(void) { return &kManifest; }

AERO_EXT_EXPORT AeroExtHandle aero_ext_create(const char* type_id) {
    AeroExtHandle handle{nullptr, nullptr};
    if (type_id != nullptr && std::strcmp(type_id, "aero.ext.gain") == 0) {
        handle.self = new GainNode();
        handle.vtable = &kGainVTable;
    }
    return handle;  // NULL vtable => "not provided" — the host reports a clean error (no crash)
}
