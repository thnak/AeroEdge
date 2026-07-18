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
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <optional>

#include "aero/core/compiled_flow.hpp"
#include "aero/core/registry.hpp"
#include "aero/drivers/generator_driver.hpp"
#include "aero/ext/native_loader.hpp"
#include "aero/nodes/builtin_nodes.hpp"
#include "aero/nodes/compute_nodes.hpp"
#include "aero/nodes/expr_rule_node.hpp"
#include "aero/nodes/mes_nodes.hpp"
#include "aero/runtime/flow_actor.hpp"
#include "aero/runtime/flow_compiler.hpp"
#include "aero/schema/application.hpp"
#include "aero/sdk/driver.hpp"
#include "aero/sdk/node.hpp"
#include "nlohmann/json.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/activation.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/engine_config.hpp"
#include "quark/core/spawn.hpp"
#include "quark/core/stream_activation.hpp"

namespace aero::runtime {

// Populate the registries with the Phase-4 built-in node/driver factories (005 §5). Lives here (not in
// aero-core/registry.hpp) because it #includes aero-nodes/aero-drivers — the one-way layering (R1)
// forbids aero-core depending upward on them.
inline void register_builtins(NodeRegistry& node_reg, DriverRegistry& driver_reg) {
    node_reg.register_type("aero.source.decode", [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::DecodeSourceNode>();
    });
    node_reg.register_type("aero.transform.scale", [](const nlohmann::json& c) {
        return std::make_unique<aero::nodes::ScaleNode>(c.value("factor", 1.0));
    });
    node_reg.register_type("aero.transform.moving_average", [](const nlohmann::json& c) {
        return std::make_unique<aero::nodes::RuntimeMovingAverageNode>(c.value("window", std::size_t{1}));
    });
    node_reg.register_type("aero.output.sum", [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::SumOutputNode>();
    });
    // Low-code Rule DSL (008 §6): parse the expression ONCE here (deploy), 0-alloc eval per Command.
    // A malformed 'expr' is rejected earlier by the flow compiler (validate_node_config); this factory
    // parses again and, defensively, a bad program yields a node whose process() returns Error.
    node_reg.register_type("aero.rule.expr", [](const nlohmann::json& c) -> std::unique_ptr<INode> {
        auto prog = aero::nodes::ExprRuleNode::compile(c.value("expr", std::string{}));
        return std::make_unique<aero::nodes::ExprRuleNode>(
            std::move(prog), c.value("alarm", std::string{"AlarmRaised"}));
    });

    // Phase-10 compute-node breadth (005 §2): pure, socket-free transforms/sources (compute_nodes.hpp).
    node_reg.register_type("aero.transform.mean", [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::MeanNode>();
    });
    node_reg.register_type("aero.transform.minmax", [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::MinMaxNode>();
    });
    node_reg.register_type("aero.transform.sum", [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::SumNode>();
    });
    node_reg.register_type("aero.transform.crc", [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::CrcNode>();
    });
    // Modbus register-map DECODE over already-arrived bytes (no socket; the Modbus-TCP transport is gated).
    node_reg.register_type("aero.source.modbus", [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::ModbusDecodeNode>();
    });
    node_reg.register_type("aero.source.json", [](const nlohmann::json&) {
        return std::make_unique<aero::nodes::JsonParseNode>();
    });

    // Phase-10 MES hook (012 §4): the outbound report Output node + the inbound order Source node.
    node_reg.register_type("aero.output.mes", [](const nlohmann::json& c) {
        auto kind = aero::StagedMesReport::Kind::Production;
        const std::string k = c.value("kind", std::string{"production"});
        if (k == "alarm") kind = aero::StagedMesReport::Kind::Alarm;
        else if (k == "tag_sample") kind = aero::StagedMesReport::Kind::TagSample;
        return std::make_unique<aero::nodes::MesReportNode>(
            c.value("line", std::string{"line-1"}), c.value("label", std::string{"produced"}), kind);
    });
    node_reg.register_type("aero.source.mes_order", [](const nlohmann::json& c) {
        return std::make_unique<aero::nodes::MesOrderSourceNode>(c.value("order_qty", 0.0));
    });

    driver_reg.register_type("aero.driver.generator", [](const nlohmann::json&) {
        return std::make_unique<aero::drivers::GeneratorDriver>();
    });
}

class Runtime {
public:
    Runtime() { register_builtins(nodes_, drivers_); }
    ~Runtime() { (void)undeploy(); }

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
            if (d->driver->open(dcfg) != aero::DriverStatus::Ok) {
                d->engine->stop();
                return std::unexpected("driver.open failed for '" + app.driver->type_id + "'");
            }

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
            d->has_driver = true;

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
                            ref.tell(ReceiveFrame{f->raw});  // copy scalar out of the pinned slot
                            batch.retire();                  // return credit after the tell is enqueued
                        }
                    }
                    if (dp->stop_flag.load(std::memory_order_acquire)) break;
                    if (producer_done && ch.occupancy() == 0) break;  // bounded driver finished + drained
                    std::this_thread::yield();  // no sleep; progress bounded by frame_count / stop
                }
            });
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

private:
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
        if (d.engine) d.engine->stop();
        if (d.driver) d.driver->close();
    }

    NodeRegistry nodes_;
    DriverRegistry drivers_;
    std::unique_ptr<Deployment> dep_;
    std::optional<schema::Application> previous_app_;  // the prior version, for rollback (009 §6)
    std::mutex mtx_;
};

}  // namespace aero::runtime
