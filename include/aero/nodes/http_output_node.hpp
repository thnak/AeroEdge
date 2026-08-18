// AeroEdge built-in node — HTTP output (019 slice: `aero.output.http`). An ordinary Output-category
// node, same shape/scope as MesReportNode (mes_nodes.hpp): it ONLY stages a request into
// ctx.http_requests (I1 — a node never does I/O inline); the actual blocking HTTP call happens on
// HttpEgressActor's own lane (egress/http_egress_actor.hpp), off every flow. Deliberately does NOT
// reuse aero-mes's durable outbox (Outbox/MesGatewayActor, mes/outbox.hpp) — that machinery exists for
// MES's at-least-once durability requirement (012 §3); a generic HTTP utility node is best-effort by
// default (fire on its own lane, no durable retry) until a real use case demands more.
#pragma once

#include <array>
#include <string>
#include <string_view>

#include "aero/sdk/node.hpp"
#include "nlohmann/json.hpp"

namespace aero::nodes {

// url/method/headers_json are node config, bound once at deploy (N3) and held as owned strings so the
// StagedHttpRequest views into them stay valid for the node's lifetime — same reasoning as
// MesReportNode's line_/label_. The JSON body, in contrast, is built fresh from the working-set tags
// each Command — inherently allocating, the same documented exception JsonParseNode already takes
// (005 §7, compute_nodes.hpp) — so this node is not on the strict 0-alloc gate.
class HttpOutputNode final : public INode {
public:
    HttpOutputNode(std::string url, std::string method, std::string headers_json, int timeout_ms)
        : url_(std::move(url)), method_(std::move(method)), headers_json_(std::move(headers_json)),
          timeout_ms_(timeout_ms) {}

    NodeResult process(ProcessingContext& ctx) noexcept override {
        nlohmann::json body = nlohmann::json::object();
        for (const auto& t : ctx.tags) {
            body[std::string(t.name)] = t.value;
        }
        ctx.http_requests.push_back(StagedHttpRequest{url_, method_, headers_json_, timeout_ms_,
                                                       body.dump()});
        return NodeResult::Continue;
    }
    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr std::array<std::string_view, 5> kMethodOptions{"GET", "POST", "PUT", "PATCH",
                                                                     "DELETE"};
    static constexpr std::array<FieldSpec, 4> kFields{{
        {.key = "url", .label = "URL", .type = FieldType::String, .required = true},
        {.key = "method", .label = "Method", .type = FieldType::Enum, .default_string = "POST",
         .enum_options = kMethodOptions},
        {.key = "headers", .label = "Headers", .type = FieldType::Object,
         .tier2_hint = "http-headers"},
        {.key = "timeout_ms", .label = "Timeout (ms)", .type = FieldType::Int,
         .default_number = 2000, .has_min = true, .min = 1},
    }};
    static constexpr NodeDescriptor kDesc{NodeCategory::Output, "aero.output.http", kFields};

private:
    std::string url_;
    std::string method_;
    std::string headers_json_;  // a flat {"Key":"Value",...} object, or "{}"
    int timeout_ms_;
};

}  // namespace aero::nodes
