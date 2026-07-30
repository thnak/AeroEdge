// AeroEdge Phase-7 gate (014 §5/§6 R5, §8): the MQTT/gRPC offline GATE + the per-link selector.
//
// Two things this proves:
//   (1) HONEST GATE (X2/R5): NullMqttTransport / NullGrpcTransport have no broker/gRPC backend offline,
//       so start() returns the DOCUMENTED gate error (stable prefix) and send() records the gated
//       attempt without faking delivery. No vendored broker, no faked stream.
//   (2) PER-LINK POLICY (014 §8, X5): TransportSelector picks, for each peer, the most-preferred
//       transport BOTH the peer advertises and this node can drive — flow-controlled first (§7), broker
//       last; no shared transport ⇒ relay (nullptr). Deterministic, exit-code-gated. 0 = pass.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "aero/transport/grpc_transport.hpp"
#include "aero/transport/mqtt_transport.hpp"
#include "aero/transport/transport.hpp"
#include "quark/core/transport.hpp"

using namespace aero::transport;

int main() {
    bool ok = true;

    // --- (1) The offline gate: both Null adapters fail start() with the documented prefix. -----------
    NullMqttTransport mqtt(MqttTransport::Config{/*broker*/ "tls://plant-broker:8883"});
    NullGrpcTransport grpc(GrpcTransport::Config{/*target*/ "dns:///mesh:50051"});

    auto ms = mqtt.start();
    auto gs = grpc.start();
    const bool mqtt_gated = !ms && std::string_view(ms.error()).substr(0, kTransportGate.size()) == kTransportGate;
    const bool grpc_gated = !gs && std::string_view(gs.error()).substr(0, kTransportGate.size()) == kTransportGate;
    ok &= mqtt_gated && grpc_gated;
    std::printf("gate mqtt: %s\ngate grpc: %s\n", ms ? "(unexpectedly started)" : ms.error().c_str(),
                gs ? "(unexpectedly started)" : gs.error().c_str());

    // send() on a gated transport must NOT pretend success — it records the attempt.
    quark::MessageFrame f;
    mqtt.send(quark::NodeId{2}, f);
    mqtt.send(quark::NodeId{2}, quark::MessageFrame{});
    ok &= mqtt.gated_sends() == 2;

    // Topic-per-node scheme is well-formed (014 §5): inbox topic == prefix + nodeId.
    ok &= mqtt.inbox_topic(quark::NodeId{7}) == std::string("aero/xport/7");

    // --- (2) Per-link selection among AVAILABLE transports (014 §8). --------------------------------
    // This node can drive a loopback transport and (a gated) mqtt. It CANNOT drive tcp or grpc here.
    quark::LoopbackFabric fabric;
    LoopbackTransportAdapter loop(fabric, quark::NodeId{1});

    TransportSelector sel;
    sel.register_transport(loop);  // class Loopback
    sel.register_transport(mqtt);  // class Mqtt

    // Peer A advertises reachable=loopback,mqtt → prefer the flow-controlled loopback (§7 order).
    sel.set_peer_reachability(quark::NodeId{10}, {TransportClass::Mqtt, TransportClass::Loopback});
    // Peer B advertises reachable=mqtt only → the only shared transport is mqtt.
    sel.set_peer_reachability(quark::NodeId{11}, {TransportClass::Mqtt});
    // Peer C advertises reachable=tcp only → no shared transport → relay (nullptr, X5).
    sel.set_peer_reachability(quark::NodeId{12}, {TransportClass::Tcp});
    // Peer D advertises reachable=grpc,mqtt → grpc not driven here, so fall back to mqtt.
    sel.set_peer_reachability(quark::NodeId{13}, {TransportClass::Grpc, TransportClass::Mqtt});

    const bool a_loop = sel.select(quark::NodeId{10}) == static_cast<ITransport*>(&loop);
    const bool b_mqtt = sel.select(quark::NodeId{11}) == static_cast<ITransport*>(&mqtt);
    const bool c_relay = sel.select(quark::NodeId{12}) == nullptr;
    const bool d_mqtt = sel.select(quark::NodeId{13}) == static_cast<ITransport*>(&mqtt);
    const bool unknown_relay = sel.select(quark::NodeId{99}) == nullptr;  // unadvertised peer → relay
    ok &= a_loop && b_mqtt && c_relay && d_mqtt && unknown_relay;

    std::printf("select A(loop,mqtt)->%s  B(mqtt)->%s  C(tcp)->%s  D(grpc,mqtt)->%s\n",
                a_loop ? "loopback" : "WRONG", b_mqtt ? "mqtt" : "WRONG", c_relay ? "relay" : "WRONG",
                d_mqtt ? "mqtt" : "WRONG");

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
