// AeroEdge Phase-2 gate — the ingestion path end-to-end (spec 006 §2).
//
// A GeneratorDriver (producer thread) pushes N frames into a Quark 024 StreamChannel; a consumer
// thread drains it in budgeted batches and runs each frame through an EdgeActor's compiled flow
// (Source -> Scale(2) -> Sum). This is exactly the 006 §2 diagram — driver as producer, a flow as the
// consumer body — with the two ends on SEPARATE threads so the SPSC cursors are genuinely exercised
// cross-core (real work for TSan). We assert the stream's correctness properties (mirroring Quark
// sample 06, but ending in a flow):
//   * FIFO      — frame ids arrive strictly in order (inversions == 0)
//   * no loss   — every id in [0,N) processed exactly once (missing == 0)
//   * no dup    — none processed twice (dup == 0)
//   * integrity — each frame's flow output == id*2 (the payload flowed intact through the flow)
//   * bounded   — occupancy never exceeds the ring capacity (backpressure held)
//
// Consumption path: BRIDGED (see report). The Engine does not yet route a stream descriptor through
// its worker loop (that is an explicit Quark 024 seam — stream_activation.hpp header), and
// system.open_stream(actor_id, transport_ep) addressing is a 006/010 seam. So we use the PROVEN
// standalone primitive from Quark sample 06 — StreamChannel + StreamBatch — with a driver as the
// single producer (via the real 024 single-producer token) and the actor's flow as the drain body.
// Backpressure lives in the StreamChannel credit between the two threads (006 §2/§3), never a mailbox.
//
// Frame lifetime (006 §4, D5): StreamBatch::next() hands a const Frame* viewing the LIVE ring slot;
// the flow runs BEFORE batch.retire(), so ctx.frame views the pinned slot for the flow's duration and
// credit returns only after the flow has consumed it. Exit code 0 = OK (ctest gate).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <thread>
#include <vector>

#include "aero/core/edge_actor.hpp"
#include "aero/drivers/generator_driver.hpp"
#include "aero/nodes/builtin_nodes.hpp"
#include "quark/core/stream_activation.hpp"

using namespace quark;

// A minimal EdgeActor: EdgeActorBase supplies the Command->Flow->Event glue; we drive its flow
// directly from the drain thread (the sole executor of this actor — single-executor invariant, I2).
struct FlowActor : aero::EdgeActorBase<FlowActor, Sequential> {
    void ingest(const aero::Frame& f) noexcept { process_frame(f); }
};

int main() {
    constexpr std::int64_t kN = 200'000;      // frames to stream through
    constexpr std::uint32_t kCapacity = 256;  // ring size == max credit == max in-flight
    constexpr std::uint32_t kBudget = 64;     // frames drained per consumer turn

    // --- Compile the flow ONCE: Source -> Scale(2) -> Sum. -------------------------------------
    aero::nodes::DecodeSourceNode source;
    aero::nodes::ScaleNode scale{2.0};
    aero::nodes::SumOutputNode sink_node;
    aero::CompiledFlow flow;
    flow.add(source).add(scale).add(sink_node);

    FlowActor actor;
    actor.bind_flow(flow);

    // --- Open the stream: the ring + the 024 single-producer token (D1). -----------------------
    StreamActivation<aero::Frame>::Config cfg;
    cfg.capacity = kCapacity;
    std::pmr::monotonic_buffer_resource mr;    // cold: the ring is pre-allocated once, here
    StreamActivation<aero::Frame> act(cfg, &mr);
    auto tok = open_stream(act);               // single-writer token; a 2nd bind would be a 007 error
    if (!tok) {
        std::printf("open_stream failed\nFAIL\n");
        return 1;
    }
    aero::StreamSink<aero::Frame> sink(std::move(tok.value()));

    // --- Producer thread: the driver's run loop pushes N frames, honoring backpressure. --------
    std::atomic<bool> stop_flag{false};
    aero::drivers::GeneratorDriver driver;
    aero::DriverConfig dcfg;
    dcfg.endpoint = "generator://seq";
    dcfg.frame_count = static_cast<std::uint32_t>(kN);
    if (driver.open(dcfg) != aero::DriverStatus::Ok) {
        std::printf("driver.open failed\nFAIL\n");
        return 1;
    }
    std::thread producer([&] { driver.run(std::move(sink), aero::StopToken{&stop_flag}); });

    // --- Consumer: drain into the flow until all N frames are processed. ------------------------
    auto& ch = act.channel();
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(kN), 0);
    std::int64_t expected = 0, processed = 0;
    std::int64_t inversions = 0, dup = 0, bad_output = 0;
    std::uint64_t max_occupancy = 0;

    while (processed < kN) {
        const std::uint64_t occ = ch.occupancy();
        if (occ == 0) {
            std::this_thread::yield();  // producer hasn't caught up; no sleep, bounded by kN progress
            continue;
        }
        if (occ > max_occupancy) max_occupancy = occ;

        StreamBatch<aero::Frame> batch(ch, kBudget);
        while (const aero::Frame* f = batch.next()) {
            const std::int64_t id = f->raw;
            if (id != expected) ++inversions;  // FIFO: ids arrive strictly in order
            expected = id + 1;
            if (id >= 0 && id < kN) {
                if (seen[static_cast<std::size_t>(id)]) ++dup;
                else seen[static_cast<std::size_t>(id)] = 1;
            }
            // ctx.frame views the LIVE slot; run the flow BEFORE retire() (006 §4, D5).
            actor.ingest(*f);
            if (actor.last_output() != static_cast<double>(id) * 2.0) ++bad_output;  // integrity
            ++processed;
            batch.retire();  // return credit ONLY after the flow consumed the slot
        }
    }
    producer.join();

    std::int64_t missing = 0;
    for (std::int64_t i = 0; i < kN; ++i)
        if (!seen[static_cast<std::size_t>(i)]) ++missing;

    std::printf("streamed %lld frames through a %u-slot ring into a flow\n",
                (long long)kN, kCapacity);
    std::printf("  driver produced : %llu  (expected %lld)\n",
                (unsigned long long)driver.produced(), (long long)kN);
    std::printf("  FIFO inversions : %lld  (expected 0)\n", (long long)inversions);
    std::printf("  duplicates      : %lld  (expected 0)\n", (long long)dup);
    std::printf("  missing frames  : %lld  (expected 0)\n", (long long)missing);
    std::printf("  bad flow output : %lld  (expected 0 — payload intact through the flow)\n",
                (long long)bad_output);
    std::printf("  frames_processed: %ld  (actor accounting, expected %lld)\n",
                actor.frames_processed(), (long long)kN);
    std::printf("  max occupancy   : %llu  (bounded by capacity %u)\n",
                (unsigned long long)max_occupancy, kCapacity);
    std::printf("  backpressure stalls: %llu\n", (unsigned long long)driver.stalls());

    const bool ok = inversions == 0 && dup == 0 && missing == 0 && bad_output == 0 &&
                    processed == kN && actor.frames_processed() == kN &&
                    driver.produced() == static_cast<std::uint64_t>(kN) &&
                    max_occupancy <= kCapacity;
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
