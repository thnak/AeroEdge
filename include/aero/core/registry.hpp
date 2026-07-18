// AeroEdge core — the Node & Driver registries (spec 005 §5).
//
// A declarative Application (009 §2) names each flow step and its driver by a stable `type_id`
// string; the registry is the `type_id → factory` table the Flow Compiler consults AT DEPLOY TIME
// (I3) to construct the concrete `INode`/`IDriver` from its JSON config. `process()` never consults
// the registry — all lookups happen once, here, before the actor starts.
//
// This header owns only the GENERIC table (it depends on the aero-sdk contract + JSON, nothing
// heavier — R1 layering). The registration of the BUILT-IN factories lives one layer up in
// aero-runtime (register_builtins, runtime.hpp), which is allowed to #include aero-nodes/aero-drivers;
// aero-core must not depend upward on them.
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "aero/sdk/driver.hpp"
#include "aero/sdk/node.hpp"
#include "nlohmann/json.hpp"

namespace aero {

// A factory builds one node/driver instance from its per-node config object (005 §5). Config is read
// ONCE here (ctor/configure), never per Command (N3). An empty JSON object means "defaults".
using NodeFactory = std::function<std::unique_ptr<INode>(const nlohmann::json& config)>;
using DriverFactory = std::function<std::unique_ptr<IDriver>(const nlohmann::json& config)>;

// Generic `type_id → factory` table. Not thread-safe for concurrent register/create; population
// happens once at startup (register_builtins), lookups happen at deploy — both single-threaded (I3).
template <class Factory, class Product>
class Registry {
public:
    void register_type(std::string type_id, Factory factory) {
        factories_.insert_or_assign(std::move(type_id), std::move(factory));
    }

    [[nodiscard]] bool contains(const std::string& type_id) const {
        return factories_.find(type_id) != factories_.end();
    }

    // Resolve + construct. Unknown type_id → a clear error (Phase-5 adds full DAG validation; Phase-4
    // does name resolution + config only, 009 §3).
    [[nodiscard]] std::expected<std::unique_ptr<Product>, std::string>
    create(const std::string& type_id, const nlohmann::json& config) const {
        const auto it = factories_.find(type_id);
        if (it == factories_.end()) {
            return std::unexpected("unknown type_id: '" + type_id + "'");
        }
        return it->second(config);
    }

private:
    std::unordered_map<std::string, Factory> factories_;
};

using NodeRegistry = Registry<NodeFactory, INode>;
using DriverRegistry = Registry<DriverFactory, IDriver>;

}  // namespace aero
