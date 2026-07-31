// AeroEdge broker ACL gate (M5 TLS+ACL milestone): pure-logic coverage of `include/aero/broker/acl.hpp`
// — no sockets, no threads, this is a policy-table unit test mirroring
// `native_broker.cpp`'s own `test_topic_matches()` in spirit but exercising the new
// `Authorizer`/`TopicAclAuthorizer` seam instead. Deterministic, exit-code-gated (0 = pass), same
// `ok &=` accumulation + FAIL_REGULAR_EXPRESSION convention as every other AeroEdge test.
//
// Covers: AllowAllAuthorizer/DenyAllAuthorizer triviality; TopicAclAuthorizer's Closed/Open default
// posture with zero rules; exact-principal + exact-topic + specific-action matching; the "*" principal
// wildcard; MQTT topic-filter wildcard matching (`+`/`#`) reused from native_broker.hpp's own
// topic_matches() cases; first-match-wins ordering (an earlier specific deny beats a later broad
// allow); and Publish/Subscribe action independence (a single-action rule does not leak into the other
// action) alongside add_rule_any_action()'s explicit both-actions behavior.
#include <cstdio>
#include <string>

#include "aero/broker/acl.hpp"

using aero::broker::AclAction;
using aero::broker::AllowAllAuthorizer;
using aero::broker::Authorizer;
using aero::broker::DenyAllAuthorizer;
using aero::broker::TopicAclAuthorizer;
using aero::broker::topic_matches;

namespace {

bool test_trivial_authorizers() {
    bool ok = true;
    AllowAllAuthorizer allow_all;
    ok &= allow_all.allow("anonymous", "any/topic", AclAction::Publish);
    ok &= allow_all.allow("alice", "sensors/room1/temp", AclAction::Subscribe);

    DenyAllAuthorizer deny_all;
    ok &= !deny_all.allow("anonymous", "any/topic", AclAction::Publish);
    ok &= !deny_all.allow("alice", "sensors/room1/temp", AclAction::Subscribe);
    return ok;
}

bool test_default_posture() {
    bool ok = true;
    // Closed + zero rules -> everything denied.
    TopicAclAuthorizer closed(TopicAclAuthorizer::Default::Closed);
    ok &= !closed.allow("alice", "sensors/room1/temp", AclAction::Publish);
    ok &= !closed.allow("anonymous", "any/topic", AclAction::Subscribe);

    // Open + zero rules -> everything allowed.
    TopicAclAuthorizer open(TopicAclAuthorizer::Default::Open);
    ok &= open.allow("alice", "sensors/room1/temp", AclAction::Publish);
    ok &= open.allow("anonymous", "any/topic", AclAction::Subscribe);
    return ok;
}

bool test_exact_and_wildcard_principal_matching() {
    bool ok = true;
    TopicAclAuthorizer authz(TopicAclAuthorizer::Default::Closed);
    // Exact principal + exact topic + specific action.
    authz.add_rule("alice", "sensors/room1/temp", AclAction::Publish, /*allow=*/true);
    ok &= authz.allow("alice", "sensors/room1/temp", AclAction::Publish);
    // Different principal, same topic/action -> falls through to default (Closed = deny).
    ok &= !authz.allow("bob", "sensors/room1/temp", AclAction::Publish);
    // Same principal, different topic -> falls through to default.
    ok &= !authz.allow("alice", "sensors/room2/temp", AclAction::Publish);
    // Same principal/topic, different action -> falls through to default (action independence).
    ok &= !authz.allow("alice", "sensors/room1/temp", AclAction::Subscribe);

    // "*" principal pattern matches any principal.
    TopicAclAuthorizer any_principal(TopicAclAuthorizer::Default::Closed);
    any_principal.add_rule("*", "status/#", AclAction::Subscribe, /*allow=*/true);
    ok &= any_principal.allow("alice", "status/line1", AclAction::Subscribe);
    ok &= any_principal.allow("bob", "status/line2/detail", AclAction::Subscribe);
    ok &= !any_principal.allow("alice", "status/line1", AclAction::Publish);  // wrong action
    return ok;
}

bool test_topic_wildcards_reused_from_native_broker_semantics() {
    bool ok = true;
    // Same cases native_broker.cpp's test_topic_matches() exercises, adapted for the ACL rule table.
    TopicAclAuthorizer plus_rule(TopicAclAuthorizer::Default::Closed);
    plus_rule.add_rule("*", "sensors/+/temp", AclAction::Publish, /*allow=*/true);
    ok &= plus_rule.allow("dev1", "sensors/room1/temp", AclAction::Publish);
    ok &= !plus_rule.allow("dev1", "sensors/room1/sub/temp", AclAction::Publish);  // '+' doesn't cross '/'
    ok &= !plus_rule.allow("dev1", "sensors/room1/humidity", AclAction::Publish);  // wrong leaf

    TopicAclAuthorizer hash_rule(TopicAclAuthorizer::Default::Closed);
    hash_rule.add_rule("*", "sensors/#", AclAction::Publish, /*allow=*/true);
    ok &= hash_rule.allow("dev1", "sensors/room1/temp", AclAction::Publish);
    ok &= hash_rule.allow("dev1", "sensors/room1/sub/temp", AclAction::Publish);  // '#' spans levels
    ok &= hash_rule.allow("dev1", "sensors", AclAction::Publish);  // '#' matches zero further levels

    // Sanity-check the duplicated topic_matches() itself against the same cases, so a future drift from
    // native_broker.hpp's copy is caught directly, not just indirectly through TopicAclAuthorizer.
    ok &= topic_matches("sensors/+/temp", "sensors/room1/temp");
    ok &= !topic_matches("sensors/+/temp", "sensors/room1/sub/temp");
    ok &= topic_matches("sensors/#", "sensors/room1/temp");
    ok &= topic_matches("sensors/#", "sensors/room1/sub/temp");
    return ok;
}

bool test_first_match_wins() {
    bool ok = true;
    TopicAclAuthorizer authz(TopicAclAuthorizer::Default::Closed);
    // Earlier, more specific "deny alice" precedes a later, broader "allow anyone".
    authz.add_rule("alice", "status/line1", AclAction::Publish, /*allow=*/false);
    authz.add_rule("*", "status/line1", AclAction::Publish, /*allow=*/true);

    ok &= !authz.allow("alice", "status/line1", AclAction::Publish);  // earlier deny wins
    ok &= authz.allow("bob", "status/line1", AclAction::Publish);     // falls through to the broad allow
    return ok;
}

bool test_action_independence_and_any_action_convenience() {
    bool ok = true;
    // A rule scoped to Publish must not silently apply to Subscribe.
    TopicAclAuthorizer one_sided(TopicAclAuthorizer::Default::Closed);
    one_sided.add_rule("alice", "cmd/line1", AclAction::Publish, /*allow=*/true);
    ok &= one_sided.allow("alice", "cmd/line1", AclAction::Publish);
    ok &= !one_sided.allow("alice", "cmd/line1", AclAction::Subscribe);

    // add_rule_any_action() explicitly opts into matching both actions.
    TopicAclAuthorizer both(TopicAclAuthorizer::Default::Closed);
    both.add_rule_any_action("alice", "cmd/line1", /*allow=*/true);
    ok &= both.allow("alice", "cmd/line1", AclAction::Publish);
    ok &= both.allow("alice", "cmd/line1", AclAction::Subscribe);
    ok &= !both.allow("bob", "cmd/line1", AclAction::Publish);  // principal still scoped
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_trivial_authorizers();
    ok &= test_default_posture();
    ok &= test_exact_and_wildcard_principal_matching();
    ok &= test_topic_wildcards_reused_from_native_broker_semantics();
    ok &= test_first_match_wins();
    ok &= test_action_independence_and_any_action_convenience();

    std::printf("acl: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
