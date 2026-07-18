// aero-sdk-native — the header a NATIVE EXTENSION AUTHOR compiles against (spec 008 §3).
//
// A native extension is an out-of-tree `.so`/`.dll` that provides nodes/drivers over the stable
// C ABI (ext_abi.h). This header is that ABI plus a few zero-cost convenience helpers so an author
// writes only their node logic, not boilerplate. It pulls in NOTHING from aero-core / the STL — an
// extension links ONLY this, keeping the C ABI honest (no C++ type crosses the boundary, E2). It
// compiles as either C or C++ (the sample uses C++ with `extern "C"` linkage).
#ifndef AERO_SDK_NATIVE_AERO_NODE_H
#define AERO_SDK_NATIVE_AERO_NODE_H

#include "aero/sdk/ext_abi.h"

// Export/linkage macro for the three required entry points. `extern "C"` gives them C linkage even
// in a .cpp translation unit; visibility("default") keeps them dlsym-resolvable under -fvisibility=hidden.
#ifdef __cplusplus
#  define AERO_EXT_LINKAGE extern "C"
#else
#  define AERO_EXT_LINKAGE
#endif
#if defined(_WIN32)
#  define AERO_EXT_EXPORT AERO_EXT_LINKAGE __declspec(dllexport)
#else
#  define AERO_EXT_EXPORT AERO_EXT_LINKAGE __attribute__((visibility("default")))
#endif

// Convenience: build one manifest node entry. Keeps the author's static manifest table terse.
static inline AeroExtNodeInfo aero_ext_node_info(const char* type_id, AeroNodeCategory category) {
    AeroExtNodeInfo info;
    info.type_id  = type_id;
    info.category = (uint32_t)category;
    return info;
}

// Convenience: fill an AeroNodeDescriptorC from a descriptor() implementation.
static inline void aero_ext_fill_descriptor(AeroNodeDescriptorC* out, AeroNodeCategory category,
                                            const char* type_id) {
    out->category = (uint32_t)category;
    out->type_id  = type_id;
}

#endif  // AERO_SDK_NATIVE_AERO_NODE_H
