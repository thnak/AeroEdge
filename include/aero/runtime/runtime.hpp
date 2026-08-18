// AeroEdge runtime — the RuntimeController (spec 009, 013 §2 aero-runtime).
//
// The Runtime is the edge daemon's core, testable IN-PROCESS with no socket: it takes a declarative
// Application (009 §2), COMPILES it at deploy (009 §3) by resolving each node/driver from the registry
// (005 §5), brings up a Quark engine hosting one FlowActor bound to the CompiledFlow, and — if a driver
// is configured — runs the Phase-2 ingestion path (GeneratorDriver → Quark 024 StreamChannel → a
// bridge that `tell`s each frame into the actor). It owns every lifetime and tears them down on
// undeploy/stop. All control logic lives HERE; aero-api is a thin HTTP shell over this (013 T2).
//
// THIN-OVER-QUARK (R0): the Runtime writes no scheduler/mailbox/stream — bring-up is the verified
// sample-01 shape (MessagePool → Activation → Engine → register_actor → LocalRouter), and ingestion
// reuses the proven 024 StreamChannel. The one thing AeroEdge adds is the compile+wire+bridge glue.
//
// INGESTION BRIDGE (why a bridge thread, not direct drive): the driver produces frames on its own I/O
// lane (D6). Feeding them to the actor by `tell` — rather than calling the actor directly from the
// drain thread — keeps the actor SINGLE-EXECUTOR (I2): only the engine worker ever touches actor
// state, so status `ask`s and frame Commands serialize through the mailbox with no data race (the
// mailbox is Quark's Vyukov MPSC — many producers may enqueue). The StreamChannel still carries the
// lossless credit backpressure between the driver and the bridge (006 §3). This is the honest Phase-4
// path until Quark routes a stream descriptor through the worker loop itself (024 seam, see 006).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <optional>

#include "aero/broker/broker_cluster.hpp"
#include "aero/broker/native_broker.hpp"
#include "aero/cluster/cluster.hpp"
#include "aero/core/compiled_flow.hpp"
#include "aero/core/registry.hpp"
#include "aero/drivers/generator_driver.hpp"
#include "aero/drivers/modbus_rtu_driver.hpp"
#include "aero/drivers/modbus_tcp_driver.hpp"
#include "aero/drivers/opcua_driver.hpp"
#include "aero/drivers/opcua_subscription_driver.hpp"
#include "aero/egress/http_egress_actor.hpp"
#include "aero/ext/native_loader.hpp"
#include "aero/mes/mes.hpp"
#include "aero/mes/outbox.hpp"
#include "aero/mes/rest_mes_adapter.hpp"
#include "aero/ota/fleet.hpp"
#include "aero/ota/ota.hpp"
#include "aero/nodes/builtin_nodes.hpp"
#include "aero/nodes/compute_nodes.hpp"
#include "aero/nodes/expr_rule_node.hpp"
#include "aero/nodes/http_output_node.hpp"
#include "aero/nodes/mes_nodes.hpp"
#include "aero/nodes/set_node.hpp"
#include "aero/nodes/switch_node.hpp"
#include "aero/runtime/flow_actor.hpp"
#include "aero/runtime/flow_compiler.hpp"
#include "aero/schema/application.hpp"
#include "aero/sdk/driver.hpp"
#include "aero/sdk/node.hpp"
#include "nlohmann/json.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/activation.hpp"
#include "quark/core/cluster.hpp"  // SwimMembership (021) — configure_fleet()'s optional real membership
#include "quark/core/engine.hpp"
#include "quark/core/engine_config.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/spawn.hpp"
#include "quark/core/stream_activation.hpp"

namespace aero::runtime {

// M9.4 (018 §8): reads an optional nested "security" JSON object into an OpcUaSecurityConfig — shared by
// both aero.driver.opcua and aero.driver.opcua_subscribe's factories below. Absent object == every field
// stays default-constructed == disabled (opcua_security.hpp's own "certificate_file.empty()" gate).
inline aero::drivers::OpcUaSecurityConfig parse_opcua_security(const nlohmann::json& c) {
    aero::drivers::OpcUaSecurityConfig sec;
    const auto it = c.find("security");
    if (it == c.end() || !it->is_object()) return sec;
    sec.certificate_file = it->value("certificate_file", std::string{});
    sec.private_key_file = it->value("private_key_file", std::string{});
    sec.trusted_server_certificate_file = it->value("trusted_server_certificate_file", std::string{});
    sec.sign_and_encrypt = it->value("sign_and_encrypt", true);
    sec.security_policy_uri = it->value("security_policy_uri", std::string{});
    return sec;
}

// Populate the registries with the Phase-4 built-in node/driver factories (005 §5). Lives here (not in
// aero-core/registry.hpp) because it #includes aero-nodes/aero-drivers — the one-way layering (R1)
// forbids aero-core depending upward on them.
inline void register_builtins(NodeRegistry& node_reg, DriverRegistry& driver_reg) {
    node_reg.register_type("aero.source.decode", aero::nodes::DecodeSourceNode::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::DecodeSourceNode>();
    });
    node_reg.register_type("aero.transform.scale", aero::nodes::ScaleNode::kDesc,
        [](const nlohmann::json& c) {
        return std::make_unique<aero::nodes::ScaleNode>(c.value("factor", 1.0));
    });
    node_reg.register_type("aero.transform.moving_average", aero::nodes::RuntimeMovingAverageNode::kDesc,
        [](const nlohmann::json& c) {
        return std::make_unique<aero::nodes::RuntimeMovingAverageNode>(c.value("window", std::size_t{1}));
    });
    node_reg.register_type("aero.output.sum", aero::nodes::SumOutputNode::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::SumOutputNode>();
    });
    // Low-code Rule DSL (008 §6): parse the expression ONCE here (deploy), 0-alloc eval per Command.
    // A malformed 'expr' is rejected earlier by the flow compiler (validate_node_config); this factory
    // parses again and, defensively, a bad program yields a node whose process() returns Error.
    node_reg.register_type("aero.rule.expr", aero::nodes::ExprRuleNode::kDesc,
        [](const nlohmann::json& c) -> std::unique_ptr<INode> {
        auto prog = aero::nodes::ExprRuleNode::compile(c.value("expr", std::string{}));
        return std::make_unique<aero::nodes::ExprRuleNode>(
            std::move(prog), c.value("alarm", std::string{"AlarmRaised"}));
    });

    // Phase-10 compute-node breadth (005 §2): pure, socket-free transforms/sources (compute_nodes.hpp).
    node_reg.register_type("aero.transform.mean", aero::nodes::MeanNode::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::MeanNode>();
    });
    node_reg.register_type("aero.transform.minmax", aero::nodes::MinMaxNode::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::MinMaxNode>();
    });
    node_reg.register_type("aero.transform.sum", aero::nodes::SumNode::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::SumNode>();
    });
    node_reg.register_type("aero.transform.crc", aero::nodes::CrcNode::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::CrcNode>();
    });
    // Modbus register-map DECODE over already-arrived bytes (no socket; the Modbus-TCP transport is gated).
    node_reg.register_type("aero.source.modbus", aero::nodes::ModbusDecodeNode::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::ModbusDecodeNode>();
    });
    // Modbus coil/discrete-input (bit-packed) DECODE — the FC01/FC02 counterpart, M9.1 PR D.
    node_reg.register_type("aero.source.modbus_bits", aero::nodes::ModbusBitsDecodeNode::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::ModbusBitsDecodeNode>();
    });
    node_reg.register_type("aero.source.json", aero::nodes::JsonParseNode::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::JsonParseNode>();
    });

    // Phase-10 MES hook (012 §4): the outbound report Output node + the inbound order Source node.
    node_reg.register_type("aero.output.mes", aero::nodes::MesReportNode::kDesc,
        [](const nlohmann::json& c) {
        auto kind = aero::StagedMesReport::Kind::Production;
        const std::string k = c.value("kind", std::string{"production"});
        if (k == "alarm") kind = aero::StagedMesReport::Kind::Alarm;
        else if (k == "tag_sample") kind = aero::StagedMesReport::Kind::TagSample;
        return std::make_unique<aero::nodes::MesReportNode>(
            c.value("line", std::string{"line-1"}), c.value("label", std::string{"produced"}), kind);
    });
    node_reg.register_type("aero.source.mes_order", aero::nodes::MesOrderSourceNode::kDesc,
        [](const nlohmann::json& c) {
        return std::make_unique<aero::nodes::MesOrderSourceNode>(c.value("order_qty", 0.0));
    });

    // 019 §5: the graph-model router (see nodes/switch_node.hpp's banner — the only utility node that
    // needed new runtime machinery; fan-out/merge are free once edges[] exists). Parses ONCE here
    // (deploy), mirroring aero.rule.expr's factory just above — 0-alloc eval per Command.
    node_reg.register_type("aero.flow.switch", aero::nodes::SwitchNode::kDesc,
        [](const nlohmann::json& c) -> std::unique_ptr<INode> {
        auto prog = aero::nodes::SwitchNode::compile(c.value("expr", std::string{}));
        return std::make_unique<aero::nodes::SwitchNode>(std::move(prog));
    });

    // 020 §7: aero.transform.set — write a DSL expression's result into a working-set tag. Parses ONCE
    // here (deploy), reusing ExprRuleNode::compile — the same pattern aero.flow.switch's factory above
    // already established, a third reuse of the same evaluator.
    node_reg.register_type("aero.transform.set", aero::nodes::SetNode::kDesc,
        [](const nlohmann::json& c) -> std::unique_ptr<INode> {
        auto prog = aero::nodes::SetNode::compile(c.value("expr", std::string{}));
        return std::make_unique<aero::nodes::SetNode>(std::move(prog), c.value("tag", std::string{}));
    });

    // 019 slice: generic HTTP output (see nodes/http_output_node.hpp's banner for scope/non-durability).
    node_reg.register_type("aero.output.http", aero::nodes::HttpOutputNode::kDesc,
        [](const nlohmann::json& c) {
        return std::make_unique<aero::nodes::HttpOutputNode>(
            c.value("url", std::string{}), c.value("method", std::string{"POST"}),
            c.contains("headers") && c["headers"].is_object() ? c["headers"].dump() : std::string{"{}"},
            c.value("timeout_ms", 2000));
    });

    driver_reg.register_type("aero.driver.generator", aero::drivers::GeneratorDriver::kDesc,
        [](const nlohmann::json&) {
        return std::make_unique<aero::drivers::GeneratorDriver>();
    });
    // M9a (018 §Multi-protocol southbound): a real Modbus-TCP PULL driver. Config is read straight out
    // of the deploy-time JSON at construction (not routed through DriverConfig's narrow endpoint/
    // frame_count/rate_hz fields, per this driver's explicit constraint — see modbus_tcp_driver.hpp).
    // "register_type": "holding" (default, FC03) | "input" (FC04) | "coils" (FC01) |
    // "discrete_inputs" (FC02) — M9.1 PR B/PR D. For "coils"/"discrete_inputs", "register_count" means
    // coil/discrete-input count, not register count (pair with "aero.source.modbus_bits", not
    // "aero.source.modbus", downstream).
    driver_reg.register_type("aero.driver.modbus_tcp", aero::drivers::ModbusTcpDriver::kDesc,
        [](const nlohmann::json& c) {
        using RF = aero::drivers::ModbusTcpDriver::ReadFunction;
        auto read_fn = RF::HoldingRegisters;
        const std::string rt = c.value("register_type", std::string{"holding"});
        if (rt == "input") read_fn = RF::InputRegisters;
        else if (rt == "coils") read_fn = RF::Coils;
        else if (rt == "discrete_inputs") read_fn = RF::DiscreteInputs;
        return std::make_unique<aero::drivers::ModbusTcpDriver>(
            c.value("host", std::string{}), c.value("port", std::uint16_t{502}),
            c.value("unit_id", std::uint8_t{1}), c.value("start_address", std::uint16_t{0}),
            c.value("register_count", std::uint16_t{8}), read_fn);
    });
    // M9b (018 §Multi-protocol southbound): a real OPC-UA client PULL driver (open62541-backed). Config
    // is read straight out of the deploy-time JSON at construction, same reasoning as
    // aero.driver.modbus_tcp above (DriverConfig's narrow fields don't fit this driver's shape either).
    // M9.4 (018 §8): an optional nested "security" object opts into Sign/SignAndEncrypt over a client
    // cert (opcua_security.hpp) — absent/empty object == disabled (MessageSecurityMode::None), matching
    // every deploy config written before M9.4. Cert/key material is loaded from DER FILE PATHS (not
    // inline bytes in this JSON — see opcua_security.hpp's banner for why).
    driver_reg.register_type("aero.driver.opcua", aero::drivers::OpcUaDriver::kDesc,
        [](const nlohmann::json& c) {
        return std::make_unique<aero::drivers::OpcUaDriver>(
            c.value("endpoint", std::string{}),
            c.value("node_ids", std::vector<std::string>{}),
            c.value("browse_root", std::string{}),
            parse_opcua_security(c));
    });
    // M9.3 (018 §8): OPC-UA Subscriptions/MonitoredItems — the PUSH counterpart to aero.driver.opcua
    // above, a SEPARATE type_id/class (opcua_subscription_driver.hpp's own banner explains why: push vs
    // pull is a different IDriver invocation contract, not a mode flag on one driver).
    driver_reg.register_type("aero.driver.opcua_subscribe", aero::drivers::OpcUaSubscriptionDriver::kDesc,
        [](const nlohmann::json& c) {
        return std::make_unique<aero::drivers::OpcUaSubscriptionDriver>(
            c.value("endpoint", std::string{}), c.value("node_ids", std::vector<std::string>{}),
            parse_opcua_security(c));
    });
    // M9.1 PR H (018 §8): Modbus RTU/serial counterpart to aero.driver.modbus_tcp above — same
    // "register_type" selector and defaults, plus serial-specific fields (port name, baud, parity,
    // stop_bits, slave_address in place of host/port/unit_id).
    driver_reg.register_type("aero.driver.modbus_rtu", aero::drivers::ModbusRtuDriver::kDesc,
        [](const nlohmann::json& c) {
        using RF = aero::drivers::ModbusRtuDriver::ReadFunction;
        auto read_fn = RF::HoldingRegisters;
        const std::string rt = c.value("register_type", std::string{"holding"});
        if (rt == "input") read_fn = RF::InputRegisters;
        else if (rt == "coils") read_fn = RF::Coils;
        else if (rt == "discrete_inputs") read_fn = RF::DiscreteInputs;
        const std::string parity_s = c.value("parity", std::string{"N"});
        const char parity = parity_s.empty() ? 'N' : parity_s[0];
        return std::make_unique<aero::drivers::ModbusRtuDriver>(
            c.value("port", std::string{}), c.value("baud_rate", std::uint32_t{9600}),
            c.value("slave_address", std::uint8_t{1}), c.value("start_address", std::uint16_t{0}),
            c.value("register_count", std::uint16_t{8}), read_fn, parity,
            c.value("stop_bits", std::uint8_t{1}));
    });
}

// Enumerate the registries into the `GET /catalog` shape (015 U1) — the single source of truth for what
// the Studio's node/driver picker offers, generated from the same descriptors the registry itself uses
// (013 T3: Studio and runtime cannot drift). Free function (not a Runtime method) so it only needs the
// registries, not a live Runtime instance — usable straight after register_builtins().
inline nlohmann::json build_catalog(const NodeRegistry& node_reg, const DriverRegistry& driver_reg) {
    auto field_json = [](const FieldSpec& f) {
        nlohmann::json j;
        j["key"] = f.key;
        j["label"] = f.label;
        switch (f.type) {
            case FieldType::Number: j["type"] = "number"; break;
            case FieldType::Int: j["type"] = "int"; break;
            case FieldType::String: j["type"] = "string"; break;
            case FieldType::Bool: j["type"] = "boolean"; break;
            case FieldType::Enum: j["type"] = "enum"; break;
            case FieldType::StringArray: j["type"] = "string_array"; break;
            case FieldType::Object: j["type"] = "object"; break;
        }
        j["required"] = f.required;
        if (f.type == FieldType::Number || f.type == FieldType::Int) j["default"] = f.default_number;
        else if (f.type == FieldType::Bool) j["default"] = f.default_bool;
        else if (!f.default_string.empty()) j["default"] = f.default_string;
        if (f.has_min) j["min"] = f.min;
        if (!f.help.empty()) j["help"] = f.help;
        if (!f.tier2_hint.empty()) j["tier2"] = f.tier2_hint;
        if (!f.enum_options.empty()) {
            j["options"] = nlohmann::json::array();
            for (const auto& o : f.enum_options) j["options"].push_back(o);
        }
        return j;
    };

    nlohmann::json out;
    out["nodes"] = nlohmann::json::array();
    node_reg.for_each([&](const std::string& type_id, const NodeDescriptor& d) {
        nlohmann::json j;
        j["type_id"] = type_id;
        switch (d.category) {
            case NodeCategory::Source: j["category"] = "Source"; break;
            case NodeCategory::Transform: j["category"] = "Transform"; break;
            case NodeCategory::Rule: j["category"] = "Rule"; break;
            case NodeCategory::Output: j["category"] = "Output"; break;
        }
        j["terminal"] = d.terminal;  // 020 §4.3: Cap (nothing may follow) vs. Stack shape
        j["fields"] = nlohmann::json::array();
        for (const auto& f : d.config_fields) j["fields"].push_back(field_json(f));
        out["nodes"].push_back(std::move(j));
    });
    out["drivers"] = nlohmann::json::array();
    driver_reg.for_each([&](const std::string& type_id, const DriverDescriptor& d) {
        nlohmann::json j;
        j["type_id"] = type_id;
        j["writable"] = d.writable;
        j["poll_driven"] = d.poll_driven;
        j["fields"] = nlohmann::json::array();
        for (const auto& f : d.config_fields) j["fields"].push_back(field_json(f));
        out["drivers"].push_back(std::move(j));
    });
    return out;
}

class Runtime {
public:
    Runtime() { register_builtins(nodes_, drivers_); }
    ~Runtime() {
        (void)undeploy();
        if (mes_engine_) mes_engine_->stop();
        if (http_egress_engine_) http_egress_engine_->stop();
        // Order matters (017 M6): stop the broker's accept loop + every session thread BEFORE the cluster
        // — that guarantees no locally-originated PUBLISH can still be mid-flight into
        // NativeBroker::deliver_publish() (and so into peer_forwarder_) once broker_cluster_ starts tearing
        // down its own engine/transport underneath it. Both stop()s are idempotent (harmless if either
        // subsystem was never configured / already stopped).
        if (broker_) broker_->stop();
        if (broker_cluster_) broker_cluster_->stop();
        // Real cluster membership (010 §5 follow-up): stop the tick thread BEFORE the transport it
        // drives SwimMembership over — swim_tick_running_ false + join() guarantees no further tick()/
        // refresh() call can start touching cluster_transport_/cluster_/swim_ once this returns, so
        // tearing those down after is race-free (mirrors BrokerCluster::stop()'s own ordering rationale).
        if (swim_tick_thread_.joinable()) {
            swim_tick_running_.store(false, std::memory_order_release);
            swim_tick_thread_.join();
        }
        if (cluster_transport_) cluster_transport_->stop();
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Load a NATIVE extension bundle (008 §2): dlopen the `.so`, ABI-check it (E6), and register a
    // factory per provided node into this Runtime's NodeRegistry — after which its type_ids resolve in
    // deploy() exactly like a built-in (E1). Must be called BEFORE deploy() references those type_ids.
    // The loaded library stays resident (ref-counted by the registered factories) until the Runtime is
    // destroyed; a version change is BuildOnly (drain + redeploy, 009 §4). Errors come back as values.
    std::expected<void, std::string> load_native_extension(const std::string& path) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (dep_) {
            return std::unexpected("load extensions before deploy (undeploy '" + dep_->name + "' first)");
        }
        auto ext = aero::ext::register_native_extension(nodes_, path);
        if (!ext) return std::unexpected(ext.error());
        return {};
    }

    // Deploy a parsed Application: compile the flow from the registry (009 §3), bring up the engine +
    // FlowActor, and start the driver ingestion path if one is configured. One Application per Runtime.
    std::expected<void, std::string> deploy(const schema::Application& app) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (dep_) {
            return std::unexpected("a runtime hosts one Application; undeploy '" + dep_->name + "' first");
        }

        // --- Validate + compile the flow BEFORE any engine is brought up (009 §3 P1). A bad
        // Application is rejected here as a value — never a crash, never a half-deploy. ------------
        auto compiled = compile_flow(app, nodes_);
        if (!compiled) {
            return std::unexpected(compiled.error());
        }

        auto d = std::make_unique<Deployment>();
        d->app = app;
        d->name = app.name;
        d->version = app.version;
        d->key = app.actor.key;
        // Heap-hold the plan so `plan->flow`'s address is stable while the actor holds a
        // `const CompiledFlow*` (ADR-008 Hot-Leaf); a hot-reload swaps this pointer wholesale (§4).
        d->plan = std::make_unique<CompiledPlan>(std::move(*compiled));

        // --- Bring up the engine hosting one FlowActor (verified sample-01 shape, R4). --------------
        d->actor = std::make_unique<FlowActor>();
        d->actor->bind_flow(d->plan->flow);  // wire before start(), so the flow is live on Command #1
        d->pool = std::make_unique<quark::detail::MessagePool>(1024);
        d->activation = std::make_unique<quark::Activation>(d->actor.get(), FlowActor::dispatch_table(),
                                                            d->pool->sink());
        d->engine = std::make_unique<quark::Engine<>>(quark::EngineConfig{/*workers*/ 1, /*shards*/ 1,
                                                                          /*budget*/ 64, 64});
        quark::register_actor<FlowActor>(*d->engine, d->key, *d->activation);
        d->router = std::make_unique<quark::LocalRouter>(d->engine->post_courier(), *d->pool);
        d->engine->start();

        // --- Driver ingestion path (optional): GeneratorDriver → 024 StreamChannel → bridge (006). --
        if (app.driver) {
            auto drv = drivers_.create(app.driver->type_id, app.driver->config);
            if (!drv) {
                d->engine->stop();
                return std::unexpected("driver: " + drv.error());
            }
            d->driver = std::move(*drv);

            aero::DriverConfig dcfg;
            dcfg.endpoint = "generator://seq";  // string literal — static storage, view-safe
            dcfg.frame_count = app.driver->config.value("frame_count", std::uint32_t{0});
            dcfg.rate_hz = app.driver->config.value("rate_hz", std::uint32_t{0});
            if (d->driver->open(dcfg) != aero::DriverStatus::Ok) {
                d->engine->stop();
                return std::unexpected("driver.open failed for '" + app.driver->type_id + "'");
            }

            d->has_driver = true;

            if (d->driver->descriptor().poll_driven) {
                // --- PULL driver lane (006 §6.1, M9.2): a "Command/Timer -> driver.poll(sink)" loop ---
                // that spec 006 documented but no code ever implemented — poll() was previously only
                // ever called from each driver's own test harness (ModbusTcpDriver/OpcUaDriver/
                // ModbusRtuDriver, all of spec 018, could NOT actually be deployed via a real
                // Application before this). `poll(StreamSink<Frame>)`'s by-value contract consumes its
                // sink after ONE call (a StreamActivation's single-producer bind is a lifetime
                // commitment — no way to hand the token back for reuse), so unlike the push lane below
                // there is no persistent d->stream/producer/bridge pair: EVERY tick stands up a small
                // throwaway StreamActivation (mirrors the exact pattern this driver family's own tests
                // already use), calls poll() once, and drains whatever frame(s) that single call
                // produced straight into the actor — synchronously, on this one poller thread. No
                // separate bridge thread is needed because one poll() is a bounded step, not an
                // independent streaming producer.
                //
                // CADENCE: for a PULL driver, `rate_hz` stops being merely advisory (as it is for push
                // drivers, which ignore it) and becomes the actual timer period — 0 falls back to
                // kDefaultPollIntervalMs (1 Hz), never "poll as fast as possible" against a real device.
                constexpr std::uint32_t kDefaultPollIntervalMs = 1000;
                const std::uint32_t rate_hz = dcfg.rate_hz;
                const auto interval = std::chrono::milliseconds(
                    rate_hz > 0 ? (1000 / rate_hz) : kDefaultPollIntervalMs);

                Deployment* dp = d.get();
                d->poller = std::thread([dp, interval]() {
                    auto ref = dp->router->get<FlowActor>(dp->key);
                    while (!dp->stop_flag.load(std::memory_order_acquire)) {
                        quark::StreamActivation<aero::Frame>::Config scfg;
                        scfg.capacity = 4;  // one poll() call yields at most a handful of frames
                        std::pmr::monotonic_buffer_resource mr;
                        quark::StreamActivation<aero::Frame> act(scfg, &mr);
                        auto tok = quark::open_stream(act);
                        if (tok) {
                            aero::StreamSink<aero::Frame> sink(std::move(tok.value()));
                            if (dp->driver->poll(std::move(sink)) == aero::DriverStatus::Ok) {
                                auto& ch = act.channel();
                                while (ch.occupancy() > 0) {
                                    quark::StreamBatch<aero::Frame> batch(ch, /*budget*/ 4);
                                    while (const aero::Frame* f = batch.next()) {
                                        ref.tell(ReceiveFrame{f->raw, f->payload_len, f->payload});
                                        batch.retire();
                                    }
                                }
                            }
                            // A non-Ok poll() (device unreachable, mid-backoff, ...) is not fatal —
                            // the driver's own bounded-backoff reconnect (006 §8) handles it; this loop
                            // just tries again next tick.
                        }

                        // Sleep in short slices so stop_flag is observed promptly (006 §8 graceful
                        // stop), not one long sleep_for(interval).
                        const auto deadline = std::chrono::steady_clock::now() + interval;
                        while (std::chrono::steady_clock::now() < deadline) {
                            if (dp->stop_flag.load(std::memory_order_acquire)) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                    }
                });
            } else {
                // --- PUSH driver lane (006 §6.2), unchanged: a persistent stream + producer + bridge. --
                quark::StreamActivation<aero::Frame>::Config scfg;
                scfg.capacity = 256;  // ring == max credit == max in-flight frames (006 §3)
                d->mr = std::make_unique<std::pmr::monotonic_buffer_resource>();
                d->stream = std::make_unique<quark::StreamActivation<aero::Frame>>(scfg, d->mr.get());
                auto tok = quark::open_stream(*d->stream);  // single-writer token (024, D1)
                if (!tok) {
                    d->engine->stop();
                    return std::unexpected("open_stream failed");
                }
                aero::StreamSink<aero::Frame> sink(std::move(tok.value()));

                // Producer lane: the driver's run loop pushes frames honoring backpressure (D6).
                d->producer = std::thread(
                    [drv = d->driver.get(), sink = std::move(sink), flag = &d->stop_flag,
                     done = &d->producer_done]() mutable {
                        drv->run(std::move(sink), aero::StopToken{flag});
                        done->store(true, std::memory_order_release);
                    });

                // Bridge lane: drain the stream and `tell` each frame into the actor (single-executor, I2).
                Deployment* dp = d.get();
                d->bridge = std::thread([dp]() {
                    auto& ch = dp->stream->channel();
                    auto ref = dp->router->get<FlowActor>(dp->key);
                    for (;;) {
                        const bool producer_done = dp->producer_done.load(std::memory_order_acquire);
                        while (ch.occupancy() > 0) {
                            quark::StreamBatch<aero::Frame> batch(ch, /*budget*/ 64);
                            while (const aero::Frame* f = batch.next()) {
                                // copy raw + byte payload out of the pinned slot (006 §4)
                                ref.tell(ReceiveFrame{f->raw, f->payload_len, f->payload});
                                batch.retire();  // return credit after the tell is enqueued
                            }
                        }
                        if (dp->stop_flag.load(std::memory_order_acquire)) break;
                        if (producer_done && ch.occupancy() == 0) break;  // bounded driver finished+drained
                        std::this_thread::yield();  // no sleep; progress bounded by frame_count / stop
                    }
                });
            }
        }

        dep_ = std::move(d);
        return {};
    }

    // Parse JSON text then deploy (the API/daemon entry). Bad JSON → a clean error, never a throw.
    std::expected<void, std::string> deploy_json(const std::string& json_text) {
        auto app = schema::load_application(json_text);
        if (!app) {
            return std::unexpected(app.error());
        }
        return deploy(*app);
    }

    // Hot-reload the running Application to a NEW one (009 §4/§6). The change is classified Live vs
    // BuildOnly (009 §4, P3): a BuildOnly change (actor kind/key or persistence model/mode) cannot be
    // a live pointer-swap and is REJECTED with a clear error (never half-applied); a Live change (flow
    // graph / node config) is validated + compiled off to the side while the old flow keeps running,
    // then swapped in via a mailbox-ordered ReloadFlow Command (P2 — 0 dropped/duplicated Commands).
    std::expected<void, std::string> reload(const schema::Application& app) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!dep_) {
            return std::unexpected("nothing deployed; deploy an Application before reload");
        }
        if (auto reason = classify_buildonly(dep_->app, app)) {
            return std::unexpected("BuildOnly change requires redeploy (undeploy first): " + *reason);
        }
        // Validate + compile the new flow off to the side (009 §4 step 1). If it fails the OLD flow is
        // untouched and keeps running — an invalid reload never reaches the actor (P1).
        auto compiled = compile_flow(app, nodes_);
        if (!compiled) {
            return std::unexpected(compiled.error());
        }
        auto next = std::make_unique<CompiledPlan>(std::move(*compiled));

        // Publish the swap (009 §4 step 3): tell a ReloadFlow carrying the new flow pointer. Mailbox
        // FIFO on a Sequential actor puts it AFTER all in-flight frames (they finish on the old flow)
        // and BEFORE all later frames (they run the new flow) — the Hot-Leaf pointer publish (ADR-008).
        auto ref = dep_->router->get<FlowActor>(dep_->key);
        ref.tell(ReloadFlow{&next->flow});

        // Retire the old plan (009 §4 step 4) only after NO execution can reference it. A status ask is
        // FIFO after the ReloadFlow on the Sequential actor, so when it returns the swap is applied and
        // every old-flow frame has completed — the old plan is then unreferenced and safe to destroy.
        (void)quark::block_on(ref.ask<FlowStatus>(GetStatus{}));

        previous_app_ = dep_->app;       // keep the prior version for rollback (009 §6)
        dep_->app = app;
        dep_->name = app.name;
        dep_->version = app.version;
        dep_->plan = std::move(next);    // old plan destroyed here — provably unreferenced (above)
        return {};
    }

    // Parse JSON then reload (the API/daemon entry). Bad JSON → a clean error, never a throw.
    std::expected<void, std::string> reload_json(const std::string& json_text) {
        auto app = schema::load_application(json_text);
        if (!app) {
            return std::unexpected(app.error());
        }
        return reload(*app);
    }

    // Rollback to the previously-deployed Application version (009 §6): a hot-reload back to the prior
    // flow. Requires a prior version (i.e. at least one successful reload). Same Live/BuildOnly rules.
    std::expected<void, std::string> rollback() {
        std::optional<schema::Application> prev;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!dep_) return std::unexpected("nothing deployed");
            if (!previous_app_) return std::unexpected("no previous version to roll back to");
            prev = *previous_app_;
        }
        return reload(*prev);  // re-locks; classify + validate + swap the prior flow back in
    }

    // Test helper: tell one frame Command directly to the deployed actor (no driver). Lets a test drive
    // an exact, deterministic send/reload interleave to prove the hot-reload ordering (009 §4).
    std::expected<void, std::string> tell_frame(std::int64_t raw) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!dep_) return std::unexpected("nothing deployed");
        dep_->router->get<FlowActor>(dep_->key).tell(ReceiveFrame{raw});
        return {};
    }

    // Status snapshot (009 §6 observability): the deployed app + the actor's live counters, read via a
    // single ask (FIFO after all prior frame tells on a Sequential actor).
    nlohmann::json status() {
        std::lock_guard<std::mutex> lock(mtx_);
        nlohmann::json j;
        if (!dep_) {
            j["deployed"] = false;
            return j;
        }
        auto ref = dep_->router->get<FlowActor>(dep_->key);
        auto r = quark::block_on(ref.ask<FlowStatus>(GetStatus{}));
        const FlowStatus s = r.has_value() ? r.value() : FlowStatus{};
        j["deployed"] = true;
        j["name"] = dep_->name;
        j["version"] = dep_->version;
        j["actor_key"] = static_cast<std::uint64_t>(dep_->key);
        j["actor_kind"] = "edge";
        j["has_driver"] = dep_->has_driver;
        j["frames_processed"] = s.frames;
        j["events_published"] = s.events;
        j["last_output"] = s.last;
        j["output_sum"] = s.output_sum;
        j["reloads"] = s.reloads;
        j["failed"] = s.failed;
        return j;
    }

    // The deployed Applications (0 or 1 in Phase-4) — name + version each.
    nlohmann::json list() {
        std::lock_guard<std::mutex> lock(mtx_);
        nlohmann::json arr = nlohmann::json::array();
        if (dep_) {
            nlohmann::json a;
            a["name"] = dep_->name;
            a["version"] = dep_->version;
            arr.push_back(std::move(a));
        }
        return arr;
    }

    // The node/driver catalog (015 U1, 016 §2-style additive route): every registered type_id's config
    // schema, straight from the registries `register_builtins()` populated — never a hand-maintained
    // Studio-side list. No lock needed: nodes_/drivers_ are populated once in the ctor and never mutated
    // after (deploy only reads them via create()).
    nlohmann::json catalog() { return build_catalog(nodes_, drivers_); }

    // Undeploy (by name; empty name == the current deployment). Stops the driver + engine and joins all
    // threads before destroying anything (ordered teardown).
    std::expected<void, std::string> undeploy(const std::string& name = "") {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!dep_) {
            return name.empty() ? std::expected<void, std::string>{} : std::unexpected("nothing deployed");
        }
        if (!name.empty() && name != dep_->name) {
            return std::unexpected("no such app: '" + name + "'");
        }
        teardown(*dep_);
        dep_.reset();
        return {};
    }

    [[nodiscard]] bool deployed() {
        std::lock_guard<std::mutex> lock(mtx_);
        return dep_ != nullptr;
    }

    // Test helper: block until a BOUNDED driver has produced all its frames and the bridge has `tell`ed
    // every one into the mailbox. After this returns, a status ask observes the full frame count
    // (mailbox FIFO on a Sequential actor). No-op when there is no driver. NOT for unbounded drivers.
    void await_driver_drain() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!dep_) return;
        if (dep_->producer.joinable()) dep_->producer.join();
        if (dep_->bridge.joinable()) dep_->bridge.join();
    }

    // ---- MES gateway (012 §3, 016 §2.3) ------------------------------------------------------------
    // The gateway is a daemon-lifetime subsystem, independent of any deployed Application's
    // deploy/undeploy cycle (an MES outage/outbox must survive a flow redeploy) — its own engine, not
    // Deployment's. Configure once at daemon start; a second call is rejected rather than silently
    // replacing the outbox store underneath any in-flight drain.
    using MesGateway = aero::mes::MesGatewayActor<quark::InMemoryStore>;
    static constexpr quark::ActorId kMesOutboxId{quark::TypeKey{0x0B08}, 1};

    std::expected<void, std::string> configure_mes(const aero::mes::MesConfig& cfg) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (mes_engine_) {
            return std::unexpected("MES gateway already configured");
        }
        mes_adapter_ = std::make_unique<aero::mes::RestMesAdapter>();
        mes_adapter_->connect(cfg);
        mes_store_ = std::make_unique<quark::InMemoryStore>();
        mes_gateway_ = std::make_unique<MesGateway>(*mes_adapter_, *mes_store_, kMesOutboxId);
        mes_pool_ = std::make_unique<quark::detail::MessagePool>(1024);
        mes_activation_ = std::make_unique<quark::Activation>(mes_gateway_.get(), MesGateway::dispatch_table(),
                                                               mes_pool_->sink());
        mes_engine_ = std::make_unique<quark::Engine<>>(quark::EngineConfig{/*workers*/ 1, /*shards*/ 1,
                                                                             /*budget*/ 64, 64});
        quark::register_actor<MesGateway>(*mes_engine_, /*key*/ 1, *mes_activation_);
        mes_router_ = std::make_unique<quark::LocalRouter>(mes_engine_->post_courier(), *mes_pool_);
        mes_engine_->start();
        return {};
    }

    // Stage a canonical report through the gateway (M2 hand-off point). Public so a flow-actor
    // integration or a test can drive it without reaching into the gateway internals.
    std::expected<void, std::string> mes_stage(const aero::mes::MesReport& r) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!mes_engine_) return std::unexpected("MES gateway not configured");
        mes_router_->get<MesGateway>(1).tell(aero::mes::StageReport{r});
        return {};
    }

    // Re-attempt a stuck drain after the MES recovers (M3).
    std::expected<void, std::string> mes_drain() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!mes_engine_) return std::unexpected("MES gateway not configured");
        mes_router_->get<MesGateway>(1).tell(aero::mes::DrainOutbox{});
        return {};
    }

    // Outbox observability (016 §2.3): {"configured": false} when configure_mes() was never called.
    nlohmann::json mes_outbox_stats() {
        std::lock_guard<std::mutex> lock(mtx_);
        nlohmann::json j;
        if (!mes_engine_) {
            j["configured"] = false;
            return j;
        }
        auto ref = mes_router_->get<MesGateway>(1);
        auto r = quark::block_on(ref.template ask<aero::mes::OutboxStats>(aero::mes::GetOutboxStats{}));
        const aero::mes::OutboxStats s = r.value_or(aero::mes::OutboxStats{});
        j["configured"] = true;
        j["staged"] = s.staged;
        j["pending"] = s.pending;
        j["delivered"] = s.delivered;
        return j;
    }

    // ---- HTTP egress (019 slice) --------------------------------------------------------------------
    // Same daemon-lifetime, opt-in shape as the MES gateway above — a HttpEgressActor is best-effort
    // (no durable outbox, see egress/http_egress_actor.hpp's banner), so unlike MES there is nothing to
    // recover on restart; it still lives at daemon scope (not deployment scope) so it survives a flow
    // redeploy, matching how an Output node's egress target should outlive the flow that staged into it.
    using HttpEgress = aero::egress::HttpEgressActor;

    std::expected<void, std::string> configure_http_egress() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (http_egress_engine_) {
            return std::unexpected("HTTP egress already configured");
        }
        http_egress_actor_ = std::make_unique<HttpEgress>();
        http_egress_pool_ = std::make_unique<quark::detail::MessagePool>(1024);
        http_egress_activation_ = std::make_unique<quark::Activation>(
            http_egress_actor_.get(), HttpEgress::dispatch_table(), http_egress_pool_->sink());
        http_egress_engine_ = std::make_unique<quark::Engine<>>(quark::EngineConfig{/*workers*/ 1,
                                                                                     /*shards*/ 1,
                                                                                     /*budget*/ 64, 64});
        quark::register_actor<HttpEgress>(*http_egress_engine_, /*key*/ 1, *http_egress_activation_);
        http_egress_router_ =
            std::make_unique<quark::LocalRouter>(http_egress_engine_->post_courier(), *http_egress_pool_);
        http_egress_engine_->start();
        return {};
    }

    // Forward one staged request through the egress actor (mirrors mes_stage()'s hand-off point). Public
    // so a flow-actor integration or a test can drive it without reaching into the actor internals —
    // same honest scope as mes_stage() (no automatic live-flow-actor forwarding is wired yet either).
    std::expected<void, std::string> http_send(const aero::egress::SendHttpRequest& r) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!http_egress_engine_) return std::unexpected("HTTP egress not configured");
        http_egress_router_->get<HttpEgress>(1).tell(r);
        return {};
    }

    // Egress observability: {"configured": false} when configure_http_egress() was never called.
    nlohmann::json http_egress_stats() {
        std::lock_guard<std::mutex> lock(mtx_);
        nlohmann::json j;
        if (!http_egress_engine_) {
            j["configured"] = false;
            return j;
        }
        auto ref = http_egress_router_->get<HttpEgress>(1);
        auto r = quark::block_on(
            ref.template ask<aero::egress::HttpEgressStats>(aero::egress::GetHttpEgressStats{}));
        const aero::egress::HttpEgressStats s = r.value_or(aero::egress::HttpEgressStats{});
        j["configured"] = true;
        j["sent"] = s.sent;
        j["failed"] = s.failed;
        return j;
    }

    // ---- Cluster + Fleet(OTA) observability (010/011, 016 §2.1/§2.2) --------------------------------
    // The device registry source (016 §6 open question) is resolved here as the minimal honest answer:
    // a config-driven list, one call at daemon start (mirrors configure_mes()). Builds a ClusterView
    // scoped to what ONE daemon can honestly see (016 §2.1 — real multi-node membership stays gated on
    // Quark 019/021) and a FleetActor over per-device MockOtaDriver instances (016 §2.2 — real
    // orchestration policy + real ECDSA-P256 image signing (011 §6, aero/pal/crypto.hpp), but no real
    // firmware-push DEVICE driver exists yet, R5 — MockOtaDriver's A/B-slot protocol stands in for one).
    struct FleetDeviceConfig {
        std::string id;
        std::string initial_version;
        std::vector<std::string> required_flags;
        std::vector<std::string> preferred_flags;
    };
    // A known OTHER cluster member this node joins the real SWIM membership through (010 §5 follow-up).
    // `flags` is that peer's OWN advertised capability set — config-declared, same posture as this
    // node's own `FleetConfig::node_flags` (capabilities are NOT gossiped; only ALIVENESS is real —
    // see cluster.hpp's own banner for why that split is the honest v1 scope).
    struct ClusterMember {
        quark::NodeId id{};
        std::string host;
        std::uint16_t port = 0;
        std::vector<std::string> flags;
    };

    // Opt-in real multi-node membership (010 §5 follow-up, Phase-8): swaps `fleet_status()`'s single-
    // hardcoded-node `ClusterView` for one backed by a real `quark::SwimMembership` (021) — a genuine
    // SWIM failure detector over a real `TcpTransport`, so "which nodes are alive" becomes an honest,
    // live answer instead of a config-time fiction. `listen_port == 0` (the default) means disabled:
    // `configure_fleet()` keeps its exact pre-existing single-node behavior, unchanged.
    //
    // WHAT THIS DOES NOT DO (honest scope, matching cluster.hpp's own banner): no capability gossip
    // (peer flags are config-declared here, not discovered), and — the actually hard part — no cross-
    // node actor state hand-off. Real fenced migration (010 §3) needs the OLD and NEW node's durable
    // stores to be the SAME store; neither Quark nor AeroEdge ships a networked/shared store today, so
    // an actor's placement can be recomputed live but nothing here ever MOVES a running actor or its
    // state across nodes. This is real-time membership OBSERVABILITY, not migration.
    struct ClusterMembershipConfig {
        quark::NodeId self{};
        std::uint16_t listen_port = 0;  // this node's own SWIM listener (0 == disabled)
        std::uint64_t cluster_id = 0;
        std::vector<ClusterMember> seeds;  // other known members to join through
        std::uint32_t tick_interval_ms = 200;  // real wall-clock cadence for SwimMembership::tick()
    };

    // TEST-ONLY EC P-256 keypair (011 §3/§6, aero/pal/crypto.hpp) — a default so every existing
    // configure_fleet() caller keeps compiling/working unchanged; NEVER used in production (same
    // convention as every other checked-in *_TEST_CERTS_DIR material in this tree — generated with
    // `openssl ecparam -name prime256v1 -genkey -noout` / `openssl ec -pubout`). A real deployment
    // overrides FleetConfig::ota_signing_key_pem/ota_trust_root_public_key_pem with its own keypair.
    static constexpr const char* kDefaultOtaSigningKeyPem =
        "-----BEGIN EC PRIVATE KEY-----\n"
        "MHcCAQEEIOI8Hgmq+AGZP7mjSfqsxVBRsXuC/ffSUOzWnaSYOj9coAoGCCqGSM49\n"
        "AwEHoUQDQgAEQVB6J9oo1Z+/PPaYwJuwXSUmrZv5+U21d34+EsXvO9IyOx0sTSqv\n"
        "XyET1vSUqgc71FfeYkXkVbum6q9pUDzoMg==\n"
        "-----END EC PRIVATE KEY-----\n";
    static constexpr const char* kDefaultOtaTrustRootPublicKeyPem =
        "-----BEGIN PUBLIC KEY-----\n"
        "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEQVB6J9oo1Z+/PPaYwJuwXSUmrZv5\n"
        "+U21d34+EsXvO9IyOx0sTSqvXyET1vSUqgc71FfeYkXkVbum6q9pUDzoMg==\n"
        "-----END PUBLIC KEY-----\n";

    struct FleetConfig {
        std::vector<std::string> node_flags;
        std::vector<FleetDeviceConfig> devices;
        double ota_threshold = 1.0;
        std::size_t ota_canary = 1;
        std::size_t ota_staged = 1;
        std::size_t ota_rate_limit = 1;
        // Real ECDSA-P256/SHA-256 trust root (011 §3/§6, aero/pal/crypto.hpp): start_rollout() signs
        // with ota_signing_key_pem (the trust root's PRIVATE key — an operator/CI posture collapsed
        // into the daemon for this scope, never sent over the wire) and FleetActor/run_ota verify with
        // ota_trust_root_public_key_pem (the PUBLIC key, O1). Defaults below are TEST-ONLY material
        // (generated for this repo, never used in production — same convention as every other
        // *_TEST_CERTS_DIR in this tree) — a real deployment supplies its own keypair via Quark 020.
        std::string ota_signing_key_pem = kDefaultOtaSigningKeyPem;
        std::string ota_trust_root_public_key_pem = kDefaultOtaTrustRootPublicKeyPem;
        ClusterMembershipConfig membership;  // listen_port==0 (default) == single-node mode, unchanged
    };

    std::expected<void, std::string> configure_fleet(const FleetConfig& cfg) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (cluster_) {
            return std::unexpected("fleet already configured");
        }

        node_flags_ = cfg.node_flags;
        quark::NodeCapabilities node_caps;
        for (const auto& f : cfg.node_flags) node_caps.add(quark::Flag{f});

        if (cfg.membership.listen_port != 0) {
            // Real multi-node membership (010 §5 follow-up) — see ClusterMembershipConfig's own banner
            // for exactly what this does and does not provide.
            self_node_id_ = cfg.membership.self;
            cluster_transport_ = std::make_unique<aero::transport::TcpTransport>(
                aero::transport::TcpTransport::Config{self_node_id_, "0.0.0.0", cfg.membership.listen_port, {}});
            auto tr = cluster_transport_->start();
            if (!tr) {
                return std::unexpected("configure_fleet: cluster membership transport failed to start: " +
                                       tr.error());
            }

            quark::SwimMembership::Config swim_cfg;
            swim_cfg.cluster_id = quark::ClusterId{cfg.membership.cluster_id};
            swim_ = std::make_unique<quark::SwimMembership>(self_node_id_, *cluster_transport_, swim_cfg);

            std::vector<aero::cluster::NodeSpec> nodes;
            nodes.push_back(aero::cluster::NodeSpec{self_node_id_, node_caps});
            for (const auto& seed : cfg.membership.seeds) {
                cluster_transport_->add_peer(seed.id, seed.host, seed.port);
                quark::NodeCapabilities seed_caps;
                for (const auto& f : seed.flags) seed_caps.add(quark::Flag{f});
                nodes.push_back(aero::cluster::NodeSpec{seed.id, seed_caps});
                swim_->request_join(seed.id);
            }
            cluster_ = std::make_unique<aero::cluster::ClusterView>(std::move(nodes), *swim_);
        } else {
            cluster_ = std::make_unique<aero::cluster::ClusterView>(
                std::vector<aero::cluster::NodeSpec>{aero::cluster::NodeSpec{self_node_id_, node_caps}});
        }

        ota_signing_key_pem_ = cfg.ota_signing_key_pem;
        ota_trust_root_public_key_pem_ = cfg.ota_trust_root_public_key_pem;
        ota_ = std::make_unique<aero::ota::FleetActor>(cfg.ota_threshold, cfg.ota_canary, cfg.ota_staged,
                                                       cfg.ota_rate_limit);

        std::vector<aero::cluster::DeviceActor> devs;
        std::uint64_t key = 1;
        for (const auto& d : cfg.devices) {
            aero::cluster::PlacementRequirement req;
            for (const auto& f : d.required_flags) {
                req.required.push_back(aero::cluster::CapabilityConstraint::flag(f));
            }
            for (const auto& f : d.preferred_flags) {
                req.preferred.push_back(aero::cluster::CapabilityConstraint::flag(f));
            }
            devs.push_back(aero::cluster::DeviceActor{quark::ActorId{quark::TypeKey{0x0DE7}, key++}, req, d.id});

            // Owned here (unique_ptr so the vector can grow without invalidating the pointee address);
            // FleetActor only holds a non-owning pointer (ota/fleet.hpp FleetDevice).
            auto driver = std::make_unique<aero::ota::MockOtaDriver>(d.initial_version);
            ota_->add_device(d.id, *driver);
            ota_drivers_.push_back(std::move(driver));
        }
        placement_ = aero::cluster::place_actors(devs, *cluster_);
        device_actors_ = std::move(devs);

        if (swim_) {
            // Deferred to here (not started above, before device_actors_/placement_ existed) so the
            // FIRST tick has real data to recompute placement over — see the loop body's own comment.
            // SwimMembership::tick() itself does no sleeping ("NO sleeping, NO wall-clock wait" — its
            // own doc comment); driving the real SWIM protocol period at a real cadence is this
            // caller's job. 20ms wake-up slices (mirrors Deployment::poller's own pattern, M9.2) so
            // stop() is observed promptly rather than blocking on the full tick_interval_ms.
            swim_tick_running_.store(true, std::memory_order_release);
            swim_tick_thread_ = std::thread([this, interval_ms = cfg.membership.tick_interval_ms] {
                while (swim_tick_running_.load(std::memory_order_acquire)) {
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        swim_->tick();
                        if (cluster_) {
                            cluster_->refresh();
                            // Placement is a pure function of the current view (cluster.hpp) — cheap to
                            // recompute every tick so a node joining/leaving is reflected in
                            // fleet_status()'s device assignments, not just its node list.
                            placement_ = aero::cluster::place_actors(device_actors_, *cluster_);
                        }
                    }
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms);
                    while (std::chrono::steady_clock::now() < deadline) {
                        if (!swim_tick_running_.load(std::memory_order_acquire)) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                }
            });
        }
        return {};
    }

    // Fleet/placement observability (016 §2.1): {"configured": false} until configure_fleet().
    nlohmann::json fleet_status() {
        std::lock_guard<std::mutex> lock(mtx_);
        nlohmann::json j;
        if (!cluster_) {
            j["configured"] = false;
            return j;
        }
        j["configured"] = true;
        // `real_membership`: honest flag for WHICH kind of node list follows — a live quark::SwimMembership
        // view (010 §5 follow-up: genuinely alive/reachable right now) vs. the single-node stand-in
        // (always just self, unconditionally "alive" by definition).
        j["real_membership"] = swim_ != nullptr;
        nlohmann::json nodes = nlohmann::json::array();
        const quark::MembershipView view = cluster_->membership();
        for (const quark::NodeId n : view.nodes()) {
            nlohmann::json nj;
            nj["id"] = n.value;
            nj["alive"] = true;  // MembershipView only ever lists currently-alive members (021)
            if (n.value == self_node_id_.value) nj["flags"] = node_flags_;
            nodes.push_back(std::move(nj));
        }
        j["nodes"] = std::move(nodes);
        j["epoch"] = view.epoch();

        nlohmann::json devices = nlohmann::json::array();
        for (const auto& d : device_actors_) {
            nlohmann::json dj;
            dj["id"] = d.name;
            const auto it = placement_.assignments.find(d.id);
            dj["eligible"] = it != placement_.assignments.end();
            if (it != placement_.assignments.end()) {
                dj["node"] = it->second.value;
            }
            devices.push_back(std::move(dj));
        }
        j["devices"] = std::move(devices);
        return j;
    }

    // OTA rollout observability (016 §2.2): {"configured": false} until configure_fleet().
    nlohmann::json ota_status() {
        std::lock_guard<std::mutex> lock(mtx_);
        nlohmann::json j;
        if (!ota_) {
            j["configured"] = false;
            return j;
        }
        j["configured"] = true;
        j["state"] = rollout_state_name(ota_->state());
        j["devices_updated"] = ota_->devices_updated();
        j["devices_rolled_back"] = ota_->devices_rolled_back();
        nlohmann::json waves = nlohmann::json::array();
        for (const auto& w : last_waves_) {
            nlohmann::json wj;
            wj["name"] = w.name;
            wj["attempted"] = w.attempted;
            wj["succeeded"] = w.succeeded;
            wj["success_rate"] = w.success_rate;
            wj["passed"] = w.passed;
            waves.push_back(std::move(wj));
        }
        j["waves"] = std::move(waves);
        return j;
    }

    // Start a wave-by-wave rollout (011 §4) against the registered (mock) drivers. `image_bytes` is the
    // firmware payload; the signature is derived from the daemon's own configured trust key (never sent
    // over the wire — 012 M5 posture). Synchronous: FleetActor::run() is a deterministic, in-memory,
    // sub-millisecond loop over MockOtaDriver — no socket, so no need for a separate actor/engine (unlike
    // the MES gateway, which fronts a real blocking HTTP client).
    std::expected<void, std::string> start_rollout(const std::string& image_version,
                                                    const std::string& image_bytes) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!ota_) {
            return std::unexpected("fleet not configured");
        }
        aero::ota::OtaImage image{image_version, image_bytes, ""};
        image.signature = aero::ota::sign_image(image, ota_signing_key_pem_);
        last_waves_ = ota_->run(image, ota_trust_root_public_key_pem_);
        return {};
    }

    // ---- Native MQTT broker / southbound termination (017, Phase 1) ---------------------------------
    // Daemon-lifetime subsystem, same shape as the MES gateway/fleet above: configure once at daemon
    // start (its own ownership, independent of any deployed Application's deploy/undeploy cycle — the
    // broker's job is accepting device connections, not running the flow), a second call is rejected
    // rather than silently replacing the listening broker underneath live sessions.
    // `cluster` (017 M6, opt-in): when omitted/`std::nullopt`, or its `peers` list is empty, NO
    // `BrokerCluster` is constructed at all — no second engine, no inter-broker listener, behavior
    // identical to every caller before M6 (single-node broker, exactly as configure_broker(cfg) alone
    // always was). Only a non-empty peer list opts a deployment into cross-node PUBLISH broadcast.
    std::expected<void, std::string> configure_broker(
        const aero::broker::Config& cfg,
        const std::optional<aero::broker::BrokerClusterConfig>& cluster = std::nullopt) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (broker_) {
            return std::unexpected("native broker already configured");
        }
        auto b = std::make_unique<aero::broker::NativeBroker>(cfg);
        auto r = b->start();
        if (!r) return std::unexpected(r.error());
        broker_ = std::move(b);

        if (cluster && !cluster->peers.empty()) {
            auto c = std::make_unique<aero::broker::BrokerCluster>(cluster->self, cluster->cluster_port,
                                                                    cluster->peers, *broker_);
            auto cr = c->start();
            if (!cr) {
                broker_->stop();
                broker_.reset();
                return std::unexpected(cr.error());
            }
            broker_cluster_ = std::move(c);
        }
        return {};
    }

    // Broker observability (017): {"configured": false} until configure_broker(). Phase 1's NativeBroker
    // exposes only listen_port() as a public accessor beyond start/stop/on_publish — no session count or
    // subscription list is available yet, so that's all this reports for now (gap noted for a follow-on
    // phase once the broker grows a status()-shaped accessor).
    nlohmann::json broker_status() {
        std::lock_guard<std::mutex> lock(mtx_);
        nlohmann::json j;
        if (!broker_) {
            j["configured"] = false;
            return j;
        }
        j["configured"] = true;
        j["listen_port"] = broker_->listen_port();
        return j;
    }

private:
    static const char* rollout_state_name(aero::ota::RolloutState s) noexcept {
        switch (s) {
            case aero::ota::RolloutState::Idle:      return "Idle";
            case aero::ota::RolloutState::Running:   return "Running";
            case aero::ota::RolloutState::Paused:    return "Paused";
            case aero::ota::RolloutState::Completed: return "Completed";
        }
        return "Idle";
    }

    // All lifetimes of one deployment. Held as unique_ptrs so deploy() can build them imperatively
    // (register_actor must run between construction steps) and teardown() can order the shutdown.
    struct Deployment {
        schema::Application app;  // the running Application (name+version+shape) — the reload/rollback base
        std::string name;
        std::string version;
        std::uint64_t key = 0;

        std::unique_ptr<CompiledPlan> plan;  // owns the nodes + the bound flow (stable heap address)
        std::unique_ptr<aero::IDriver> driver;

        std::unique_ptr<FlowActor> actor;
        std::unique_ptr<quark::detail::MessagePool> pool;
        std::unique_ptr<quark::Activation> activation;
        std::unique_ptr<quark::Engine<>> engine;
        std::unique_ptr<quark::LocalRouter> router;

        std::unique_ptr<std::pmr::monotonic_buffer_resource> mr;
        std::unique_ptr<quark::StreamActivation<aero::Frame>> stream;

        std::atomic<bool> stop_flag{false};
        std::atomic<bool> producer_done{false};
        bool has_driver = false;

        std::thread producer;  // declared last → joined in teardown, dtor sees non-joinable
        std::thread bridge;
        std::thread poller;    // PULL drivers only (§6.1) — see deploy()'s driver-ingestion branch
    };

    // Classify a reload as Live or BuildOnly (009 §4 table, P3). Returns nullopt for a Live change
    // (flow graph / node config — hot-swappable), or a reason string for a BuildOnly change that
    // cannot be a live pointer-swap: a different actor kind or key (a different actor identity /
    // placement), or a persistence model/mode change (rebinds the durable-state path, 007). A name
    // change is treated as BuildOnly too — reload targets the SAME Application, not a replacement.
    static std::optional<std::string> classify_buildonly(const schema::Application& cur,
                                                         const schema::Application& next) {
        if (cur.name != next.name) {
            return "app name changed ('" + cur.name + "' -> '" + next.name + "')";
        }
        if (cur.actor.kind != next.actor.kind) {
            return "actor kind changed ('" + cur.actor.kind + "' -> '" + next.actor.kind + "')";
        }
        if (cur.actor.key != next.actor.key) {
            return "actor key changed";
        }
        const bool cur_p = cur.persistence.has_value();
        const bool next_p = next.persistence.has_value();
        if (cur_p != next_p) {
            return "persistence presence changed";
        }
        if (cur_p && next_p &&
            (cur.persistence->model != next.persistence->model ||
             cur.persistence->mode != next.persistence->mode)) {
            return "persistence model/mode changed";
        }
        return std::nullopt;  // Live: flow graph and/or node config only
    }

    static void teardown(Deployment& d) noexcept {
        d.stop_flag.store(true, std::memory_order_release);  // graceful stop (006 §8): finish in-flight
        if (d.producer.joinable()) d.producer.join();
        if (d.bridge.joinable()) d.bridge.join();
        if (d.poller.joinable()) d.poller.join();
        if (d.engine) d.engine->stop();
        if (d.driver) d.driver->close();
    }

    NodeRegistry nodes_;
    DriverRegistry drivers_;
    std::unique_ptr<Deployment> dep_;
    std::optional<schema::Application> previous_app_;  // the prior version, for rollback (009 §6)
    std::mutex mtx_;

    // MES gateway lifetime (daemon-scoped, not deployment-scoped — see configure_mes() above).
    std::unique_ptr<aero::mes::RestMesAdapter> mes_adapter_;
    std::unique_ptr<quark::InMemoryStore> mes_store_;
    std::unique_ptr<MesGateway> mes_gateway_;
    std::unique_ptr<quark::detail::MessagePool> mes_pool_;
    std::unique_ptr<quark::Activation> mes_activation_;
    std::unique_ptr<quark::Engine<>> mes_engine_;
    std::unique_ptr<quark::LocalRouter> mes_router_;

    // HTTP egress lifetime (daemon-scoped — see configure_http_egress() above).
    std::unique_ptr<HttpEgress> http_egress_actor_;
    std::unique_ptr<quark::detail::MessagePool> http_egress_pool_;
    std::unique_ptr<quark::Activation> http_egress_activation_;
    std::unique_ptr<quark::Engine<>> http_egress_engine_;
    std::unique_ptr<quark::LocalRouter> http_egress_router_;

    // Fleet/OTA/placement lifetime (daemon-scoped, set by configure_fleet() above).
    std::vector<std::string> node_flags_;
    quark::NodeId self_node_id_{1};  // NodeId{1}: matches the pre-M5.1-follow-up single-node default
    std::unique_ptr<aero::cluster::ClusterView> cluster_;
    std::vector<aero::cluster::DeviceActor> device_actors_;
    aero::cluster::PlacementPlan placement_;
    std::vector<std::unique_ptr<aero::ota::MockOtaDriver>> ota_drivers_;
    std::unique_ptr<aero::ota::FleetActor> ota_;
    std::string ota_signing_key_pem_;
    std::string ota_trust_root_public_key_pem_;
    std::vector<aero::ota::WaveResult> last_waves_;

    // Real multi-node membership (010 §5 follow-up, opt-in via FleetConfig::membership) — all null/
    // idle unless configure_fleet() was given a non-zero listen_port. See ~Runtime() for stop ordering.
    std::unique_ptr<aero::transport::TcpTransport> cluster_transport_;
    std::unique_ptr<quark::SwimMembership> swim_;
    std::thread swim_tick_thread_;
    std::atomic<bool> swim_tick_running_{false};

    // Native MQTT broker lifetime (daemon-scoped, set by configure_broker() above).
    std::unique_ptr<aero::broker::NativeBroker> broker_;
    // Cross-node PUBLISH broadcast (017 M6, opt-in) — nullptr unless configure_broker()'s cluster
    // param was given a non-empty peer list. See ~Runtime() for the required stop() ordering.
    std::unique_ptr<aero::broker::BrokerCluster> broker_cluster_;
};

}  // namespace aero::runtime
