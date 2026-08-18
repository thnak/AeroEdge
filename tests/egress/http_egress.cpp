// AeroEdge 019 slice — aero.output.http end to end. Mirrors tests/mes/mes_outbox.cpp's shape (a local
// httplib server, ACT-numbered assertions) but WITHOUT a durable outbox — HttpEgressActor is best-effort
// by design (see egress/http_egress_actor.hpp's banner), so there is nothing to recover across a
// restart to prove, unlike MES's ACT2.
//
//   ACT 0 — a flow with HttpOutputNode STAGES a request into ctx (I1: the node only stages, never does
//           HTTP I/O itself). This is the flow -> egress hand-off payload, same shape as MES's ACT0.
//   ACT 1 — Runtime::http_send() forwards the staged request through a live HttpEgressActor to a local
//           httplib server; the server receives the expected method/path/body/header, and the egress
//           actor's own stats count it as sent.
//   ACT 2 — a failed delivery (server down / wrong port) counts as failed, not a crash — best-effort
//           means the caller is never blocked or thrown into by a dead endpoint.
//
// Exit code 0 = OK.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "aero/core/compiled_flow.hpp"
#include "aero/egress/http_egress_actor.hpp"
#include "aero/nodes/builtin_nodes.hpp"
#include "aero/nodes/http_output_node.hpp"
#include "aero/runtime/runtime.hpp"
#include "httplib.h"

int main() {
    bool ok = true;

    // ----- ACT 0: a flow with HttpOutputNode stages a request into ctx (I1: stage only) -------------
    aero::StagedHttpRequest staged;
    {
        aero::nodes::DecodeSourceNode source;
        aero::nodes::HttpOutputNode http_node("http://placeholder/ignored", "POST", "{}", 2000);
        aero::CompiledFlow flow;
        flow.add(source).add(http_node);
        aero::ProcessingContext ctx;
        aero::Frame frame{42};
        ctx.reset(&frame);
        flow.execute(ctx);
        const bool staged_ok = ctx.http_requests.size() == 1 && ctx.http_requests[0].method == "POST";
        ok &= staged_ok;
        if (!ctx.http_requests.empty()) staged = ctx.http_requests[0];
        std::printf("[act0] HttpOutputNode staged %zu request(s) method=%s body=%s %s\n",
                    ctx.http_requests.size(), std::string(staged.method).c_str(), staged.body.c_str(),
                    staged_ok ? "ok" : "FAIL");
    }

    // ----- Start a local mock HTTP sink on an ephemeral port ----------------------------------------
    std::mutex mu;
    int posts = 0;
    std::string last_path, last_body;
    httplib::Server svr;
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });
    svr.Post("/ingest", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(mu);
        ++posts;
        last_path = req.path;
        last_body = req.body;
        res.status = 200;
    });
    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) { std::printf("bind failed\nFAIL\n"); return 1; }
    std::thread server_thread([&svr] { svr.listen_after_bind(); });

    {
        httplib::Client probe("127.0.0.1", port);
        probe.set_connection_timeout(2, 0);
        bool ready = false;
        for (int i = 0; i < 300 && !ready; ++i) {
            auto h = probe.Get("/health");
            if (h && h->status == 200) ready = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!ready) { std::printf("sink not ready\nFAIL\n"); svr.stop(); server_thread.join(); return 1; }
    }

    aero::runtime::Runtime rt;
    auto cfg = rt.configure_http_egress();
    ok &= static_cast<bool>(cfg);
    std::printf("[setup] configure_http_egress: %s\n", cfg ? "ok" : "FAIL");

    // ----- ACT 1: forward the staged request to the real sink through HttpEgressActor ---------------
    {
        aero::egress::SendHttpRequest req;
        req.url = "http://127.0.0.1:" + std::to_string(port) + "/ingest";
        req.method = "POST";
        req.headers_json = "{}";
        req.timeout_ms = 2000;
        req.body = R"({"raw":42})";
        auto r = rt.http_send(req);
        ok &= static_cast<bool>(r);

        bool delivered = false;
        for (int i = 0; i < 300 && !delivered; ++i) {
            {
                std::lock_guard<std::mutex> g(mu);
                delivered = posts >= 1;
            }
            if (!delivered) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::lock_guard<std::mutex> g(mu);
        const bool right_shape = delivered && last_path == "/ingest" && last_body == req.body;
        ok &= right_shape;
        std::printf("[act1] delivered=%d path=%s body=%s %s\n", posts, last_path.c_str(),
                    last_body.c_str(), right_shape ? "ok" : "FAIL");

        auto stats = rt.http_egress_stats();
        const bool stats_ok = stats.value("configured", false) && stats.value("sent", 0) >= 1;
        ok &= stats_ok;
        std::printf("[act1] egress stats: %s %s\n", stats.dump().c_str(), stats_ok ? "ok" : "FAIL");
    }

    // ----- ACT 2: a dead endpoint counts as failed, never blocks/crashes ----------------------------
    {
        aero::egress::SendHttpRequest dead;
        dead.url = "http://127.0.0.1:1/unreachable";  // port 1: nothing listens, connect fails fast
        dead.method = "POST";
        dead.headers_json = "{}";
        dead.timeout_ms = 500;
        dead.body = "{}";
        auto r = rt.http_send(dead);
        ok &= static_cast<bool>(r);

        std::uint64_t failed_before = rt.http_egress_stats().value("failed", 0);
        bool saw_failure = failed_before >= 1;
        for (int i = 0; i < 100 && !saw_failure; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            saw_failure = rt.http_egress_stats().value("failed", 0) >= 1;
        }
        ok &= saw_failure;
        std::printf("[act2] dead endpoint counted as failed: %s\n", saw_failure ? "ok" : "FAIL");
    }

    svr.stop();
    server_thread.join();

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
