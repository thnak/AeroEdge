// AeroEdge SDK — the INode extension contract (spec 005).
//
// Every pluggable unit — built-in, native, or WASM — implements this. A node knows ONLY its
// ProcessingContext (I4); it never sees the Actor, Flow, or Engine. `process()` is the per-Command
// hot method: noexcept, non-blocking, allocation-free on the steady path (N1). `descriptor()` is a
// static identity/category used for DAG validation (Phase-5 compiler). Slots/versioning (005 §3)
// are added when the compiler needs them; Phase-1 keeps the descriptor minimal.
#pragma once

#include <span>
#include <string_view>

#include "aero/sdk/config_schema.hpp"
#include "aero/sdk/processing_context.hpp"

namespace aero {

enum class NodeResult {
    Continue,  // proceed to the next node
    Stop,      // short-circuit the flow (e.g. a Rule decided "done") — not an error
    Error,     // the flow failed at this node; the actor decides recovery (004 §6)
};

enum class NodeCategory {
    Source,     // introduce data from the frame
    Transform,  // reshape data
    Rule,       // business logic / routing
    Output,     // stage egress
};

struct NodeDescriptor {
    NodeCategory category;
    std::string_view type_id;  // stable identity, e.g. "aero.transform.scale"
    // The config schema `GET /catalog` serves (015 U1) — what this type_id's JSON config accepts.
    // Default-empty so every existing 2-field `kDesc{...}` brace-init keeps compiling unchanged
    // (same posture as `DriverDescriptor::poll_driven`'s additive default).
    std::span<const FieldSpec> config_fields{};
    // 020 §4.3: true for a node type that must be the flow's LAST step — a Cap shape (Previous only,
    // no Next) in the block-taxonomy sense (020 §3), not a Stack. Default-false/additive, same posture
    // as `config_fields` above — every existing kDesc{...} brace-init keeps compiling unchanged.
    // aero.output.mes/aero.output.http stay non-terminal (false): they legitimately stay mid-chain as a
    // non-stopping side effect (012 §4); aero.output.sum is, in practice, always the flow's true end.
    bool terminal = false;
};

class INode {
public:
    virtual ~INode() = default;

    // The only method the flow executor calls per Command. See N1–N6 (005 §7).
    virtual NodeResult process(ProcessingContext& ctx) noexcept = 0;

    // Static descriptor for DAG validation and registry lookup.
    virtual const NodeDescriptor& descriptor() const noexcept = 0;
};

}  // namespace aero
