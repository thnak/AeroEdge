// AeroEdge MES adapter — RestMesAdapter over an HTTP MES (spec 012 §5, the default-build adapter).
//
// Implements IMesAdapter by POSTing the canonical MesReport as JSON to a REST MES. httplib (the
// blocking HTTP client) is confined HERE, in aero-mes (R1) — it never enters aero-core/aero-sdk; the
// canonical DTOs + the seam (mes.hpp) stay HTTP-free so a flow/actor depends only on the contract.
//
// The gateway owns the instance and is the only caller (M2), so the blocking client runs on the
// gateway's lane, never a flow. Delivery mapping (M3): a 2xx → Delivered (drop from outbox); a
// transport error / 5xx / 429 → Retry (STAY in the outbox, re-send later — an MES outage delays but
// never drops a report); a 4xx → Rejected (permanent, drop so a poison report can't wedge the drain).
// The idempotency key rides an `Idempotency-Key` header so a retried POST is deduped MES-side (M3).
#pragma once

#include <mutex>
#include <string>

#include "aero/mes/mes.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace aero::mes {

class RestMesAdapter final : public IMesAdapter {
public:
    MesStatus connect(const MesConfig& cfg) override {
        std::lock_guard<std::mutex> lock(mtx_);
        cfg_ = cfg;
        // A fresh client bound to host:port. httplib::Client is not thread-safe for concurrent calls;
        // the gateway is a single-executor actor so all report()s serialize — the mutex is belt-and-
        // braces for a test that shares one adapter across a stage + a direct probe.
        cli_ = std::make_unique<httplib::Client>(cfg.endpoint, cfg.port);
        cli_->set_connection_timeout(2, 0);
        cli_->set_read_timeout(3, 0);
        cli_->set_keep_alive(true);
        return MesStatus::Ok;  // lazy connect: the first report() is the real reachability probe
    }

    MesResult report(const MesReport& r) override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!cli_) return MesResult::Retry;  // not connected yet — keep it in the outbox

        nlohmann::json body;
        body["kind"] = r.kind_name();
        body["line"] = r.line;
        body["label"] = r.label;
        body["value"] = r.value;
        body["seq"] = r.seq;
        body["idempotency_key"] = r.idempotency_key;

        httplib::Headers headers = {{"Idempotency-Key", r.idempotency_key}};
        if (!cfg_.token.empty()) headers.emplace("Authorization", "Bearer " + cfg_.token);

        auto res = cli_->Post(cfg_.report_path, headers, body.dump(), "application/json");
        if (!res) return MesResult::Retry;              // transport error (MES down/unreachable) — M3
        const int s = res->status;
        if (s >= 200 && s < 300) return MesResult::Delivered;
        if (s == 429 || (s >= 500 && s < 600)) return MesResult::Retry;  // transient — keep + re-send
        return MesResult::Rejected;                     // 4xx permanent — drop so it can't wedge drain
    }

    const MesAdapterDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr MesAdapterDescriptor kDesc{"aero.mes.rest", /*outbound*/ true, /*inbound*/ false};

private:
    std::mutex mtx_;
    MesConfig cfg_{};
    std::unique_ptr<httplib::Client> cli_;
};

}  // namespace aero::mes
