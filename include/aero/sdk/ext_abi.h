// AeroEdge SDK — the Native extension C ABI (spec 008 §2, invariant E2).
//
// This is the STABLE C boundary a native (.so/.dll) extension exports. It is deliberately C, not
// C++: C++ types (std::string, std::vector, exceptions, RTTI) MUST NEVER cross it (E2), so an
// extension and the host can be built by different compilers / STL versions and still interoperate.
// The host wraps whatever this exposes in a C++ `INode` adapter (native_loader.hpp) — from the flow
// executor's view a native node is indistinguishable from a built-in (E1).
//
// Boundary rules (008 §2):
//   * No C++ types cross — only C scalars, C strings (ptr+len), and the flat structs below.
//   * No exceptions cross — a node reports failure with AeroNodeResult, never by throwing (E2).
//   * ProcessingContext is NOT copied across the wire. The host passes an OPAQUE `AeroCtx*` (its own
//     ProcessingContext) plus a table of accessor function pointers (`AeroHostApi`) the extension
//     calls to read/write the working set. Pointers into host memory therefore stay host-owned; the
//     extension only touches the context through the accessors. (For WASM, 008 §3, the same context
//     would instead be MARSHALLED into guest linear memory — that is the coarser, sandboxed seam.)
//   * ABI versioning: the host checks `aero_ext_abi_version()` against AERO_EXT_ABI_VERSION and
//     refuses to load a mismatch at deploy, never at runtime (E6).
#ifndef AERO_SDK_EXT_ABI_H
#define AERO_SDK_EXT_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The ABI revision this header defines. Bumped on any breaking change to the structs/exports below.
// The host embeds this constant and rejects a library reporting a different aero_ext_abi_version().
#define AERO_EXT_ABI_VERSION 1u

// Mirror of aero::NodeResult (node.hpp). A native node returns one of these from process(); it maps
// 1:1 to the host enum. There is NO "throw" — a failure is AERO_NODE_ERROR (E2).
typedef enum AeroNodeResult {
    AERO_NODE_CONTINUE = 0,  // proceed to the next node
    AERO_NODE_STOP     = 1,  // short-circuit the flow (a Rule decided "done") — not an error
    AERO_NODE_ERROR    = 2,  // the node failed; the actor decides recovery
} AeroNodeResult;

// Mirror of aero::NodeCategory (node.hpp), used for DAG validation on the host side.
typedef enum AeroNodeCategory {
    AERO_CAT_SOURCE    = 0,
    AERO_CAT_TRANSFORM = 1,
    AERO_CAT_RULE      = 2,
    AERO_CAT_OUTPUT    = 3,
} AeroNodeCategory;

// Opaque handle to the host's ProcessingContext. The extension NEVER dereferences this — it only
// passes it back to the AeroHostApi accessors. That is what keeps host C++ types off the boundary.
typedef struct AeroCtx AeroCtx;

// The host-provided accessor table. The host fills this once and hands a const pointer to every
// process() call; the extension reads/writes the working set exclusively through these calls. All
// strings are (ptr, len) pairs — no NUL assumption, no C++ string. Names handed to tag_push MUST
// point at storage that outlives the flow (a compiled-in literal in the extension is fine — the .so
// stays loaded while any of its nodes live, see native_loader.hpp / 009 §4 Native = BuildOnly).
typedef struct AeroHostApi {
    // Number of tags in the current working set.
    size_t (*tag_count)(AeroCtx* ctx);
    // Read tag i into (name, name_len, value). Returns 0 on success, non-zero if i is out of range.
    int    (*tag_get)(AeroCtx* ctx, size_t i, const char** name, size_t* name_len, double* value);
    // Overwrite the value of tag i in place (an in-place transform). Non-zero if i is out of range.
    int    (*tag_set_value)(AeroCtx* ctx, size_t i, double value);
    // Append a new tag to the working set. `name` must be stable for the flow's duration (see above).
    void   (*tag_push)(AeroCtx* ctx, const char* name, size_t name_len, double value);
    // Stage one egress value (Output nodes).
    void   (*output_push)(AeroCtx* ctx, double value);
    // Emit an Event to be published post-commit. `type` must be stable for the flow's duration.
    void   (*event_push)(AeroCtx* ctx, const char* type, size_t type_len, double value);
} AeroHostApi;

// A node's static identity, filled by the extension's descriptor() so the host can validate the DAG
// and build a C++ NodeDescriptor. `type_id` must point at storage that lives as long as the loaded
// library (a static string literal); the host adapter holds the pointer, not a copy.
typedef struct AeroNodeDescriptorC {
    uint32_t    category;  // one of AeroNodeCategory
    const char* type_id;   // e.g. "aero.ext.gain"
} AeroNodeDescriptorC;

// The per-instance vtable of C function pointers a native node exposes. The host wraps `self` +
// this vtable in a C++ INode adapter (NativeNode). All functions take the opaque `self` first.
typedef struct AeroNodeVTable {
    // Per-Command hot method (mirrors INode::process). MUST NOT throw, MUST NOT block (E2/E5, N1).
    AeroNodeResult (*process)(void* self, AeroCtx* ctx, const AeroHostApi* host);
    // Called ONCE after create with the node's config as JSON text (ptr+len). 0 = ok, non-zero =
    // rejected (bad config) — the host fails the deploy cleanly, it does not run the node (N3).
    int            (*configure)(void* self, const char* config_json, size_t config_len);
    // Fill the static descriptor (called once, right after configure).
    void           (*descriptor)(void* self, AeroNodeDescriptorC* out);
    // Destroy the instance. Called once, on flow teardown, before the library may be unloaded.
    void           (*destroy)(void* self);
} AeroNodeVTable;

// The result of aero_ext_create: an opaque instance + its vtable. A NULL vtable (or NULL self)
// signals "this library does not provide that type_id" — the host reports a clean error, no crash.
typedef struct AeroExtHandle {
    void*                  self;
    const AeroNodeVTable*  vtable;
} AeroExtHandle;

// Describes one node/driver a library provides (copied into the host's registry at load).
typedef struct AeroExtNodeInfo {
    const char* type_id;   // stable identity registered into the host NodeRegistry (005 §5)
    uint32_t    category;  // one of AeroNodeCategory (advisory; the instance's descriptor() is authoritative)
} AeroExtNodeInfo;

// The library's self-description (008 §5). Returned by aero_ext_manifest(); all pointers must be
// stable for the library's lifetime (static storage). `requires_capabilities` is the WASM/marketplace
// capability declaration (008 §5/§7 E3) — for a Native extension it is advisory/audit metadata only,
// since Native runs with full process trust (008 §2, E6).
typedef struct AeroExtManifest {
    const char*             name;
    const char*             version;
    uint32_t                abi_version;  // must equal AERO_EXT_ABI_VERSION for the host to load it
    size_t                  node_count;
    const AeroExtNodeInfo*  nodes;
    size_t                  capability_count;
    const char* const*      requires_capabilities;  // may be NULL when capability_count == 0
} AeroExtManifest;

// --- The three exports every native extension MUST provide (008 §2) ---------------------------
//
//   uint32_t             aero_ext_abi_version(void);       // ABI it was built against (E6 gate)
//   const AeroExtManifest* aero_ext_manifest(void);        // what it provides (copied into host)
//   AeroExtHandle        aero_ext_create(const char* type_id);  // construct one node by type_id
//
// The host resolves these by name with dlsym (native_loader.hpp). Destruction is per-instance via
// the vtable's destroy(); the library is dlclose()d only after every instance is gone (E5, 009 §4).
typedef uint32_t               (*AeroExtAbiVersionFn)(void);
typedef const AeroExtManifest* (*AeroExtManifestFn)(void);
typedef AeroExtHandle          (*AeroExtCreateFn)(const char* type_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AERO_SDK_EXT_ABI_H
