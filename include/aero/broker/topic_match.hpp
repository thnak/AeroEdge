// AeroEdge Broker — `topic_matches()`, MQTT 3.1.1 §4.7 topic-filter wildcard matching. Shared by
// `NativeBroker` (session subscription routing) and `acl.hpp` (per-topic ACL rule matching) — both need
// the EXACT same wildcard semantics, so it lives here once instead of drifting between two copies
// (017 N3 precedent: the wire codec, mqtt_codec.hpp, was shared the same way).
//
// Extracted from native_broker.hpp's original inline definition (M5 TLS+ACL integration pass) — this is
// a pure, behavior-preserving move, not a logic change. acl.hpp's own copy (banner-commented "DUPLICATED
// CODE, TEMPORARY") is the deferred duplicate this extraction was always meant to pay off.
#pragma once

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

}  // namespace aero::broker
