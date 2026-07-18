// AeroEdge Transport — `GrpcTransport` adapter SHAPE, gated offline (014 §6, X2/X6/X7).
//
// WHAT THIS IS: the typed-streaming-over-HTTP/2 adapter for firewall-friendly, service-mesh, or
// cross-datacenter links. No gRPC/protoc/HTTP2 stack is available offline, so this header defines the
// adapter shape + an honest offline GATE (NullGrpcTransport) — no faked stream (R5).
//
// THE DESIGN it documents (a real build fills the gRPC stub in behind this exact seam):
//   * ONE bidirectional gRPC stream per peer carries MessageFrames, mirroring Quark's
//     one-connection-per-peer TCP model. HTTP/2 stream ordering gives C1 (per-`(from → target)` FIFO)
//     ON A SINGLE STREAM for free — so, UNLIKE MqttTransport, GrpcTransport needs NO resequencer: the
//     transport substrate itself is order-preserving and flow-controlled. (If a deployment ever
//     multiplexed one logical link across MULTIPLE streams it would reintroduce reordering and then owe
//     the resequencer — single-stream-per-peer is the design choice that avoids it.)
//   * mTLS satisfies C5 (node↔node auth/encryption); the Quark 020 Principal still rides IN the frame
//     (X6) — the mesh secures the hop, Quark authorizes the actor message.
//   * gRPC deadlines map naturally to the propagated frame deadline (018, C2).
//   * Backpressure (X7 / §7): HTTP/2 flow control + app-level stream credit stall the SENDER, so a
//     cross-node stream over gRPC DOES preserve Quark's end-to-end backpressure (stall-not-drop) — a
//     flow-controlled transport, like TCP and unlike a broker. Good for high-rate streams that scale
//     across nodes; heavier than Quark's default TCP, so chosen per deployment (014 §6), not by default.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "aero/transport/mqtt_transport.hpp"  // kTransportGate (shared offline gate string)
#include "aero/transport/transport.hpp"

namespace aero::transport {

// The adapter SHAPE. Abstract: a real build provides a concrete subclass wrapping a gRPC bidi stub; the
// offline build provides NullGrpcTransport. NOTE the deliberate absence of a Resequencer member — a
// single HTTP/2 stream is order-preserving, so C1 holds without a shim (contrast MqttTransport).
class GrpcTransport : public ITransport {
public:
    struct Config {
        std::string target = "dns:///localhost:50051";  // gRPC name-resolver target (mTLS in real build)
        bool tls = true;                                 // C5: mTLS between peers
    };

    explicit GrpcTransport(Config cfg) : cfg_(std::move(cfg)) {}

    // Open the bidi stream to the peer (real impl); the offline Null impl returns the gate error (R5).
    [[nodiscard]] virtual std::expected<void, std::string> start() = 0;

    [[nodiscard]] std::string_view name() const noexcept override { return "grpc"; }
    [[nodiscard]] TransportClass transport_class() const noexcept override {
        return TransportClass::Grpc;
    }

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

protected:
    Config cfg_;
};

// The honest offline GATE (R5, X2): no gRPC stack offline → start() fails with the documented gate
// error; send() records the gated attempt without faking a stream; on_receive registers nothing.
class NullGrpcTransport final : public GrpcTransport {
public:
    explicit NullGrpcTransport(Config cfg = {}) : GrpcTransport(std::move(cfg)) {}

    [[nodiscard]] std::expected<void, std::string> start() override {
        return std::unexpected(std::string(kTransportGate) + " [grpc target: " + cfg_.target + "]");
    }

    void send(NodeId, MessageFrame) override { ++gated_sends_; }
    void on_receive(std::function<void(MessageFrame)>) override { /* no stream → no inbound */ }

    [[nodiscard]] std::uint64_t gated_sends() const noexcept { return gated_sends_; }

private:
    std::uint64_t gated_sends_ = 0;
};

}  // namespace aero::transport
