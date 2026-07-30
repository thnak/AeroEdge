// AeroEdge Broker — `acl.hpp`, a broker-local, per-topic authorization seam for `NativeBroker` (M5
// TLS+ACL milestone; 017 §"Phase 1 scope" named Quark 020 authorization as explicitly deferred — this
// is that deferral being paid off, broker-local rather than routed through Quark).
//
// WHY A NEW SEAM INSTEAD OF REUSING quark::Authorizer (020 §3) DIRECTLY: Quark's `Authorizer::allow`
// is keyed on `(Principal, ActorId, TypeKey)` — actor addressing types that exist because Quark routes
// messages between actors. An MQTT broker has no `ActorId`/`TypeKey` on its hot path: what it routes
// on is topic STRINGS with `+`/`#` wildcard semantics (3.1.1 §4.7), which don't map onto Quark's
// addressing model at all. Bolting topic strings onto `ActorId`/`TypeKey` would either lose wildcard
// matching or force a fake actor-space just to satisfy a type signature that doesn't fit the problem.
// So this is a DELIBERATELY INDEPENDENT, purpose-built seam — same shape (boundary-enforcement virtual
// interface + a concrete ordered-rule-table implementation + an explicit default posture), not a
// literal reuse of Quark's classes. (017 N6 correction: this reasoning is canonical — the spec doc
// mirrors this paragraph.)
//
// WHAT THIS FILE DELIBERATELY IS NOT:
//   - Not an authentication system. `Authenticator` (bottom of this file) is a bare function-object
//     TYPE so both NativeBroker's `Config` and whatever concrete authenticator a deployment supplies
//     agree on a shape without a circular include; this file supplies no implementation of it.
//   - Not a general rule DSL. `aero::nodes::ExprRuleNode` (include/aero/nodes/expr_rule_node.hpp,
//     008 §6) already owns "small, config-driven, first-match/short-circuit rule evaluation" as a
//     product surface (flow-authored expressions over tags). `TopicAclAuthorizer` below is calibrated
//     to that same house style — a small ordered rule list, first match wins, explicit default — but
//     it is its own narrow (principal, topic-filter, action) → allow/deny table, not a reuse of the
//     expression engine (an ACL rule is a 3-tuple match, not an expression to evaluate).
//
// WIRED INTO NativeBroker (M5 integration pass): `native_broker.hpp`'s CONNECT/SUBSCRIBE/PUBLISH
// handlers call `Authorizer::allow()` at the CONNECT/SUBSCRIBE/PUBLISH boundary BEFORE any topic state
// (retained store, session subscription list, route_publish fan-out) is touched, so a denied request has
// zero side effects — see `Config::authorizer`/`Config::authenticate` there.
//
// `topic_matches()` now lives in `topic_match.hpp` (extracted from native_broker.hpp's original inline
// copy so both this file and native_broker.hpp share one definition, 017 N3 precedent — the wire codec
// was shared the same way). This file used to carry its own temporary duplicate pending that extraction;
// the extraction has happened, so it now just includes the shared header.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aero/broker/topic_match.hpp"

namespace aero::broker {

// What a topic-scoped ACL rule is deciding about — MQTT's two message-carrying client operations.
// (CONNECT has no topic and is out of scope for this seam; it's an authentication decision, see
// `Authenticator` below, not a per-topic authorization one.)
enum class AclAction : std::uint8_t { Publish, Subscribe };

// The authorization seam (styled after Quark 020's Authorizer boundary-enforcement pattern — see this
// file's banner for why it's NOT a literal reuse of quark::Authorizer). Runs at the CONNECT/SUBSCRIBE/
// PUBLISH boundary, before any topic state is touched — a denied request must have zero side effects
// (no retain, no route, no session-state mutation for a denied SUBSCRIBE's filter). `allow` is a pure,
// `noexcept` predicate (matches quark::Authorizer::allow's own noexcept posture: an authorization
// check must never throw on the admission path) over an immutable policy set up once, cold, before the
// broker starts serving connections.
class Authorizer {
public:
    virtual ~Authorizer() = default;
    [[nodiscard]] virtual bool allow(std::string_view principal, std::string_view topic,
                                     AclAction action) const noexcept = 0;
};

// Trivial authorizers, useful for tests and as an explicit "everything allowed"/"everything denied"
// building block (mirrors quark::AllowAllAuthorizer/DenyAllAuthorizer).
class AllowAllAuthorizer final : public Authorizer {
public:
    [[nodiscard]] bool allow(std::string_view, std::string_view, AclAction) const noexcept override {
        return true;
    }
};

class DenyAllAuthorizer final : public Authorizer {
public:
    [[nodiscard]] bool allow(std::string_view, std::string_view, AclAction) const noexcept override {
        return false;
    }
};

// The default ACL policy: an ORDERED list of rules; the FIRST matching rule wins. A rule matches when
// ALL of the following hold:
//   - the principal pattern matches: either an EXACT string equal to `principal`, or the wildcard "*"
//     meaning "any principal";
//   - the topic filter matches `topic` per MQTT wildcard semantics (topic_matches(), above);
//   - the action matches: EITHER the rule's `action` equals the query's `action`, OR the rule was
//     registered via `add_rule_any_action()` (this class's chosen convenience for "applies to both
//     Publish and Subscribe" — callers wanting one-sided rules for both actions must add two rules
//     with `add_rule()`; this keeps the common "principal X may not touch topic Y at all" case a
//     single call instead of a mandatory pair).
// No matching rule -> the configured Default posture (Closed = deny, Open = allow). Closed is the
// datacenter-standard default (020's own "unlisted route is refused" philosophy), matching
// quark::AclAuthorizer's own default.
//
// Rule storage is a plain `std::vector<Rule>`, scanned linearly in registration order on every
// `allow()` call. This is a cold/setup-time, low-cardinality structure (tens to low-hundreds of rules
// for a real deployment, not millions) — no indexing/hashing is warranted; a linear scan keeps
// first-match-wins trivially correct to read and audit, which matters more here than raw throughput.
class TopicAclAuthorizer final : public Authorizer {
public:
    enum class Default : std::uint8_t { Closed = 0, Open = 1 };

    explicit TopicAclAuthorizer(Default def = Default::Closed) noexcept : default_(def) {}

    // Cold setup path (called during config, not per-message) — appends a rule to the ordered list
    // that matches ONLY the given `action`. No input validation is performed: an empty
    // `topic_filter`/`principal_pattern` or a filter with a mid-string '#' is accepted as written and
    // simply never matches anything at query time (topic_matches()'s own semantics) — malformed-but-
    // inert config is preferred over a throwing/asserting setup path (N3-style: config errors are
    // values or silent no-ops here, never exceptions).
    void add_rule(std::string principal_pattern, std::string topic_filter, AclAction action, bool allow) {
        rules_.push_back(Rule{std::move(principal_pattern), std::move(topic_filter), action, /*any_action=*/false,
                              allow});
    }

    // Convenience: a rule that matches EITHER action (Publish or Subscribe) — for policy that scopes a
    // principal's access to a topic regardless of direction, without requiring two `add_rule()` calls.
    void add_rule_any_action(std::string principal_pattern, std::string topic_filter, bool allow) {
        rules_.push_back(Rule{std::move(principal_pattern), std::move(topic_filter), AclAction::Publish,
                              /*any_action=*/true, allow});
    }

    [[nodiscard]] bool allow(std::string_view principal, std::string_view topic,
                             AclAction action) const noexcept override {
        for (const Rule& r : rules_) {
            if (!r.any_action && r.action != action) continue;
            if (r.principal_pattern != "*" && r.principal_pattern != principal) continue;
            if (!topic_matches(r.topic_filter, topic)) continue;
            return r.allow;  // first match wins
        }
        return default_ == Default::Open;
    }

private:
    struct Rule {
        std::string principal_pattern;  // exact string, or "*" for any principal
        std::string topic_filter;       // MQTT wildcard filter (topic_matches() semantics)
        AclAction action;               // ignored when any_action is true
        bool any_action;                // true => matches both Publish and Subscribe (add_rule_any_action)
        bool allow;                     // the rule's verdict when it matches
    };

    std::vector<Rule> rules_;
    Default default_;
};

// Pluggable authentication: turns MQTT CONNECT username/password into an optional principal id.
// nullopt = reject the CONNECT. The integration pass wires this into NativeBroker::Config; this file
// only defines the type so both sides agree on the shape without a circular include — no
// implementation lives here.
using Authenticator =
    std::function<std::optional<std::string>(std::string_view username, std::string_view password)>;

}  // namespace aero::broker
