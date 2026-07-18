// Sample AeroEdge native extension with a DELIBERATELY WRONG ABI version (spec 008 §2, E6).
//
// Identical to gain_node.cpp except it reports an ABI the host was NOT built against. The
// `native_extension` test dlopens this to prove the host's ABI gate REJECTS it cleanly at load —
// no registration, no crash, no runtime surprise (E6: "a mismatch fails the load at deploy"). This
// is the honest failure path a real version-skewed bundle would hit.
#include <cstring>

#include "aero/sdk_native/aero_node.h"

namespace {

// Whatever the host requires, this library claims a different, incompatible revision.
constexpr uint32_t kBadAbiVersion = AERO_EXT_ABI_VERSION + 999u;

struct BadNode {};

AeroNodeResult bad_process(void*, AeroCtx*, const AeroHostApi*) { return AERO_NODE_CONTINUE; }
int  bad_configure(void*, const char*, size_t) { return 0; }
void bad_descriptor(void*, AeroNodeDescriptorC* out) {
    aero_ext_fill_descriptor(out, AERO_CAT_TRANSFORM, "aero.ext.gain");
}
void bad_destroy(void* self) { delete static_cast<BadNode*>(self); }

constexpr AeroNodeVTable kVTable{&bad_process, &bad_configure, &bad_descriptor, &bad_destroy};
constexpr AeroExtNodeInfo kNodes[]{{"aero.ext.gain", AERO_CAT_TRANSFORM}};
constexpr AeroExtManifest kManifest{
    "aero-ext-badabi-sample", "0.1.0", kBadAbiVersion, 1, kNodes, 0, nullptr,
};

}  // namespace

AERO_EXT_EXPORT uint32_t aero_ext_abi_version(void) { return kBadAbiVersion; }
AERO_EXT_EXPORT const AeroExtManifest* aero_ext_manifest(void) { return &kManifest; }
AERO_EXT_EXPORT AeroExtHandle aero_ext_create(const char* type_id) {
    AeroExtHandle handle{nullptr, nullptr};
    if (type_id != nullptr && std::strcmp(type_id, "aero.ext.gain") == 0) {
        handle.self = new BadNode();
        handle.vtable = &kVTable;
    }
    return handle;
}
