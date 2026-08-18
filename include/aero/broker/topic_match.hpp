// AeroEdge Broker — `topic_matches()`, MQTT 3.1.1 §4.7 topic-filter wildcard matching. Shared by
// `NativeBroker` (session subscription routing) and `acl.hpp` (per-topic ACL rule matching) — both need
// the EXACT same wildcard semantics, so it lives here once instead of drifting between two copies
// (017 N3 precedent: the wire codec, mqtt_codec.hpp, was shared the same way).
//
// Extracted from native_broker.hpp's original inline definition (M5 TLS+ACL integration pass) — this is
// a pure, behavior-preserving move, not a logic change. acl.hpp's own copy (banner-commented "DUPLICATED
// CODE, TEMPORARY") is the deferred duplicate this extraction was always meant to pay off.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace aero::broker {

// MQTT topic-filter matching (3.1.1 §4.7): `+` matches exactly one level, `#` matches the rest of the
// topic (including zero further levels) and must be the filter's last level to have any effect beyond
// literal comparison. Pure function, independently testable.
[[nodiscard]] inline bool topic_matches(std::string_view filter, std::string_view topic) noexcept {
    auto split = [](std::string_view s) {
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        for (;;) {
            const auto pos = s.find('/', start);
            if (pos == std::string_view::npos) {
                parts.push_back(s.substr(start));
                break;
            }
            parts.push_back(s.substr(start, pos - start));
            start = pos + 1;
        }
        return parts;
    };
    const auto f = split(filter);
    const auto t = split(topic);
    std::size_t i = 0;
    for (; i < f.size(); ++i) {
        if (f[i] == "#") return true;         // matches everything remaining, including zero levels
        if (i >= t.size()) return false;      // filter has more levels than topic, and it's not '#'
        if (f[i] == "+") continue;            // matches exactly this one level
        if (f[i] != t[i]) return false;
    }
    return i == t.size();  // no '#' consumed the tail => topic must end exactly where the filter does
}

// A Topic Name (unlike a Topic Filter) must never contain a wildcard character — required for
// Response Topic (§3.3.2.3.5) and any other place a Properties field claims to be a publishable
// Topic Name rather than a subscribable Topic Filter.
[[nodiscard]] inline bool topic_name_has_wildcard(std::string_view topic) noexcept {
    return topic.find('+') != std::string_view::npos || topic.find('#') != std::string_view::npos;
}

// MQTT 5 §4.8.2 Shared Subscriptions: a SUBSCRIBE filter of the form `$share/<ShareName>/<TopicFilter>`
// puts this subscriber into a named group — the broker delivers each matching message to exactly ONE
// member of the group (round-robin/random/whatever it likes), not to every member the way a regular
// subscription would. `ShareName` MUST NOT contain `/`, `+`, or `#` (§4.8.2) since it isn't itself a
// topic-filter level; a violation is a Protocol Error, same as a malformed filter with no ShareName or
// no TopicFilter after it — the caller is expected to reject the SUBSCRIBE in that case, not silently
// treat it as a literal (non-shared) filter.
struct SharedSubscription {
    std::string_view group;
    std::string_view filter;  // the real Topic Filter, with the "$share/<group>/" prefix stripped
};

[[nodiscard]] inline std::optional<SharedSubscription> parse_shared_subscription(
    std::string_view raw_filter) noexcept {
    constexpr std::string_view kPrefix = "$share/";
    if (raw_filter.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    const auto rest = raw_filter.substr(kPrefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) return std::nullopt;  // no TopicFilter after the ShareName
    const auto group = rest.substr(0, slash);
    const auto filter = rest.substr(slash + 1);
    if (group.empty() || filter.empty()) return std::nullopt;
    if (group.find('+') != std::string_view::npos || group.find('#') != std::string_view::npos ||
        group.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    return SharedSubscription{group, filter};
}

}  // namespace aero::broker
