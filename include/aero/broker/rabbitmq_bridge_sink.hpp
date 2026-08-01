// AeroEdge M8 (017 §6 Status) — RabbitMqBridgeSink: a fourth `aero::broker::IBridgeSink`
// (broker/bridge.hpp) implementation, republishing a broker PUBLISH to a RabbitMQ (AMQP 0-9-1) exchange
// over rabbitmq-c (github.com/alanxz/rabbitmq-c, MIT), the standard open-source C AMQP client library.
//
// POSTURE — mirrors `MqttBridgeSink` EXACTLY (bridge.hpp's own banner), not `HttpWebhookBridgeSink`'s
// lazy-connect: `connect()` eagerly dials + logs in + opens a channel; `publish()` fails fast on any
// prior failure and marks the sink disconnected the moment a publish attempt itself fails — there is NO
// internal reconnect loop here. A reconnect-on-loss policy is a driver-specific obligation (006 §8), not
// a bridge-sink one (bridge.hpp's own file banner makes the same call for MqttBridgeSink).
//
// v1 SCOPE (explicit, not an oversight, matches this project's other vendored-dependency v1 cuts):
// PUBLISH only (no AMQP consumer/inbound direction — a bridge sink is outbound-only by IBridgeSink's own
// contract), no TLS/SASL-EXTERNAL (SASL PLAIN only, matches OPC-UA v1's "no security policy" precedent,
// opcua_driver.hpp's own banner), no publisher confirms (`amqp_confirm_select` — publish() reports "the
// PUBLISH was accepted by rabbitmq-c's frame writer", not "the broker persisted it", mirroring
// MqttBridgeSink's own documented QoS-1-write-not-ack gap), no exchange auto-declare (the configured
// `BridgeConfig::exchange` is assumed to already exist on the broker, same as MqttBridgeSink assumes a
// reachable downstream broker rather than provisioning one).
//
// COMPILED-OUT BUILDS (AERO_ENABLE_RABBITMQ=OFF, root CMakeLists.txt): AERO_RABBITMQ_ENABLED is then
// undefined and this header compiles a stub with the IDENTICAL class/method surface whose
// `connect()`/`publish()` always return `false` — mirrors `aero/drivers/opcua_driver.hpp`'s "not compiled
// in" stub pattern exactly, so code that unconditionally includes this header still builds either way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "aero/broker/bridge.hpp"  // aero::broker::{IBridgeSink, BridgeConfig}

#if defined(AERO_RABBITMQ_ENABLED) && AERO_RABBITMQ_ENABLED

#include <cstring>

#if !defined(_WIN32)
#include <csignal>  // SIGPIPE — see kIgnoreSigpipe below
#endif

#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/framing.h>
#include <rabbitmq-c/tcp_socket.h>

namespace aero::broker {

// RabbitMqBridgeSink — a thin client over rabbitmq-c: `amqp_login` + `amqp_channel_open` once at
// connect(), `amqp_basic_publish` per publish() call. See file banner for the full v1 scope statement.
class RabbitMqBridgeSink final : public IBridgeSink {
public:
    RabbitMqBridgeSink() {
#if !defined(_WIN32)
        // rabbitmq-c does not reliably guard every send() against SIGPIPE on all POSIX platforms — a
        // broken/closed connection (exactly what test scenario (c) forces) can otherwise deliver SIGPIPE
        // to this process, whose default disposition is to terminate it. Ignoring it once here is the
        // documented workaround (rabbitmq-c's own README/FAQ); EPIPE is still reported to the caller via
        // the normal `amqp_basic_publish` return code either way. Windows has no SIGPIPE.
        static const bool kIgnoreSigpipe = [] { std::signal(SIGPIPE, SIG_IGN); return true; }();
        (void)kIgnoreSigpipe;
#endif
    }
    ~RabbitMqBridgeSink() override { close(); }

    RabbitMqBridgeSink(const RabbitMqBridgeSink&) = delete;
    RabbitMqBridgeSink& operator=(const RabbitMqBridgeSink&) = delete;

    // Eager connect: TCP dial (bounded, 5s — matches this project's dial_tcp default, aero/pal/net_dial.
    // hpp) -> SASL PLAIN login -> open channel 1. Any failure at any step cleans up fully and returns
    // `false` — never throws, never leaks the connection (mirrors MqttBridgeSink::connect()).
    bool connect(const BridgeConfig& cfg) override {
        std::lock_guard<std::mutex> g(mu_);
        close_locked();

        if (cfg.endpoint.empty()) return false;
        const int port = cfg.port != 0 ? cfg.port : kDefaultPort;

        conn_ = amqp_new_connection();
        if (!conn_) return false;

        amqp_socket_t* socket = amqp_tcp_socket_new(conn_);
        if (!socket) {
            amqp_destroy_connection(conn_);
            conn_ = nullptr;
            return false;
        }

        struct timeval timeout{};
        timeout.tv_sec = kConnectTimeoutMs / 1000;
        timeout.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
        if (amqp_socket_open_noblock(socket, cfg.endpoint.c_str(), port, &timeout) != AMQP_STATUS_OK) {
            amqp_destroy_connection(conn_);
            conn_ = nullptr;
            return false;
        }

        const std::string user = cfg.username.empty() ? "guest" : cfg.username;
        const std::string pass = cfg.password.empty() ? "guest" : cfg.password;
        const std::string vhost = cfg.vhost.empty() ? "/" : cfg.vhost;

        // SASL PLAIN varargs: (const char* username, const char* password) — C varargs, NOT
        // compiler-checked (rabbitmq-c's own documented contract for AMQP_SASL_METHOD_PLAIN).
        const amqp_rpc_reply_t login_reply =
            amqp_login(conn_, vhost.c_str(), /*channel_max*/ 0, /*frame_max*/ 131072, /*heartbeat*/ 0,
                       AMQP_SASL_METHOD_PLAIN, user.c_str(), pass.c_str());
        if (login_reply.reply_type != AMQP_RESPONSE_NORMAL) {
            amqp_destroy_connection(conn_);
            conn_ = nullptr;
            return false;
        }

        // amqp_channel_open's own return value does not reliably signal failure — the documented idiom
        // (rabbitmq-c's own examples) is to always follow an RPC step with amqp_get_rpc_reply().
        amqp_channel_open(conn_, kChannel);
        const amqp_rpc_reply_t chan_reply = amqp_get_rpc_reply(conn_);
        if (chan_reply.reply_type != AMQP_RESPONSE_NORMAL) {
            amqp_destroy_connection(conn_);
            conn_ = nullptr;
            return false;
        }

        cfg_ = cfg;
        connected_ = true;
        return true;
    }

    // Fails fast if not connected (mirrors MqttBridgeSink::publish()'s `fd_ == invalid_fd` check) — no
    // reconnect attempt here. Builds `amqp_basic_properties_t` with just the delivery-mode flag (qos>=1
    // -> persistent(2), qos==0 -> non-persistent(1)); `topic` is the routing key, `payload` is sent as
    // raw bytes (NOT `amqp_cstring_bytes` — it is binary, not necessarily NUL-terminated). A publish
    // failure (broken connection, detected via `amqp_basic_publish`'s own return code — the standard
    // rabbitmq-c signal for a write-level failure) marks this sink disconnected so every subsequent
    // publish() fails fast too, never attempting I/O against a connection already known to be dead.
    // 017 M7.2 PR B: `props` is accepted but UNUSED for now — a real AMQP-native mapping exists
    // (correlation_data -> amqp_basic_properties_t::correlation_id, response_topic -> reply_to,
    // user_properties -> headers table) but wiring it up is good follow-on work, not this PR.
    bool publish(std::string_view topic, std::span<const std::byte> payload, std::uint8_t qos,
                const PublishProperties& /*props*/) override {
        std::lock_guard<std::mutex> g(mu_);
        if (!connected_ || !conn_) return false;

        amqp_basic_properties_t props;
        std::memset(&props, 0, sizeof(props));
        props._flags = AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.delivery_mode = qos >= 1 ? 2 : 1;  // 2 = persistent, 1 = non-persistent

        const amqp_bytes_t exchange = amqp_cstring_bytes(cfg_.exchange.c_str());
        amqp_bytes_t routing_key{};
        routing_key.len = topic.size();
        routing_key.bytes = const_cast<char*>(topic.data());
        amqp_bytes_t body{};
        body.len = payload.size();
        body.bytes = const_cast<std::byte*>(payload.data());

        const int rc = amqp_basic_publish(conn_, kChannel, exchange, routing_key, /*mandatory*/ 0,
                                           /*immediate*/ 0, &props, body);
        if (rc != AMQP_STATUS_OK) {
            connected_ = false;  // fail fast on every subsequent call — no reconnect loop here
            return false;
        }
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> g(mu_);
        close_locked();
    }

private:
    static constexpr int kDefaultPort = 5672;
    static constexpr int kConnectTimeoutMs = 5000;
    static constexpr amqp_channel_t kChannel = 1;

    // Best-effort, never throws: a channel/connection that's already broken (the far end vanished) simply
    // fails these RPCs, which is fine to ignore here — the goal is "don't leak the connection_state_t",
    // not "prove the broker acknowledged a graceful close".
    void close_locked() noexcept {
        if (conn_) {
            if (connected_) {
                amqp_channel_close(conn_, kChannel, AMQP_REPLY_SUCCESS);
                amqp_connection_close(conn_, AMQP_REPLY_SUCCESS);
            }
            amqp_destroy_connection(conn_);
            conn_ = nullptr;
        }
        connected_ = false;
    }

    std::mutex mu_;  // serializes connect()/publish()/close() — one PUBLISH on the wire at a time
    BridgeConfig cfg_{};
    amqp_connection_state_t conn_ = nullptr;
    bool connected_ = false;
};

}  // namespace aero::broker

#else  // !AERO_RABBITMQ_ENABLED — identical call-site surface, honest "not compiled in" gate (mirrors
       // aero/drivers/opcua_driver.hpp's stub pattern).

namespace aero::broker {

class RabbitMqBridgeSink final : public IBridgeSink {
public:
    bool connect(const BridgeConfig& /*cfg*/) override { return false; }
    bool publish(std::string_view /*topic*/, std::span<const std::byte> /*payload*/,
                 std::uint8_t /*qos*/, const PublishProperties& /*props*/) override {
        return false;
    }
    void close() {}  // matches the real implementation's public surface (never anything to close here)
};

}  // namespace aero::broker

#endif  // AERO_RABBITMQ_ENABLED
