// AeroEdge built-in drivers (spec 006 §9). Phase-2 ships the honest subset: a synthetic frame source
// that exercises the FULL Quark 024 StreamChannel path (credit backpressure, FIFO, lossless stall)
// without a socket, plus a documented STUB for the real TCP driver gated on the Quark PAL (019, D3).
//
// Why no real socket driver yet (AGENTS R4/R5): D3 mandates that ALL socket/serial/timer I/O go
// through the Quark PAL (019). The PAL is spec'd but NOT implemented (see Quark 019), so a TCP/Serial
// driver would have to fake POSIX socket I/O — which R4 ("use the real Quark API, don't invent it")
// and R5 ("gate the rest, don't overstate") forbid. GeneratorDriver is the honest stand-in: its run()
// loop is byte-for-byte the shape a TcpDriver's read loop will take (read -> frame -> try_push ->
// back off on no-credit), with the blocking "read" replaced by a deterministic counter.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "aero/sdk/driver.hpp"

namespace aero::drivers {

// GeneratorDriver — a deterministic high-rate PUSH frame source (006 §6.2). Emits a monotone sequence
// of frames (frame.raw = 0,1,2,…) into the bound StreamSink. On credit depletion (try_push == false)
// it BACKS OFF and retries the SAME frame — lossless backpressure, never a silent drop (D2, 006 §3).
// This is the Phase-2 driver: it proves the ingestion path end-to-end before the PAL (019) exists.
class GeneratorDriver final : public IDriver {
public:
    DriverStatus open(const DriverConfig& cfg) noexcept override {
        cfg_ = cfg;
        opened_ = true;
        return DriverStatus::Ok;
    }

    // PUSH producer loop (006 §6.2). Emits cfg.frame_count frames (0 => until the StopToken is raised),
    // honoring backpressure. On no-credit it yields and RETRIES the same frame — a real TCP driver
    // would instead stop reading and let the OS socket buffer + TCP flow control back-pressure the
    // sender (§3); the lossless-stall contract is identical. The StopToken is polled every frame AND
    // inside the stall so a drain/stop finishes the in-flight frame and returns promptly (006 §8).
    DriverStatus run(StreamSink<Frame> sink, StopToken stop) noexcept override {
        if (!opened_ || !sink.valid()) return DriverStatus::Error;
        const bool bounded = cfg_.frame_count != 0;
        const std::int64_t limit = static_cast<std::int64_t>(cfg_.frame_count);

        for (std::int64_t seq = 0; !stop.stop_requested(); ++seq) {
            if (bounded && seq >= limit) break;
            const Frame f{seq};                       // synthetic "read -> frame": seq is the payload
            while (!sink.try_push(f)) {                // lossless backpressure (§3): stall, never drop
                stalls_.fetch_add(1, std::memory_order_relaxed);
                if (stop.stop_requested()) return DriverStatus::Ok;  // graceful stop mid-stall (§8)
                std::this_thread::yield();             // back off (no sleep); credit returns on drain
            }
            produced_.fetch_add(1, std::memory_order_relaxed);
        }
        return DriverStatus::Ok;
    }

    void close() noexcept override { opened_ = false; }

    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    // Observability (009): frames produced and how many stall episodes backpressure caused.
    [[nodiscard]] std::uint64_t produced() const noexcept { return produced_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_.load(std::memory_order_relaxed); }

    static constexpr DriverDescriptor kDesc{"aero.driver.generator", /*writable*/ false};

private:
    DriverConfig cfg_{};
    bool opened_ = false;
    std::atomic<std::uint64_t> produced_{0};
    std::atomic<std::uint64_t> stalls_{0};
};

// TcpDriver — STUB, gated on the Quark PAL (019 sockets) + the 016 wire framing. Per D3 a driver may
// do NO raw POSIX socket I/O; per AGENTS R4/R5 we do NOT fake it. open/run therefore fail explicitly
// and document the gate. It exists to pin the descriptor/capability shape and make the 019 dependency
// visible to the Flow Compiler (a flow that binds a driver can see it is not yet available).
class TcpDriver final : public IDriver {
public:
    // GATED (019): a real open() would connect a PAL socket to cfg.endpoint. Until the PAL lands this
    // returns Error rather than faking a connection (R4/R5).
    DriverStatus open(const DriverConfig& /*cfg*/) noexcept override { return DriverStatus::Error; }

    // GATED (019/016): the read loop needs PAL sockets + wire framing. Not yet available.
    DriverStatus run(StreamSink<Frame> /*sink*/, StopToken /*stop*/) noexcept override {
        return DriverStatus::Unsupported;
    }

    void close() noexcept override {}
    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    static constexpr DriverDescriptor kDesc{"aero.driver.tcp", /*writable*/ false};
};

}  // namespace aero::drivers
