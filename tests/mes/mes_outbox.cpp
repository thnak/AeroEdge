// AeroEdge Phase-10 LOAD-BEARING gate #1 (spec 012 §3, M2/M3): the transactional MES outbox.
//
//   ACT 0 — a flow with a MesReportNode STAGES a report into ctx (I1/M2: the node only stages, it never
//           does MES I/O). This is the flow→gateway hand-off payload.
//   ACT 1 — MES DOWN (a local httplib MES returning 503): the MesGateway actor durably STAGES the report
//           and its drain attempt gets Retry, so the report is RETAINED in the outbox — not dropped (M3).
//   ACT 2 — DURABILITY across a simulated gateway restart: a FRESH gateway over the SAME Quark 012 store
//           RECOVERS the pending report (proves it survived, M3).
//   ACT 3 — MES UP: drain the recovered outbox → the MES receives the report; pending → 0, delivered → 1.
//   ACT 4 — EXACTLY-ONCE: a duplicate delivery (same idempotency key) is deduped MES-side — the server's
//           unique-report count stays 1 though it saw 2 POSTs (at-least-once + idempotency = once).
//
// Robustness: ephemeral-port local MES, wait-for-ready by bounded /health polling, clean server + engine
// shutdown (TSan-clean). Exit code 0 = OK; prints "FAIL" on any mismatch.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "aero/core/compiled_flow.hpp"
#include "aero/mes/mes.hpp"
#include "aero/mes/outbox.hpp"
#include "aero/mes/rest_mes_adapter.hpp"
#include "aero/nodes/builtin_nodes.hpp"
#include "aero/nodes/mes_nodes.hpp"
#include "httplib.h"
#include "quark/core/engine.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/spawn.hpp"

using namespace aero::mes;
using quark::ActorId;
using quark::InMemoryStore;
using quark::TypeKey;

// A controllable mock MES: 503 when "down", 200 + dedup-by-idempotency-key when "up".
struct MockMes {
    std::atomic<bool> up{false};
    std::mutex mu;
    std::set<std::string> unique_keys;  // distinct reports actually accepted (exactly-once proof)
    int total_posts = 0;                // all accepted POSTs (>= unique_keys on a retry)

    void install(httplib::Server& svr) {
        svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("ok", "text/plain");
        });
        svr.Post("/production", [this](const httplib::Request& req, httplib::Response& res) {
            if (!up.load(std::memory_order_acquire)) {
                res.status = 503;  // MES outage — the adapter maps this to Retry (M3)
                return;
            }
            const std::string key = req.get_header_value("Idempotency-Key");
            {
                std::lock_guard<std::mutex> g(mu);
                unique_keys.insert(key);  // dedup: a retried key does not add a second unique report
                ++total_posts;
            }
            res.status = 200;
        });
    }
    std::size_t unique_count() { std::lock_guard<std::mutex> g(mu); return unique_keys.size(); }
    int posts() { std::lock_guard<std::mutex> g(mu); return total_posts; }
};

// Bring up a gateway actor over `store`+`outbox_id`, run `body`, tear down cleanly.
template <class Body>
static void with_gateway(IMesAdapter& adapter, InMemoryStore& store, ActorId outbox_id, std::uint32_t key,
                         Body&& body) {
    using Gw = MesGatewayActor<InMemoryStore>;
    Gw gw(adapter, store, outbox_id);
    quark::detail::MessagePool pool(1024);
    quark::Activation act(&gw, Gw::dispatch_table(), pool.sink());
    quark::Engine<> eng(quark::EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});
    quark::register_actor<Gw>(eng, key, act);
    quark::LocalRouter router(eng.post_courier(), pool);
    auto ref = router.get<Gw>(key);
    eng.start();
    body(ref);
    eng.stop();
}

int main() {
    bool ok = true;

    // ----- ACT 0: a flow with MesReportNode stages a report into ctx (M2: stage only) -------------
    aero::mes::MesReport staged_report;
    {
        aero::nodes::DecodeSourceNode source;
        aero::nodes::MesReportNode report_node("line-7", "produced", aero::StagedMesReport::Kind::Production);
        aero::CompiledFlow flow;
        flow.add(source).add(report_node);
        aero::ProcessingContext ctx;
        aero::Frame frame{42};
        ctx.reset(&frame);
        flow.execute(ctx);
        const bool staged = ctx.mes_reports.size() == 1 &&
                            ctx.mes_reports[0].kind == aero::StagedMesReport::Kind::Production &&
                            ctx.mes_reports[0].line == "line-7" && ctx.mes_reports[0].value == 42.0;
        ok &= staged;
        // The flow actor would forward this to the gateway (M2). Build the canonical report here.
        if (!ctx.mes_reports.empty()) {
            const auto& s = ctx.mes_reports[0];
            staged_report = MesReport{MesReport::Kind::Production, std::string(s.line),
                                      std::string(s.label), s.value, 0, ""};
        }
        std::printf("[act0] MesReportNode staged %zu report(s) line=%s value=%.0f %s\n",
                    ctx.mes_reports.size(), staged_report.line.c_str(), staged_report.value,
                    staged ? "ok" : "FAIL");
    }

    // ----- Start the local mock MES on an ephemeral port (initially DOWN) -------------------------
    MockMes mes;
    httplib::Server svr;
    mes.install(svr);
    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) { std::printf("bind failed\nFAIL\n"); return 1; }
    std::thread server_thread([&svr] { svr.listen_after_bind(); });

    // Wait-for-ready: bounded /health poll (<= 3s).
    {
        httplib::Client probe("127.0.0.1", port);
        probe.set_connection_timeout(2, 0);
        bool ready = false;
        for (int i = 0; i < 300 && !ready; ++i) {
            auto h = probe.Get("/health");
            if (h && h->status == 200) ready = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!ready) { std::printf("MES not ready\nFAIL\n"); svr.stop(); server_thread.join(); return 1; }
    }

    RestMesAdapter adapter;
    MesConfig cfg;
    cfg.endpoint = "127.0.0.1";
    cfg.port = port;
    cfg.report_path = "/production";
    adapter.connect(cfg);

    InMemoryStore store;
    const ActorId outbox_id{TypeKey{0x0B08}, 1};
    std::string first_key;

    // ----- ACT 1: MES DOWN → stage durably, drain gets Retry → report RETAINED (M3) ---------------
    mes.up.store(false, std::memory_order_release);
    with_gateway(adapter, store, outbox_id, /*key*/ 1, [&](auto& ref) {
        ref.tell(StageReport{staged_report});
        auto st = quark::block_on(ref.template ask<OutboxStats>(GetOutboxStats{}));
        const OutboxStats s = st.value_or(OutboxStats{});
        const bool retained = s.staged == 1 && s.pending == 1 && s.delivered == 0;
        ok &= retained;
        std::printf("[act1] MES down: staged=%llu pending=%llu delivered=%llu (retained, not dropped) %s\n",
                    (unsigned long long)s.staged, (unsigned long long)s.pending,
                    (unsigned long long)s.delivered, retained ? "ok" : "FAIL");
    });

    // ----- ACT 2: DURABILITY across restart — a fresh gateway recovers the pending report ----------
    with_gateway(adapter, store, outbox_id, /*key*/ 2, [&](auto& ref) {
        auto st = quark::block_on(ref.template ask<OutboxStats>(GetOutboxStats{}));
        const OutboxStats s = st.value_or(OutboxStats{});
        const bool survived = s.pending == 1 && s.staged == 1;
        ok &= survived;
        std::printf("[act2] restart: recovered pending=%llu (report survived the outage) %s\n",
                    (unsigned long long)s.pending, survived ? "ok" : "FAIL");
    });

    // ----- ACT 3: MES UP → drain → delivered, pending → 0 -----------------------------------------
    mes.up.store(true, std::memory_order_release);
    with_gateway(adapter, store, outbox_id, /*key*/ 3, [&](auto& ref) {
        ref.tell(DrainOutbox{});
        auto st = quark::block_on(ref.template ask<OutboxStats>(GetOutboxStats{}));
        const OutboxStats s = st.value_or(OutboxStats{});
        const bool drained = s.pending == 0 && s.delivered == 1;
        ok &= drained;
        std::printf("[act3] MES up: drained pending=%llu delivered=%llu; MES unique=%zu posts=%d %s\n",
                    (unsigned long long)s.pending, (unsigned long long)s.delivered, mes.unique_count(),
                    mes.posts(), (drained && mes.unique_count() == 1) ? "ok" : "FAIL");
        ok &= (mes.unique_count() == 1);
    });

    // ----- ACT 4: EXACTLY-ONCE — a duplicate delivery (same idempotency key) is deduped MES-side ---
    {
        // Reconstruct the exact report the outbox delivered (seq 1 → its stable idempotency key) and
        // deliver it AGAIN directly through the adapter — an at-least-once retry. The MES dedups by key.
        MesReport dup{MesReport::Kind::Production, "line-7", "produced", 42.0, 1,
                      std::string("line-7:production:1")};
        const MesResult r = adapter.report(dup);
        const bool once = r == MesResult::Delivered && mes.unique_count() == 1 && mes.posts() == 2;
        ok &= once;
        std::printf("[act4] duplicate retry: result=Delivered, MES unique=%zu posts=%d (exactly-once) %s\n",
                    mes.unique_count(), mes.posts(), once ? "ok" : "FAIL");
    }

    svr.stop();
    server_thread.join();

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
