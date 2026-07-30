// AeroEdge Phase-7 gate (014 §2 X1): cross-node actor messaging over Quark's REAL Transport seam.
//
// PATH USED: Quark's real `LoopbackTransport` + `LoopbackFabric` + `DistributedRouter` + HRW placement
// + `InProcessMembership` — the FULLY-WIRED in-process cross-node data path (Quark sample 08 /
// distribution_routing_test). This is the honest proof that AeroEdge's transport phase rides Quark's
// existing seam (X1: adapters, not a parallel runtime) rather than a bespoke router: two actors on
// DISTINCT NodeIds, one tells the other by key, the message crosses the transport and arrives on the
// far node with per-(from → target) FIFO intact (C1). No sockets, deterministic, exit-code-gated.
//
// (The offline C1 proof for a REORDERING substrate — which the loopback fabric is not — is
// tests/resequencer.cpp over the AeroEdge Resequencer; loopback already delivers a sender's frames
// FIFO by construction, so it needs no shim. Both together cover 014 §3.)
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/distribution.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {

// The cross-node message. QUARK_SERIALIZE (016) is required for any remotely-sent type and must sit in
// the SAME namespace as the type (ADL) — here, this anonymous namespace.
struct Ping {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Ping, (1, n));

// A minimal actor that records what it received, on its single executor lane (single-executor, I2).
struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Ping>;
    std::vector<int> got;
    std::atomic<int> count{0};
    void handle(const Ping& p) noexcept {
        got.push_back(p.n);
        count.fetch_add(1, std::memory_order_release);
    }
};

// One cluster node: engine + local router + membership + a Quark LoopbackTransport endpoint + the
// DistributedRouter tying them together, hosting a Sink at `sink_key`.
struct ClusterNode {
    detail::MessagePool pool{4096};
    Sink actor;
    std::unique_ptr<Activation> act;
    Engine<> eng{EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64}};
    LocalRouter local{eng.post_courier(), pool};
    InProcessMembership membership;
    LoopbackTransport transport;  // Quark's REAL in-process transport double (the seam under test)
    std::unique_ptr<DistributedRouter> dist;

    ClusterNode(NodeId self, LoopbackFabric& fabric, std::uint64_t sink_key)
        : membership(self, {NodeId{1}, NodeId{2}}), transport(fabric, self) {
        act = std::make_unique<Activation>(&actor, Sink::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<Sink>(sink_key), *act);
        dist = std::make_unique<DistributedRouter>(membership, local, transport);
        dist->template register_remote<Sink, Ping>();  // teach this node to decode+post inbound (Sink,Ping)
    }
};

bool wait_count(std::atomic<int>& c, int want) {
    for (std::uint64_t spins = 0; c.load(std::memory_order_acquire) < want; ++spins)
        if (spins > 5'000'000'000ULL) return false;
    return true;
}

}  // namespace

int main() {
    bool ok = true;

    // Placement is a deterministic pure function of membership — pick a key owned by node 2 (remote to
    // node 1's router) so node 1 → that key exercises the REMOTE cross-node path, not the local shortcut.
    InProcessMembership probe(NodeId{1}, {NodeId{1}, NodeId{2}});
    const MembershipView pv = probe.view();
    std::uint64_t key_on_2 = 0;
    for (std::uint64_t k = 1; k < 10'000 && !key_on_2; ++k)
        if (place(actor_id_of<Sink>(k), pv)->value == 2) key_on_2 = k;
    ok &= key_on_2 != 0;

    LoopbackFabric fabric;
    ClusterNode n1(NodeId{1}, fabric, /*unused local key*/ 999999);
    ClusterNode n2(NodeId{2}, fabric, key_on_2);  // node 2 hosts the Sink
    n1.eng.start();
    n2.eng.start();

    constexpr int N = 64;

    // Node 1 tells the node-2-owned Sink N pings. Each serializes (016), crosses the LoopbackTransport,
    // decodes on node 2, and posts to the Sink — a genuine cross-node actor→actor message.
    DistRef<Sink> ref = n1.dist->get<Sink>(key_on_2);
    ok &= !n1.dist->is_local(actor_id_of<Sink>(key_on_2));  // confirm this is the remote path
    const std::uint64_t wire_before = fabric.sends();
    for (int i = 0; i < N; ++i) ref.tell(Ping{i});

    ok &= wait_count(n2.actor.count, N);
    const std::uint64_t wire_used = fabric.sends() - wire_before;
    ok &= wire_used == static_cast<std::uint64_t>(N);  // every remote tell went through the transport

    // FIFO across the wire (C1): node 2 received 0,1,...,N-1 in order.
    bool ordered = static_cast<int>(n2.actor.got.size()) >= N;
    for (int i = 0; i < N && ordered; ++i)
        if (n2.actor.got[static_cast<std::size_t>(i)] != i) ordered = false;
    ok &= ordered;

    n1.eng.stop();
    n2.eng.stop();

    std::printf("cross-node (Quark LoopbackTransport + DistributedRouter): remote_key=%llu delivered=%d "
                "wire_frames=%llu (expected %d) FIFO=%s\n",
                (unsigned long long)key_on_2, n2.actor.count.load(), (unsigned long long)wire_used, N,
                ordered ? "intact" : "BROKEN");
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
