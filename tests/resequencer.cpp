// AeroEdge Phase-7 LOAD-BEARING gate (014 §3 C1 / §5, X2): the FIFO / dedup resequencing shim.
//
// Push a stream of N seq-stamped frames through a ChaosChannel that deliberately REORDERS + DUPLICATES
// (the worst an MQTT broker does across reconnect + QoS-1 redelivery), then:
//
//   CONTROL (proves the shim is NON-VACUOUS): the RAW chaos output is inspected directly — it MUST be
//   out-of-order AND contain duplicates. If it were already clean, the shim would be proving nothing.
//   So we assert the C1 property FAILS on the raw stream.
//
//   SHIM: the SAME raw stream is fed through the Resequencer — assert the output is STRICT FIFO, every
//   frame delivered EXACTLY ONCE, 0 loss, 0 duplication, 0 inversion, 0 overflow. The property that
//   fails without the shim holds with it. Deterministic (seeded LCG), exit-code-gated. 0 = pass.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "aero/transport/resequencer.hpp"
#include "chaos_transport.hpp"

using aero::testing::ChaosChannel;
using aero::transport::OfferOutcome;
using aero::transport::Resequencer;
using aero::transport::SequenceStamper;

int main() {
    constexpr std::uint64_t N = 2000;
    constexpr std::size_t kWindow = 64;  // > ChaosChannel max_jitter (12) ⇒ no overflow (bounded mem)

    // --- Sender: stamp a strictly-monotonic per-link seq on each frame; payload == logical index. ----
    // Drive the chaos channel, capturing its RAW (scrambled + duplicated) output into `raw`.
    SequenceStamper stamper;
    ChaosChannel<std::uint64_t> chaos(/*seed*/ 0xA11CEull, {/*max_jitter*/ 12, /*dup_percent*/ 25});
    std::vector<std::pair<std::uint64_t, std::uint64_t>> raw;  // (seq, value) as the substrate emits it
    chaos.on_deliver([&](std::uint64_t seq, const std::uint64_t& value) { raw.emplace_back(seq, value); });

    for (std::uint64_t i = 0; i < N; ++i) {
        const auto framed = stamper.stamp<std::uint64_t>(i);  // value i carries logical order
        chaos.push(framed.seq, framed.value);
    }
    chaos.flush();  // end of stream — release everything still scheduled

    // --- CONTROL: the raw substrate output must be reordered AND duplicated (else non-vacuous). -------
    std::uint64_t raw_dups = raw.size() - N;  // every extra arrival beyond N unique is a duplicate
    std::uint64_t raw_inversions = 0;         // adjacent descents in delivered seq == reordering
    for (std::size_t i = 1; i < raw.size(); ++i)
        if (raw[i].first < raw[i - 1].first) ++raw_inversions;

    bool raw_is_fifo_exactly_once = raw.size() == N;  // would need exact count AND order
    if (raw_is_fifo_exactly_once)
        for (std::uint64_t i = 0; i < N; ++i)
            if (raw[i].second != i) raw_is_fifo_exactly_once = false;

    const bool control_ok = raw_dups > 0 && raw_inversions > 0 && !raw_is_fifo_exactly_once;
    std::printf("CONTROL (no shim): raw arrivals=%zu (N=%llu) duplicates=%llu inversions=%llu -> "
                "strict-FIFO-exactly-once=%s\n",
                raw.size(), (unsigned long long)N, (unsigned long long)raw_dups,
                (unsigned long long)raw_inversions, raw_is_fifo_exactly_once ? "YES" : "NO (as expected)");

    // --- SHIM: feed the identical raw stream through the Resequencer; collect the released order. -----
    Resequencer<std::uint64_t> reseq(kWindow);
    std::vector<std::uint64_t> out;
    out.reserve(N);
    for (const auto& [seq, value] : raw) {
        reseq.offer(seq, value, [&](std::uint64_t v) { out.push_back(v); });
    }

    const auto& st = reseq.stats();
    const bool no_loss = out.size() == N && st.released == N;
    bool strict_fifo = out.size() == N;  // exactly-once + in-order ⇔ out == 0,1,2,...,N-1
    for (std::uint64_t i = 0; i < out.size(); ++i)
        if (out[i] != i) strict_fifo = false;
    const bool dups_dropped = st.duplicates == raw_dups;  // every raw duplicate was dropped by the shim
    const bool no_overflow = st.overflow == 0;
    const bool bounded_mem = st.max_buffered <= kWindow;

    std::printf("SHIM  (resequencer): released=%llu (expected %llu) dropped_dups=%llu overflow=%llu "
                "max_buffered=%llu (window=%zu)\n",
                (unsigned long long)st.released, (unsigned long long)N,
                (unsigned long long)st.duplicates, (unsigned long long)st.overflow,
                (unsigned long long)st.max_buffered, kWindow);
    std::printf("SHIM  properties: strict-FIFO=%s loss=%s duplication=%s inversion=%s\n",
                strict_fifo ? "yes" : "NO", no_loss ? "0" : "NONZERO",
                st.duplicates == raw_dups ? "0-out" : "LEAK", strict_fifo ? "0" : "PRESENT");

    const bool pass =
        control_ok && strict_fifo && no_loss && dups_dropped && no_overflow && bounded_mem;
    std::printf("%s\n", pass ? "OK" : "FAIL");
    return pass ? 0 : 1;
}
