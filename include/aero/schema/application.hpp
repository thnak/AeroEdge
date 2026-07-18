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

// One ordered step of the linear flow pipeline (009 §2): a node type_id + its opaque config object.
// The config is passed verbatim to the node factory (005 §5); the schema does not interpret it.
struct NodeSpec {
    std::string type_id;
    nlohmann::json config = nlohmann::json::object();
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
    std::vector<NodeSpec> flow;  // ordered, the linear pipeline (Phase-4; branch/fan-out is Phase-5)
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
    for (const auto& n : j["flow"]) {
        if (!n.is_object() || !n.contains("type_id") || !n["type_id"].is_string()) {
            return std::unexpected("each flow node needs a string 'type_id'");
        }
        NodeSpec ns;
        ns.type_id = n["type_id"].get<std::string>();
        if (n.contains("config") && !n["config"].is_null()) {
            if (!n["config"].is_object()) return std::unexpected("node.config must be an object");
            ns.config = n["config"];
        }
        app.flow.push_back(std::move(ns));
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
