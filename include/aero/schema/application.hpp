// AeroEdge schema — the declarative Application model + JSON loader (spec 009 §2, 013 T3).
//
// An Application is the unit of deployment: a versioned, self-contained description of a coherent
// slice of edge behavior — the actor it binds to, the ordered flow (node type_ids + per-node config),
// an optional driver, and an optional persistence declaration. The flow *topology* is data (this
// schema); the node *logic* is compiled C++/WASM (005/008). This is the low-code/pro-code split made
// concrete (009 §2).
//
// This C++ struct + parser is the CANONICAL contract, the single source of truth (013 T3): the Studio
// codegen to TS/C# (Phase 9) derives FROM this shape, never the reverse. `load_application` parses AND
// validates the JSON *shape* (required fields, types) — it does NOT resolve node type_ids or validate
// the DAG; that is the Flow Compiler's job at deploy (009 §3, Runtime::deploy). Bad JSON → a clean
// error string, never a throw across the API boundary.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace aero::schema {

// One step of the flow pipeline (009 §2): a node type_id + its opaque config object. The config is
// passed verbatim to the node factory (005 §5); the schema does not interpret it. `id` is a stable,
// edge-addressable identity (019 §1) — always populated by `load_application` (explicit from JSON, or
// synthesized "n<index>" if absent), so `edges[]` can reference any node regardless of whether the
// JSON author supplied one.
struct NodeSpec {
    std::string id;
    std::string type_id;
    nlohmann::json config = nlohmann::json::object();
};

// One edge of the flow graph (019 §1/§2). `from_port` is empty for an unconditional/unlabeled edge, or
// a branch label (currently only "true"/"false", set by `aero.flow.switch`) for a conditional one —
// there is no `to_port`: nodes have no per-input slot to address (every node reads/writes the SAME
// shared ProcessingContext buffers), so an edge only needs to say WHICH node runs next and under what
// branch condition, not which of that node's "inputs" it feeds.
struct EdgeSpec {
    std::string from;
    std::string from_port;
    std::string to;
};

// The optional ingestion driver bound to the actor (006). config carries e.g. {"frame_count": N}.
struct DriverSpec {
    std::string type_id;
    nlohmann::json config = nlohmann::json::object();
};

// The actor binding (009 §2 "bindings: actor-kind → flows/drivers"). `key` is the Quark ActorId key.
struct ActorSpec {
    std::string kind = "edge";
    std::uint64_t key = 0;
};

// Optional durable-state declaration (007 §1 tier-1). Phase-4 records intent; the persistence path is
// driven by the actor (persistent_actor.hpp). model = snapshot|event_sourced, mode = sync|async.
struct PersistenceSpec {
    std::string model;
    std::string mode;
};

struct Application {
    std::string name;
    std::string version;
    ActorSpec actor;
    std::vector<NodeSpec> flow;  // ordered; array order IS the DAG when `edges` is empty (019 G6)
    std::vector<EdgeSpec> edges;  // optional (019 §1/§2) — non-empty opts into a real graph, letting a
                                  // node have more than one next/previous step (fan-out/merge/branch)
    std::optional<DriverSpec> driver;
    std::optional<PersistenceSpec> persistence;
};

// Parse + shape-validate an Application from JSON text. Returns the Application or a human-readable
// error (no exception escapes — bad input is a value, not a throw, so the API can 4xx cleanly).
inline std::expected<Application, std::string> load_application(const std::string& json_text) {
    nlohmann::json j = nlohmann::json::parse(json_text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        return std::unexpected("invalid JSON: parse error");
    }
    if (!j.is_object()) {
        return std::unexpected("Application must be a JSON object");
    }

    Application app;

    if (!j.contains("name") || !j["name"].is_string()) {
        return std::unexpected("Application.name (string) is required");
    }
    app.name = j["name"].get<std::string>();

    if (!j.contains("version") || !j["version"].is_string()) {
        return std::unexpected("Application.version (string) is required");
    }
    app.version = j["version"].get<std::string>();

    // actor (optional block; defaults to {kind:"edge", key:0}).
    if (j.contains("actor") && !j["actor"].is_null()) {
        const auto& a = j["actor"];
        if (!a.is_object()) {
            return std::unexpected("Application.actor must be an object");
        }
        if (a.contains("kind")) {
            if (!a["kind"].is_string()) return std::unexpected("actor.kind must be a string");
            app.actor.kind = a["kind"].get<std::string>();
        }
        if (a.contains("key")) {
            if (!a["key"].is_number_integer() && !a["key"].is_number_unsigned()) {
                return std::unexpected("actor.key must be an integer");
            }
            app.actor.key = a["key"].get<std::uint64_t>();
        }
    }

    // flow (required, non-empty ordered array of node specs).
    if (!j.contains("flow") || !j["flow"].is_array() || j["flow"].empty()) {
        return std::unexpected("Application.flow (non-empty array) is required");
    }
    std::size_t node_index = 0;
    for (const auto& n : j["flow"]) {
        if (!n.is_object() || !n.contains("type_id") || !n["type_id"].is_string()) {
            return std::unexpected("each flow node needs a string 'type_id'");
        }
        NodeSpec ns;
        ns.type_id = n["type_id"].get<std::string>();
        // id (019 §1): explicit if given, else synthesized from position — always populated so
        // `edges[]` (whether authored by hand or by a future Studio) can address any node.
        if (n.contains("id") && !n["id"].is_null()) {
            if (!n["id"].is_string()) return std::unexpected("node.id must be a string");
            ns.id = n["id"].get<std::string>();
        } else {
            ns.id = "n" + std::to_string(node_index);
        }
        if (n.contains("config") && !n["config"].is_null()) {
            if (!n["config"].is_object()) return std::unexpected("node.config must be an object");
            ns.config = n["config"];
        }
        app.flow.push_back(std::move(ns));
        ++node_index;
    }

    // edges (optional, 019 §1/§2): a real graph over the flow's node ids. Shape-checked only here
    // (every field present + a string); resolving ids against the actual node set, acyclicity, and
    // branch-label consistency is the Flow Compiler's job at deploy (009 §3), same split as `flow`
    // above — this loader never resolves type_ids or ids, only validates JSON shape.
    if (j.contains("edges") && !j["edges"].is_null()) {
        if (!j["edges"].is_array()) return std::unexpected("Application.edges must be an array");
        for (const auto& e : j["edges"]) {
            if (!e.is_object() || !e.contains("from") || !e["from"].is_string() ||
                !e.contains("to") || !e["to"].is_string()) {
                return std::unexpected("each edge needs string 'from' and 'to'");
            }
            EdgeSpec es;
            es.from = e["from"].get<std::string>();
            es.to = e["to"].get<std::string>();
            if (e.contains("from_port") && !e["from_port"].is_null()) {
                if (!e["from_port"].is_string()) return std::unexpected("edge.from_port must be a string");
                es.from_port = e["from_port"].get<std::string>();
            }
            app.edges.push_back(std::move(es));
        }
    }

    // driver (optional).
    if (j.contains("driver") && !j["driver"].is_null()) {
        const auto& d = j["driver"];
        if (!d.is_object() || !d.contains("type_id") || !d["type_id"].is_string()) {
            return std::unexpected("driver needs a string 'type_id'");
        }
        DriverSpec ds;
        ds.type_id = d["type_id"].get<std::string>();
        if (d.contains("config") && !d["config"].is_null()) {
            if (!d["config"].is_object()) return std::unexpected("driver.config must be an object");
            ds.config = d["config"];
        }
        app.driver = std::move(ds);
    }

    // persistence (optional).
    if (j.contains("persistence") && !j["persistence"].is_null()) {
        const auto& p = j["persistence"];
        if (!p.is_object()) {
            return std::unexpected("Application.persistence must be an object");
        }
        PersistenceSpec ps;
        if (p.contains("model") && p["model"].is_string()) ps.model = p["model"].get<std::string>();
        if (p.contains("mode") && p["mode"].is_string()) ps.mode = p["mode"].get<std::string>();
        app.persistence = std::move(ps);
    }

    return app;
}

}  // namespace aero::schema
