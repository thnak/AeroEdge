// AeroEdge Transport — `MqttTransport` adapter SHAPE, gated offline (014 §5, X2/X3/X6/X7).
//
// WHAT THIS IS: the actor-frame-over-a-broker adapter for links where nodes cannot dial each other
// directly (NAT, plant firewalls, hub-and-spoke plant networks) but can all reach an MQTT broker.
// It is a CLIENT to an existing broker — AeroEdge never reimplements or replaces one (014 §4 B1 / X3).
// The default build links NO broker (B2); this header defines the adapter shape + an honest offline
// GATE (NullMqttTransport). No broker/paho/mosquitto is available offline, so there is nothing to fake.
//
// THE DESIGN it documents (a real build fills the client in behind this exact seam):
//   * Topic-per-node (014 §5): one inbound topic per destination node, `aero/xport/<toNodeId>`. The
//     MessageFrame header already carries the `target` ActorId, so ONE node-inbox topic suffices and
//     keeps per-publisher ordering to that topic. A node subscribes to its own inbox.
//   * QoS >= 1 ALWAYS (C3): QoS 0 is at-most-once → illegal for at-least-once actors. QoS-1 duplicates
//     are absorbed downstream (by the resequencer here + Quark 017 idempotency at the actor).
//   * Ordering shim (C1, MANDATORY): MQTT preserves order per-topic per-session but NOT across a
//     reconnect / clean-session. So this adapter COMPOSES aero::transport::Resequencer — stamp a
//     per-`(from → to)` seq on send, resequence + dedup on receive — restoring the FIFO Quark demands.
//     `send()` shows where the SequenceStamper stamps; `on_receive` shows where the Resequencer sits.
//   * Broker untrusted for authz (C5/X6): TLS to the broker protects the hop; actor authorization stays
//     Quark 020 principal propagation carried IN the frame. A compromised broker can delay/replay
//     (mitigated by seq + dedup) but cannot forge an authorized actor message.
//   * Backpressure caveat (X7 / §7): a broker DECOUPLES producer from consumer, so a cross-node STREAM
//     over MQTT does NOT deliver end-to-end backpressure to the original producer — credit stalls at the
//     broker, which then buffers/drops per its QoS/retention. MQTT is for discrete control-plane
//     messages + reachability, NOT the backpressured high-rate stream path (use TCP/gRPC for those).
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "aero/transport/resequencer.hpp"
#include "aero/transport/transport.hpp"

namespace aero::transport {

// The gate string every offline transport backend returns (014 gate, R5 honest-subset). Callers match
// on the stable prefix; the suffix names the specific backend/endpoint for diagnostics.
inline constexpr std::string_view kTransportGate =
    "transport backend not configured (014 gate: no offline broker/gRPC)";

// The adapter SHAPE. Abstract: a real build provides a concrete subclass wrapping an MQTT client; the
// offline build provides NullMqttTransport. Carries the §5 config + the resequencer/stamper the C1 shim
// needs, and implements name()/transport_class() so it drops into TransportSelector.
class MqttTransport : public ITransport {
public:
    struct Config {
        std::string broker_uri = "tcp://localhost:1883";  // tls://... in a real build (C5)
        int qos = 1;                                       // C3: QoS >= 1, never 0
        std::string topic_prefix = "aero/xport/";          // inbox topic per node: <prefix><toNodeId> (§5)
        std::size_t reorder_window = Resequencer<MessageFrame>::kDefaultWindow;  // §5 shim bound
    };

    explicit MqttTransport(Config cfg) : cfg_(std::move(cfg)) {}

    // Bring the client up: dial the broker (TLS), subscribe this node's own inbox topic. A real impl
    // connects; the offline Null impl returns the documented gate error — no faked broker (R5).
    [[nodiscard]] virtual std::expected<void, std::string> start() = 0;

    // The inbound topic this node subscribes to (014 §5 topic-per-node scheme).
    [[nodiscard]] std::string inbox_topic(NodeId self) const {
        return cfg_.topic_prefix + std::to_string(self.value);
    }

    [[nodiscard]] std::string_view name() const noexcept override { return "mqtt"; }
    [[nodiscard]] TransportClass transport_class() const noexcept override {
        return TransportClass::Mqtt;
    }

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

protected:
    Config cfg_;
};

// The honest offline GATE (R5, X2): no broker/client is linked offline, so start() fails with the
// documented gate error and send() is a no-op that RECORDS the gated attempt (never silently succeeds,
// never fakes delivery). on_receive registers nothing. This is the "NullMqttTransport returns a clear
// gate error" required by the phase — the resequencer/topic/QoS design above is real; only the broker
// backend is absent.
class NullMqttTransport final : public MqttTransport {
public:
    explicit NullMqttTransport(Config cfg = {}) : MqttTransport(std::move(cfg)) {}

    [[nodiscard]] std::expected<void, std::string> start() override {
        return std::unexpected(std::string(kTransportGate) + " [mqtt broker: " + cfg_.broker_uri + "]");
    }

    // No backend → cannot move a frame. Count the gated attempt; do NOT pretend it was delivered.
    void send(NodeId, MessageFrame) override { ++gated_sends_; }
    void on_receive(std::function<void(MessageFrame)>) override { /* no broker → no inbound */ }

    [[nodiscard]] std::uint64_t gated_sends() const noexcept { return gated_sends_; }

private:
    std::uint64_t gated_sends_ = 0;
};

}  // namespace aero::transport
