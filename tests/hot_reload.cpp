// AeroEdge Phase-5 LOAD-BEARING gate — hot-reload with 0 dropped / 0 duplicated Commands (009 §4 P2).
//
// The load-bearing capability: swap a LIVE actor's flow while Commands are flowing, without dropping
// or duplicating any Command. Mechanism (009 §4): the new CompiledFlow is compiled + validated off to
// the side (old one still running), then published via a mailbox-ordered ReloadFlow Command. On a
// Sequential actor the mailbox is FIFO on a single executor (Quark 001/ADR-002), so the swap lands at
// a quiescent point BETWEEN messages — in-flight frames finish on the OLD flow, later frames run the
// NEW flow. No explicit quiesce(Drain) is needed: on Sequential it is a no-op resolving synchronously
// between messages (Quark 015), which the mailbox already provides.
//
// The interleave is deterministic and controlled by the test: deploy flow A (Scale x2), tell N frames
// (raw=1 → output 2 each), reload to flow B (Scale x10), tell M frames (raw=1 → output 10 each). The
// N frames are still draining on the engine worker when the reload is issued (genuine in-flight), and
// mailbox FIFO guarantees they complete on A. Proof:
//   (i)   frames_processed == N + M            → 0 dropped, 0 duplicated (exactly-once)
//   (ii)  after reload: frames == N, sum == 2N → every in-flight Command finished on A before the swap
//   (iii) last_output == 10, sum == 2N + 10M   → subsequent Commands ran on B; the split is exact
// (output_sum == 2*countA + 10*countB with countA+countB == N+M has the UNIQUE solution countA==N,
// countB==M — any drop/dup/mis-order across the swap would change it). Must be TSan-clean. Exit 0 = OK.
#include <cstdio>
#include <string>

#include "aero/runtime/runtime.hpp"

namespace {
constexpr long N = 5000;  // frames on flow A (Scale x2), sent BEFORE the reload
constexpr long M = 5000;  // frames on flow B (Scale x10), sent AFTER the reload
}  // namespace

int main() {
    // Flow A: decode -> scale x2 -> sum. NO driver — the test drives frames itself for a deterministic
    // send/reload interleave. Each frame raw=1 → decode "raw"=1 → x2 → sum = 2.
    const char* flow_a = R"({
      "name":"reloadable","version":"1.0.0","actor":{"kind":"edge","key":5},"flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale","config":{"factor":2}},
        {"type_id":"aero.output.sum"}]})";

    // Flow B: same shape, Scale x10 → sum = 10. A Live change (node config), so it hot-reloads.
    const char* flow_b = R"({
      "name":"reloadable","version":"2.0.0","actor":{"kind":"edge","key":5},"flow":[
        {"type_id":"aero.source.decode"},
        {"type_id":"aero.transform.scale","config":{"factor":10}},
        {"type_id":"aero.output.sum"}]})";

    aero::runtime::Runtime rt;
    if (auto r = rt.deploy_json(flow_a); !r) {
        std::printf("deploy A failed: %s\nFAIL\n", r.error().c_str());
        return 1;
    }

    // Send N frames onto flow A. These are async tells: the engine worker is still draining them when
    // the reload below is issued — the genuine "reload while Commands are flowing" case.
    for (long i = 0; i < N; ++i) {
        if (auto r = rt.tell_frame(1); !r) {
            std::printf("tell_frame A failed: %s\nFAIL\n", r.error().c_str());
            return 1;
        }
    }

    // Hot-reload to flow B. reload() tells ReloadFlow (FIFO after the N frames) then blocks on a status
    // ask (FIFO after the swap) — so when it returns, all N frames have completed on A and the swap is
    // applied. The old plan is retired only then (provably unreferenced).
    if (auto r = rt.reload_json(flow_b); !r) {
        std::printf("reload A->B failed: %s\nFAIL\n", r.error().c_str());
        return 1;
    }

    // (ii) Immediately after the reload: exactly N frames processed, all on A (sum == 2N), 1 swap.
    auto mid = rt.status();
    const long mid_frames = mid.value("frames_processed", 0L);
    const double mid_sum = mid.value("output_sum", 0.0);
    const long mid_reloads = mid.value("reloads", 0L);

    // Send M frames onto flow B. Each raw=1 → x10 → sum = 10.
    for (long i = 0; i < M; ++i) {
        if (auto r = rt.tell_frame(1); !r) {
            std::printf("tell_frame B failed: %s\nFAIL\n", r.error().c_str());
            return 1;
        }
    }

    // A status ask is FIFO after all M frame tells on the Sequential actor → observes the full count.
    auto st = rt.status();
    const long frames = st.value("frames_processed", 0L);
    const double sum = st.value("output_sum", 0.0);
    const double last = st.value("last_output", 0.0);
    const long reloads = st.value("reloads", 0L);

    const long total_sent = N + M;
    const bool exactly_once = frames == total_sent;                 // (i) 0 dropped, 0 duplicated
    const bool mid_all_on_a = mid_frames == N && mid_sum == 2.0 * N && mid_reloads == 1;  // (ii)
    const bool split_exact = sum == (2.0 * N + 10.0 * M);           // (iii) N on A, M on B, exact
    const bool reflects_b = last == 10.0 && reloads == 1;           // subsequent Commands run B

    std::printf("sent            : %ld  (A=%ld, B=%ld)\n", total_sent, N, M);
    std::printf("frames processed: %ld  (expected %ld)  -> dropped/dup = %ld\n", frames, total_sent,
                total_sent - frames);
    std::printf("after reload    : frames=%ld sum=%.0f reloads=%ld (expected %ld / %.0f / 1)\n",
                mid_frames, mid_sum, mid_reloads, N, 2.0 * N);
    std::printf("output_sum      : %.0f  (expected %.0f = 2*%ld + 10*%ld)\n", sum, 2.0 * N + 10.0 * M,
                N, M);
    std::printf("last_output     : %.0f  (expected 10, flow B)\n", last);

    const bool pass = exactly_once && mid_all_on_a && split_exact && reflects_b;
    std::printf("%s\n", pass ? "OK" : "FAIL");
    return pass ? 0 : 1;
}
