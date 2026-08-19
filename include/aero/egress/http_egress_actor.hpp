// AeroEdge HTTP egress — the delivery lane for `aero.output.http` (019 slice). httplib (the blocking
// HTTP client) is confined HERE, in aero-egress (R1) — it never enters aero-core/aero-sdk; the node
// (nodes/http_output_node.hpp) only stages a StagedHttpRequest, SDK-level and HTTP-free.
//
// Deliberately NOT `aero-mes`'s durable outbox (mes/outbox.hpp: Outbox<Store>/MesGatewayActor) — that
// machinery exists for MES's at-least-once/survive-a-crash requirement (012 §3). A generic HTTP utility
// node has no such requirement by default: this actor is best-effort — one httplib::Client per send, on
// its own Sequential lane, off every flow (I1). A durable retry path is future work if a real use case
// demands it, not built speculatively now (see http_output_node.hpp's banner for the same reasoning).
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"  // quark::Ask
#include "quark/core/dispatch.hpp"   // quark::Protocol, Sequential

namespace aero::egress {

// From Runtime::http_send (mirrors aero::mes::StageReport) -> this actor: one resolved HTTP request.
// Owns its own strings (unlike StagedHttpRequest's views into node config, which die with the flow,
// 003 I6) so it fits a `tell()` that outlives the originating flow.
struct SendHttpRequest {
    std::string url;
    std::string method;
    std::string headers_json;
    std::int32_t timeout_ms = 2000;
    std::string body;
};

struct HttpEgressStats {
    std::uint64_t sent = 0;
    std::uint64_t failed = 0;
};
struct GetHttpEgressStats {};

struct HttpEgressActor : quark::Actor<HttpEgressActor, quark::Sequential> {
    using protocol = quark::Protocol<SendHttpRequest, quark::Ask<GetHttpEgressStats, HttpEgressStats>>;

    void handle(const SendHttpRequest& r) noexcept {
        const auto [scheme_host_port, path] = split_url(r.url);
        httplib::Client cli(scheme_host_port);
        const int secs = r.timeout_ms / 1000;
        const int usecs = (r.timeout_ms % 1000) * 1000;
        cli.set_connection_timeout(secs, usecs);
        cli.set_read_timeout(secs, usecs);

        httplib::Headers headers;
        const auto hj = nlohmann::json::parse(r.headers_json, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (hj.is_object()) {
            for (auto it = hj.begin(); it != hj.end(); ++it) {
                if (it.value().is_string()) headers.emplace(it.key(), it.value().get<std::string>());
            }
        }

        httplib::Result res;
        if (r.method == "GET") res = cli.Get(path, headers);
        else if (r.method == "PUT") res = cli.Put(path, headers, r.body, "application/json");
        else if (r.method == "PATCH") res = cli.Patch(path, headers, r.body, "application/json");
        else if (r.method == "DELETE") res = cli.Delete(path, headers, r.body, "application/json");
        else res = cli.Post(path, headers, r.body, "application/json");  // default POST

        if (res && res->status >= 200 && res->status < 300) ++sent_;
        else ++failed_;
    }

    void handle(const quark::Ask<GetHttpEgressStats, HttpEgressStats>& m) noexcept {
        m.respond(HttpEgressStats{sent_, failed_});
    }

private:
    // Splits "scheme://host[:port][/path...]" into the httplib::Client(scheme_host_port) part and the
    // request path — httplib's single-string Client ctor wants the former, its Get/Post/... the latter.
    // A URL with no path (or no scheme) degrades to path "/" / the whole string as scheme_host_port —
    // same tolerant, hand-rolled-parser posture as this codebase's Modbus target-string parsing.
    static std::pair<std::string, std::string> split_url(const std::string& url) {
        const auto scheme_end = url.find("://");
        const std::size_t search_from = scheme_end == std::string::npos ? 0 : scheme_end + 3;
        const auto path_start = url.find('/', search_from);
        if (path_start == std::string::npos) return {url, "/"};
        return {url.substr(0, path_start), url.substr(path_start)};
    }

    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
};

}  // namespace aero::egress
