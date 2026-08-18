// AeroEdge 019 slice — GET /catalog regression gate (spec 015 U1). Asserts Runtime::catalog() lists
// every registered node/driver type_id (20: the 14 nodes + 5 drivers wired before this slice, plus the
// new aero.output.http) with the right category/writable/poll_driven, and spot-checks field lists for
// a few non-trivial entries — this IS the drift-prevention gate the whole slice exists for: a node/
// driver added to register_builtins() without a matching kDesc field list should make this fail, not
// silently ship invisible to the Studio (which is exactly what happened to modbus_tcp/modbus_rtu/opcua/
// opcua_subscribe before this slice — see 019-Flow-Graph-Model-and-Studio-Canvas-API.md's ground-truth
// table). Exit code 0 = OK.
#include <cstdio>
#include <set>
#include <string>

#include "aero/runtime/runtime.hpp"

namespace {

bool has_field(const nlohmann::json& fields, const std::string& key) {
    for (const auto& f : fields) {
        if (f.value("key", std::string{}) == key) return true;
    }
    return false;
}

}  // namespace

int main() {
    bool ok = true;
    aero::runtime::Runtime rt;
    const nlohmann::json cat = rt.catalog();

    const std::set<std::string> expected_nodes = {
        "aero.source.decode", "aero.transform.scale", "aero.transform.moving_average",
        "aero.output.sum", "aero.rule.expr", "aero.transform.mean", "aero.transform.minmax",
        "aero.transform.sum", "aero.transform.crc", "aero.source.modbus", "aero.source.modbus_bits",
        "aero.source.json", "aero.output.mes", "aero.source.mes_order", "aero.output.http",
        "aero.flow.switch", "aero.transform.set", "aero.flow.loop_start", "aero.flow.loop_back",
    };
    const std::set<std::string> expected_drivers = {
        "aero.driver.generator", "aero.driver.modbus_tcp", "aero.driver.modbus_rtu",
        "aero.driver.opcua", "aero.driver.opcua_subscribe",
    };

    std::set<std::string> seen_nodes;
    for (const auto& n : cat.value("nodes", nlohmann::json::array())) {
        seen_nodes.insert(n.value("type_id", std::string{}));
    }
    std::set<std::string> seen_drivers;
    for (const auto& d : cat.value("drivers", nlohmann::json::array())) {
        seen_drivers.insert(d.value("type_id", std::string{}));
    }

    const bool nodes_complete = seen_nodes == expected_nodes;
    const bool drivers_complete = seen_drivers == expected_drivers;
    ok &= nodes_complete;
    ok &= drivers_complete;
    std::printf("[catalog] nodes=%zu (expected %zu) drivers=%zu (expected %zu) %s\n", seen_nodes.size(),
                expected_nodes.size(), seen_drivers.size(), expected_drivers.size(),
                (nodes_complete && drivers_complete) ? "ok" : "FAIL");

    // Spot-check: aero.rule.expr must carry BOTH "expr" and "alarm" — the field catalog.ts had been
    // missing before this slice.
    for (const auto& n : cat["nodes"]) {
        if (n.value("type_id", std::string{}) != "aero.rule.expr") continue;
        const bool has_both = has_field(n["fields"], "expr") && has_field(n["fields"], "alarm");
        ok &= has_both;
        std::printf("[catalog] aero.rule.expr fields include expr+alarm: %s\n", has_both ? "ok" : "FAIL");
    }

    // Spot-check: aero.driver.modbus_tcp — previously ABSENT from the Studio entirely — now carries its
    // full real config surface (host/port/unit_id/start_address/register_count/register_type).
    for (const auto& d : cat["drivers"]) {
        if (d.value("type_id", std::string{}) != "aero.driver.modbus_tcp") continue;
        const bool writable = d.value("writable", false);
        const bool poll_driven = d.value("poll_driven", false);
        const bool full_fields = has_field(d["fields"], "host") && has_field(d["fields"], "port") &&
                                  has_field(d["fields"], "unit_id") &&
                                  has_field(d["fields"], "start_address") &&
                                  has_field(d["fields"], "register_count") &&
                                  has_field(d["fields"], "register_type");
        const bool as_expected = writable && poll_driven && full_fields;
        ok &= as_expected;
        std::printf("[catalog] aero.driver.modbus_tcp writable=%d poll_driven=%d full_fields=%d %s\n",
                    writable, poll_driven, full_fields, as_expected ? "ok" : "FAIL");
    }

    // Spot-check: aero.output.http (new this slice) carries its full config surface.
    for (const auto& n : cat["nodes"]) {
        if (n.value("type_id", std::string{}) != "aero.output.http") continue;
        const bool full_fields = has_field(n["fields"], "url") && has_field(n["fields"], "method") &&
                                  has_field(n["fields"], "headers") &&
                                  has_field(n["fields"], "timeout_ms");
        ok &= full_fields;
        std::printf("[catalog] aero.output.http full_fields=%d %s\n", full_fields,
                    full_fields ? "ok" : "FAIL");
    }

    // Spot-check: aero.transform.set (020 §7, new this slice) carries tag+expr.
    for (const auto& n : cat["nodes"]) {
        if (n.value("type_id", std::string{}) != "aero.transform.set") continue;
        const bool full_fields = has_field(n["fields"], "tag") && has_field(n["fields"], "expr");
        ok &= full_fields;
        std::printf("[catalog] aero.transform.set full_fields=%d %s\n", full_fields,
                    full_fields ? "ok" : "FAIL");
    }

    // Spot-check: 020 §8 loop nodes carry their full config surface.
    for (const auto& n : cat["nodes"]) {
        if (n.value("type_id", std::string{}) != "aero.flow.loop_start") continue;
        const bool full_fields = has_field(n["fields"], "counter_tag") && has_field(n["fields"], "start_expr") &&
                                  has_field(n["fields"], "max_iterations") &&
                                  has_field(n["fields"], "max_duration_ms");
        ok &= full_fields;
        std::printf("[catalog] aero.flow.loop_start full_fields=%d %s\n", full_fields,
                    full_fields ? "ok" : "FAIL");
    }
    for (const auto& n : cat["nodes"]) {
        if (n.value("type_id", std::string{}) != "aero.flow.loop_back") continue;
        const bool full_fields = has_field(n["fields"], "counter_tag") && has_field(n["fields"], "step_expr") &&
                                  has_field(n["fields"], "end_expr");
        ok &= full_fields;
        std::printf("[catalog] aero.flow.loop_back full_fields=%d %s\n", full_fields,
                    full_fields ? "ok" : "FAIL");
    }

    // Spot-check (020 §4.3): aero.output.sum is terminal (Cap shape); aero.output.mes/http are NOT
    // (they legitimately stay mid-chain, 012 §4) — the flag must actually distinguish, not default-true
    // everywhere.
    {
        bool sum_terminal = false, mes_terminal = true, http_terminal = true;
        for (const auto& n : cat["nodes"]) {
            const auto id = n.value("type_id", std::string{});
            if (id == "aero.output.sum") sum_terminal = n.value("terminal", false);
            else if (id == "aero.output.mes") mes_terminal = n.value("terminal", false);
            else if (id == "aero.output.http") http_terminal = n.value("terminal", false);
        }
        const bool as_expected = sum_terminal && !mes_terminal && !http_terminal;
        ok &= as_expected;
        std::printf("[catalog] terminal: sum=%d mes=%d http=%d %s\n", sum_terminal, mes_terminal,
                    http_terminal, as_expected ? "ok" : "FAIL");
    }

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
