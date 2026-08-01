// AeroEdge Broker — bridging + a rule engine over `NativeBroker::on_publish()` (017 §6, REVISED scope).
//
// WHY THIS EXISTS: 017 §6 originally parked "bridging to Kafka/other brokers, rule engine" as
// out-of-scope ("EMQX's own paid-tier breadth; not this spec's problem to solve"). That call is
// revised: AeroEdge wants EMQX-PARITY BREADTH on bridging + rules, WITHOUT EMQX's BSL 1.1 license —
// which means REUSING what AeroEdge already has, not re-implementing EMQX's own approach:
//   * The bridge OUT side reuses aero-mes's `RestMesAdapter` idiom (httplib, bool/never-throw error
//     handling, mes/outbox.hpp's `IMesAdapter` shape) for an HTTP webhook sink, and the shared MQTT
//     wire codec (mqtt_codec.hpp, 017 §9 N3) for an MQTT sink — no new HTTP client, no new MQTT codec.
//   * The rule-match/filter side reuses `aero::nodes::ExprRuleNode` (008 §6's low-code expression DSL)
//     verbatim — no second expression grammar/parser. A broker rule is (topic filter, optional
//     ExprRuleNode, sink): the topic filter is `aero::broker::topic_matches` (native_broker.hpp, a
//     free function — this header does NOT depend on `NativeBroker` itself, only that function).
//
// WIRING (deliberately NOT this header's job): `BrokerRuleEngine::handle_publish` has EXACTLY the
// signature `NativeBroker::on_publish()` expects. Whoever owns the broker instance wires
// `broker.on_publish([&](auto t, auto p, auto q, const auto& props){ engine.handle_publish(t, p, q,
// props); });` — this header never touches `NativeBroker`'s socket/session internals. (017 M7.2 PR B:
// the 4th parameter, `props`, was added when on_publish()/IBridgeSink::publish() both grew a
// PublishProperties parameter — see those declarations' own comments.)
//
// Kafka/Pulsar bridges are OUT of scope here — those need new third-party client deps, separate future
// milestones (M8.1/M8.2, see 017 §6 Status). RabbitMQ shipped in M8 (`RabbitMqBridgeSink`,
// broker/rabbitmq_bridge_sink.hpp, over rabbitmq-c) as the one bridge in that family AeroEdge actually
// vendors. MQTT, HTTP-webhook, and RabbitMQ together cover the floor: republish to another broker
// (cloud/second AeroEdge node), to any HTTP-speaking downstream (a rules engine, a lake ingester), or to
// an AMQP 0-9-1 broker (a common on-prem industrial message bus).
//
// DESIGN CALL — `MqttBridgeSink` (documented per the task, not mechanical): `MqttClientTransport`
// (mqtt_client_transport.hpp) is a working MQTT 3.1.1 client, but its `send()`/`on_receive()` contract
// is HARD-WIRED to `quark::MessageFrame` (an 8-byte seq header + the inter-actor frame codec,
// resequencer, per-destination sequence counters) — none of which applies to republishing an arbitrary
// (topic, bytes, qos) PUBLISH from the broker. Shoehorning raw bytes through that contract would mean
// either faking a MessageFrame around a bridged payload (semantically wrong — a bridged PUBLISH is not
// an inter-actor frame) or growing MqttClientTransport a second, parallel raw-publish path (churn on a
// class this pass explicitly must not modify, and now serving two different jobs). Instead
// `MqttBridgeSink` is a SMALL, SELF-CONTAINED client built directly on `mqtt_codec.hpp` +
// `quark::pal::net` — the exact same primitives `NativeBroker` and `MqttClientTransport` both already
// use (017 §4/§9 N3) — implementing only CONNECT/CONNACK (once, at connect()) and PUBLISH (per
// publish() call). This is the "less invasive, cleaner" option the task called out: no changes to
// MqttClientTransport, no MessageFrame coupling, ~80 lines of wire code that already exists elsewhere
// in spirit (mirrors MqttClientTransport's own mqtt_connect()/dial()/parse_broker_uri()) rather than
// duplicated logic bolted onto a class whose contract doesn't fit.
//
// QoS-1 PUBACK scope: `MqttBridgeSink::publish()` returns true once the PUBLISH is WRITTEN to the
// downstream socket — it does not wait for or track the far broker's PUBACK. This mirrors
// MqttClientTransport's own documented gap ("no retransmit-on-missing-PUBACK", that file's banner) —
// waiting synchronously for an ack the far end might never send would let a stalled/malicious
// downstream hang the bridge forever, which nothing on this seam may do (never block indefinitely).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pal/net.hpp"  // quark::pal::* — fd_t, tcp_connect/recv_some/send_some/...

#if !defined(_WIN32)
#include <netdb.h>  // getaddrinfo/freeaddrinfo (Windows: transitively via pal/net.hpp's ws2tcpip.h)
#endif

#include "aero/broker/native_broker.hpp"  // aero::broker::topic_matches (free function only — no
                                           // dependency on NativeBroker's session/socket internals)
#include "aero/nodes/expr_rule_node.hpp"  // aero::nodes::ExprRuleNode — reused, not reimplemented (008 §6)
#include "aero/pal/net_dial.hpp"
#include "aero/pal/poll.hpp"
#include "aero/sdk/processing_context.hpp"
#include "aero/transport/mqtt_codec.hpp"  // shared MQTT 3.1.1 wire codec (017 §9 N3)
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace aero::broker {

// ---- small, self-contained helpers (no new third-party deps) --------------------------------------
namespace bridge_detail {

// A payload is sent as a JSON string verbatim (readable on the far side) when it is valid UTF-8 — the
// common case for AeroEdge device telemetry (JSON/text). Binary payloads are base64-encoded so no byte
// is lost or mangled crossing the JSON boundary (`HttpWebhookBridgeSink::publish`, below).
[[nodiscard]] inline bool is_valid_utf8(std::span<const std::byte> data) noexcept {
    std::size_t i = 0;
    const std::size_t n = data.size();
    while (i < n) {
        const auto b0 = std::to_integer<std::uint8_t>(data[i]);
        std::size_t extra = 0;
        if ((b0 & 0x80) == 0x00) extra = 0;
        else if ((b0 & 0xE0) == 0xC0) extra = 1;
        else if ((b0 & 0xF0) == 0xE0) extra = 2;
        else if ((b0 & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= n) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((std::to_integer<std::uint8_t>(data[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

[[nodiscard]] inline std::string base64_encode(std::span<const std::byte> data) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const auto n = static_cast<std::uint32_t>((std::to_integer<std::uint32_t>(data[i]) << 16) |
                                                    (std::to_integer<std::uint32_t>(data[i + 1]) << 8) |
                                                     std::to_integer<std::uint32_t>(data[i + 2]));
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
    }
    const std::size_t rem = data.size() - i;
    if (rem == 1) {
        const auto n = static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(data[i]) << 16);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const auto n = static_cast<std::uint32_t>((std::to_integer<std::uint32_t>(data[i]) << 16) |
                                                    (std::to_integer<std::uint32_t>(data[i + 1]) << 8));
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

}  // namespace bridge_detail

// Connection knobs handed to `IBridgeSink::connect()`. Fields are consumed per-sink-kind (mirrors
// `aero::mes::MesConfig`'s own "some fields used by one adapter kind" shape, mes/mes.hpp):
//   MqttBridgeSink       — `endpoint` only, as "tcp://host:port" (the downstream broker to dial).
//   HttpWebhookBridgeSink — `endpoint`+`port` (the HTTP host:port), `path`, optional `token`.
//   RabbitMqBridgeSink   — `endpoint` (bare host) + `port` (0 -> default 5672), `username`/`password`
//                          (SASL PLAIN, default "guest"/"guest"), `vhost` (default "/"), `exchange`
//                          (default "" -> the default exchange; `topic` is always the routing key).
struct BridgeConfig {
    std::string endpoint;                    // MQTT: "tcp://host:port". HTTP/RabbitMQ: bare host.
    int port = 0;                            // HTTP webhook / RabbitMQ port only (RabbitMQ: 0 -> 5672).
    std::string path = "/bridge";            // HTTP webhook POST path only.
    std::string token;                       // HTTP webhook: optional bearer token (Authorization).
    std::string client_id = "aero-bridge";   // MQTT: the CONNECT client identifier.
    std::string username = "guest";          // RabbitMQ: SASL PLAIN username.
    std::string password = "guest";          // RabbitMQ: SASL PLAIN password.
    std::string vhost = "/";                 // RabbitMQ: virtual host.
    std::string exchange;                    // RabbitMQ: exchange name ("" -> the default exchange).
};

// The seam AeroEdge owns for "republish a broker PUBLISH somewhere else" — same spirit as
// `aero::mes::IMesAdapter` (mes/outbox.hpp): a small virtual interface, adapters implement it, NEVER
// throws, reports failure as a bool return. Deliberately narrower than IMesAdapter (no inbound command
// sink — a bridge sink is outbound-only by definition here).
class IBridgeSink {
public:
    virtual ~IBridgeSink() = default;

    // Connect/configure the sink. Called once by whoever owns it, before any publish(). A bad
    // config/unreachable target is a `false` return, never an exception (mirrors IMesAdapter::connect).
    virtual bool connect(const BridgeConfig& cfg) = 0;

    // Republish one broker PUBLISH downstream. Called from `BrokerRuleEngine::handle_publish` on
    // whatever thread `NativeBroker::on_publish()` fires from (a session's reader thread, 017 Phase 1)
    // — implementations must tolerate that thread and must never throw or block indefinitely.
    // 017 M7.2 PR B: gained a 4th parameter, `props` (Response Topic/Correlation Data/User Properties) —
    // no default argument: defaults on a pure virtual don't affect dynamic dispatch, and every call site
    // (BrokerRuleEngine::handle_publish, below) has a real PublishProperties value to pass.
    virtual bool publish(std::string_view topic, std::span<const std::byte> payload, std::uint8_t qos,
                        const PublishProperties& props) = 0;
};

// ---- MqttBridgeSink: republish to ANOTHER MQTT broker -----------------------------------------------
// A thin, self-contained MQTT 3.1.1 client — CONNECT once, then PUBLISH per call. See the file banner's
// "DESIGN CALL" for why this is not built on top of `MqttClientTransport`. Built on the exact same
// `mqtt_codec.hpp` + `quark::pal::net` primitives NativeBroker/MqttClientTransport already use.
class MqttBridgeSink final : public IBridgeSink {
public:
    MqttBridgeSink() = default;
    ~MqttBridgeSink() override { close(); }

    MqttBridgeSink(const MqttBridgeSink&) = delete;
    MqttBridgeSink& operator=(const MqttBridgeSink&) = delete;

    bool connect(const BridgeConfig& cfg) override {
        std::lock_guard<std::mutex> g(mu_);
        close_locked();

        std::string host;
        std::uint16_t port = 0;
        if (!parse_broker_uri(cfg.endpoint, host, port)) return false;

        fd_ = dial(host, port);
        if (fd_ == quark::pal::invalid_fd) return false;
        running_.store(true, std::memory_order_release);

        if (!mqtt_connect(cfg.client_id.empty() ? "aero-bridge" : cfg.client_id)) {
            close_locked();
            return false;
        }
        return true;
    }

    // 017 M7.2 PR B: `props` is accepted but UNUSED — this sink is hardcoded to protocol level 0x04
    // (MQTT 3.1.1, mqtt_connect() below) which has no Properties concept at all to carry Response
    // Topic/Correlation Data/User Properties on. Upgrading this sink to speak MQTT 5 is out of scope for
    // this PR (see the file banner's "DESIGN CALL" for why this sink is a small, self-contained client
    // rather than layered on MqttClientTransport — the same reasoning applies to not silently growing it
    // a v5 mode as a side effect of this PR).
    bool publish(std::string_view topic, std::span<const std::byte> payload, std::uint8_t qos,
                const PublishProperties& /*props*/) override {
        std::lock_guard<std::mutex> g(mu_);
        if (fd_ == quark::pal::invalid_fd) return false;

        const std::uint8_t q = qos > 1 ? 1 : qos;  // ceiling matches NativeBroker's own v1 QoS scope
        std::vector<std::byte> vh;
        aero::transport::mqtt::put_str(vh, topic);
        if (q > 0) aero::transport::mqtt::put_u16_be(vh, next_packet_id());
        vh.insert(vh.end(), payload.begin(), payload.end());
        const auto flags = static_cast<std::uint8_t>(q << 1);
        // Fire-and-forget beyond the write for QoS 1 (see file banner) — true means "written", not
        // "acked by the far broker".
        return aero::transport::mqtt::write_packet(fd_, static_cast<std::byte>(0x30 | flags), vh);
    }

    void close() {
        std::lock_guard<std::mutex> g(mu_);
        close_locked();
    }

private:
    void close_locked() {
        running_.store(false, std::memory_order_release);
        if (fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(fd_);
            fd_ = quark::pal::invalid_fd;
        }
    }

    // Mirrors MqttClientTransport::parse_broker_uri (private there, so re-expressed here rather than
    // reused — it's five lines with no shared state to factor out).
    static bool parse_broker_uri(std::string_view uri, std::string& host, std::uint16_t& port) {
        constexpr std::string_view kPrefix = "tcp://";
        if (uri.substr(0, kPrefix.size()) != kPrefix) return false;
        uri.remove_prefix(kPrefix.size());
        const auto colon = uri.rfind(':');
        if (colon == std::string_view::npos) return false;
        host = std::string(uri.substr(0, colon));
        const std::string ps(uri.substr(colon + 1));
        if (ps.empty()) return false;
        port = static_cast<std::uint16_t>(std::stoul(ps));
        return !host.empty() && port != 0;
    }

    static constexpr int kConnectTimeoutMs = 5000;

    // Shared with MqttClientTransport::dial() via aero/pal/net_dial.hpp's dial_tcp() (was duplicated
    // here verbatim — see that header's banner; same timeout, same per-candidate retry shape).
    static quark::pal::fd_t dial(const std::string& host, std::uint16_t port) {
        auto r = aero::pal::dial_tcp(host, port, kConnectTimeoutMs);
        return r ? *r : quark::pal::invalid_fd;
    }

    // Blocking CONNECT → CONNACK, exactly once at connect() (same "poll until running_ flips or the
    // peer answers/closes" posture MqttClientTransport::mqtt_connect() already uses — a peer that
    // accepts the TCP connection and never speaks MQTT hangs connect() until the caller closes it,
    // matching that existing precedent rather than inventing a new bounded-wait primitive here).
    bool mqtt_connect(const std::string& client_id) {
        std::vector<std::byte> vh;
        aero::transport::mqtt::put_str(vh, "MQTT");
        vh.push_back(std::byte{0x04});  // protocol level 4 == MQTT 3.1.1
        vh.push_back(std::byte{0x02});  // connect flags: clean session
        aero::transport::mqtt::put_u16_be(vh, 60);
        aero::transport::mqtt::put_str(vh, client_id);
        if (!aero::transport::mqtt::write_packet(fd_, std::byte{0x10}, vh)) return false;

        auto pkt = aero::transport::mqtt::read_packet(fd_, running_);
        return pkt.has_value() && (pkt->type_flags & 0xF0) == 0x20 && pkt->body.size() >= 2 &&
               std::to_integer<std::uint8_t>(pkt->body[1]) == 0;
    }

    std::uint16_t next_packet_id() {
        const std::uint16_t id = packet_id_.fetch_add(1, std::memory_order_relaxed);
        return id == 0 ? packet_id_.fetch_add(1, std::memory_order_relaxed) : id;  // MQTT packet id != 0
    }

    std::mutex mu_;  // serializes connect()/publish()/close() — one PUBLISH on the wire at a time
    quark::pal::fd_t fd_ = quark::pal::invalid_fd;
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> packet_id_{1};
};

// ---- HttpWebhookBridgeSink: POST to any HTTP-speaking downstream ------------------------------------
// Exactly RestMesAdapter's own pattern (mes/rest_mes_adapter.hpp): a fresh httplib::Client bound at
// connect(), lazy-connect (the first publish() is the real reachability probe), bool/status-code error
// mapping instead of throwing.
class HttpWebhookBridgeSink final : public IBridgeSink {
public:
    bool connect(const BridgeConfig& cfg) override {
        std::lock_guard<std::mutex> lock(mtx_);
        cfg_ = cfg;
        cli_ = std::make_unique<httplib::Client>(cfg.endpoint, cfg.port);
        cli_->set_connection_timeout(2, 0);
        cli_->set_read_timeout(3, 0);
        cli_->set_keep_alive(true);
        return true;
    }

    bool publish(std::string_view topic, std::span<const std::byte> payload, std::uint8_t qos,
                const PublishProperties& props) override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!cli_) return false;

        nlohmann::json body;
        body["topic"] = std::string(topic);
        body["qos"] = qos;
        // UTF-8 passthrough for readable text/JSON payloads; base64 for binary (see bridge_detail).
        if (bridge_detail::is_valid_utf8(payload)) {
            body["payload"] = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
            body["payload_encoding"] = "utf8";
        } else {
            body["payload"] = bridge_detail::base64_encode(payload);
            body["payload_encoding"] = "base64";
        }

        // 017 M7.2 PR B: Response Topic/Correlation Data/User Properties, added to the outgoing JSON body
        // when present. Correlation Data is base64-encoded — it's opaque binary by definition, no
        // ambiguity requiring a paired `*_encoding` field the way `payload` needs one (payload can
        // legitimately BE readable UTF-8 text; correlation data never claims to be). User Properties is a
        // JSON ARRAY of {key, value} objects, not an object keyed by name — MQTT 5 permits duplicate keys
        // and a JSON object would silently collapse them.
        if (props.response_topic) body["response_topic"] = *props.response_topic;
        if (props.correlation_data)
            body["correlation_data"] = bridge_detail::base64_encode(*props.correlation_data);
        if (!props.user_properties.empty()) {
            auto& arr = body["user_properties"] = nlohmann::json::array();
            for (const auto& [k, v] : props.user_properties) arr.push_back({{"key", k}, {"value", v}});
        }

        httplib::Headers headers;
        if (!cfg_.token.empty()) headers.emplace("Authorization", "Bearer " + cfg_.token);

        auto res = cli_->Post(cfg_.path.empty() ? "/" : cfg_.path, headers, body.dump(), "application/json");
        if (!res) return false;                              // transport error
        return res->status >= 200 && res->status < 300;
    }

private:
    std::mutex mtx_;
    BridgeConfig cfg_{};
    std::unique_ptr<httplib::Client> cli_;
};

// ---- rule engine: topic filter + reused ExprRuleNode + a sink ---------------------------------------
// One bridge rule: a topic filter (`aero::broker::topic_matches`, native_broker.hpp) gates which
// PUBLISHes are even considered; an OPTIONAL compiled `ExprRuleNode` further gates on payload content
// (empty `expr_text` at registration ⇒ the rule fires on topic match alone); the sink receives every
// PUBLISH that clears both gates.
struct BrokerRule {
    std::string topic_filter;
    std::optional<aero::nodes::ExprRuleNode> expr;  // unset == "no content gate, topic match is enough"
    std::shared_ptr<IBridgeSink> sink;
};

// Evaluates BrokerRules against every PUBLISH the broker hands it. Reuses `ExprRuleNode` (008 §6) for
// the content gate rather than a second expression engine (per 017's revised scope: EMQX-parity
// breadth via REUSE, not a bespoke rule VM).
//
// EXPR ↔ MQTT MAPPING (a real design call, documented): ExprRuleNode's grammar (expr_rule_node.hpp) is
// PURELY NUMERIC — `tag("name")`/a bare identifier read a named double from `ProcessingContext::tags`;
// there is no string-literal type (a quoted token IS a tag reference, not a constant to compare
// against). So the natural, honest mapping is: the MQTT TOPIC is handled entirely by `topic_matches`
// (string routing, its own well-suited job) — it is NOT re-exposed as an expr tag, since there is no
// way to compare it against a literal in this grammar anyway. The PAYLOAD, if (and only if) it parses
// as a JSON object, has its top-level number/boolean fields flattened into tags by key name (e.g.
// `{"temperature": 91.4}` ⇒ `tag("temperature")`), so an expr like `tag("temperature") > 90` behaves
// exactly as 008 §6 intends for a flow-level threshold rule, now over broker traffic. QoS is exposed as
// `tag("qos")`. A non-JSON (or non-object) payload yields NO fields — any tag reference then reads 0.0
// per ExprRuleNode's own "missing tag reads as 0.0" semantics (never a crash, never a throw), so an expr
// referencing payload fields on non-JSON traffic is simply falsy, exactly per this task's spec.
class BrokerRuleEngine {
public:
    // Compiles `expr_text` ONCE, here (008 §6 parse-once discipline, extended to broker rules) — never
    // per-PUBLISH. `expr_text` empty ⇒ the rule has no content gate (topic match alone fires it).
    // Returns false (and, if `error_out` is given, the parser's message) on a malformed expression —
    // rejected at registration, mirroring how a flow's deploy path rejects a bad ExprRuleNode config.
    bool add_rule(std::string topic_filter, std::string_view expr_text, std::shared_ptr<IBridgeSink> sink,
                  std::string* error_out = nullptr) {
        BrokerRule rule;
        rule.topic_filter = std::move(topic_filter);
        rule.sink = std::move(sink);
        if (!expr_text.empty()) {
            auto prog = aero::nodes::ExprRuleNode::compile(expr_text);
            if (!prog.ok) {
                if (error_out) *error_out = prog.error;
                return false;
            }
            rule.expr.emplace(std::move(prog), std::string(kFiredEventType));
        }
        rules_.push_back(std::move(rule));
        return true;
    }

    // EXACTLY `NativeBroker::on_publish()`'s callback signature (017 Phase 1) — wire this method
    // directly: `broker.on_publish([&](auto t, auto p, auto q, const auto& props){ engine.handle_publish(t,
    // p, q, props); });`. Never throws: a malformed/non-JSON payload only makes payload-derived tags read
    // 0.0 (above), never aborts the publish path a device is relying on. 017 M7.2 PR B: `props` is
    // forwarded to `IBridgeSink::publish()` unchanged — this engine doesn't itself act on Response
    // Topic/Correlation Data/User Properties, it's purely a pass-through to whichever sink a rule targets.
    void handle_publish(std::string_view topic, std::span<const std::byte> payload, std::uint8_t qos,
                        const PublishProperties& props) noexcept {
        for (auto& rule : rules_) {
            if (!topic_matches(rule.topic_filter, topic)) continue;
            if (rule.expr && !fires(*rule.expr, payload, qos)) continue;
            if (rule.sink) (void)rule.sink->publish(topic, payload, qos, props);
        }
    }

    [[nodiscard]] std::size_t rule_count() const noexcept { return rules_.size(); }

private:
    static constexpr std::string_view kFiredEventType = "aero.broker.rule.fired";

    static bool fires(aero::nodes::ExprRuleNode& expr, std::span<const std::byte> payload,
                      std::uint8_t qos) noexcept {
        ProcessingContext ctx;
        std::vector<std::string> names_storage;  // owns the key strings Tag::name views (must outlive
                                                   // the process() call below, and MUST NOT be appended
                                                   // to again once tags start referencing it — see
                                                   // populate_tags' single-pass-then-freeze ordering).
        populate_tags(ctx, payload, qos, names_storage);
        return expr.process(ctx) == NodeResult::Stop;  // Stop == the expr evaluated non-zero (008 §6)
    }

    static void populate_tags(ProcessingContext& ctx, std::span<const std::byte> payload, std::uint8_t qos,
                              std::vector<std::string>& names_storage) noexcept {
        ctx.tags.clear();
        std::vector<double> values;
        try {
            const std::string_view sv(reinterpret_cast<const char*>(payload.data()), payload.size());
            auto j = nlohmann::json::parse(sv, nullptr, /*allow_exceptions=*/false);
            if (!j.is_discarded() && j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) {
                    if (it.value().is_number()) {
                        names_storage.push_back(it.key());
                        values.push_back(it.value().get<double>());
                    } else if (it.value().is_boolean()) {
                        names_storage.push_back(it.key());
                        values.push_back(it.value().get<bool>() ? 1.0 : 0.0);
                    }
                }
            }
        } catch (...) {
            // Never let a malformed payload escape as an exception onto the broker's session thread —
            // fall through with whatever fields were already collected (possibly none).
        }
        names_storage.push_back("qos");
        values.push_back(static_cast<double>(qos));

        // names_storage is now frozen (no further push_back) — safe to take stable string_views into it.
        ctx.tags.reserve(names_storage.size());
        for (std::size_t i = 0; i < names_storage.size(); ++i)
            ctx.tags.push_back(Tag{std::string_view(names_storage[i]), values[i]});
    }

    std::vector<BrokerRule> rules_;
};

}  // namespace aero::broker
