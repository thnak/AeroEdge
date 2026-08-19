// AeroEdge SDK — the per-plugin config field schema (spec 015 §2, U1: "config schema is the single
// source of truth" — the runtime's descriptor, not a hand-maintained Studio array). A `NodeDescriptor`/
// `DriverDescriptor` (node.hpp/driver.hpp) carries a `std::span<const FieldSpec>` describing exactly
// what its `type_id`'s JSON config accepts — the same shape `register_builtins()` (runtime.hpp) already
// reads with `c.value(key, default)`. `GET /catalog` (aero-api) serves this so the Studio's config forms
// can't drift from what the runtime actually accepts (013 T3).
//
// Rich, generic-form-unfriendly fields (an OPC UA `security` object, a Modbus register map) are typed
// `Object`/`StringArray` and carry a `tier2_hint` — the Tier-1/Tier-2 split (015 §3) stays a Studio-side
// concern; this header only describes the field, it does not render it.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace aero {

enum class FieldType : std::uint8_t {
    Number,       // floating point
    Int,          // integer
    String,       // free text
    Bool,
    Enum,         // string constrained to `enum_options`
    StringArray,  // e.g. OPC UA node_ids — Tier-2 in practice (015 §3)
    Object,       // opaque nested config (OPC UA `security`, HTTP `headers`) — always Tier-2
};

// One config key a node/driver's JSON accepts. Deliberately flat/POD (no std::string/std::vector) so a
// whole field list can be a `static constexpr std::array<FieldSpec, N>` referenced by a descriptor's
// `std::span`, exactly like `NodeDescriptor`/`DriverDescriptor` themselves (005/006).
struct FieldSpec {
    std::string_view key;
    std::string_view label;
    FieldType type = FieldType::String;
    bool required = false;

    // Only the member matching `type` is meaningful; the others sit at their zero value.
    double default_number = 0.0;           // Number / Int
    std::string_view default_string = {};  // String / Enum
    bool default_bool = false;             // Bool

    bool has_min = false;
    double min = 0.0;  // Number / Int only, when has_min

    std::string_view help = {};
    std::string_view tier2_hint = {};                  // e.g. "opcua-security", "http-headers"
    std::span<const std::string_view> enum_options{};  // Enum only
};

}  // namespace aero
