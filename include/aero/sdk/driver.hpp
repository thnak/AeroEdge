// AeroEdge SDK — the IDriver contract (spec 006).
//
// A Driver bridges a device protocol into frames and feeds them into an actor's inbound stream. It
// runs on an I/O / blocking lane, never inside a flow (D6). Phase-2 wires the streaming producer path:
// a driver produces frames and pushes them into a Quark 024 StreamChannel (credit-controlled SPSC
// ring); the actor's stream drain runs the flow per frame with backpressure — never shedding (§3, D2).
//
// This header owns the SEAM only. AeroEdge writes NO ring, NO scheduler, NO wakeup (R0): `StreamSink`
// is a thin handle over Quark's single-producer `StreamRef` (024). The concrete drivers live in
// `aero/drivers/`; the real socket/serial transports are gated on the Quark PAL (019, D3).
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "aero/sdk/processing_context.hpp"      // aero::Frame — the inline stream-slot frame (006 §4)
#include "quark/core/stream_activation.hpp"       // quark::StreamRef<F> — the 024 single-producer token

namespace aero {

enum class DriverStatus {
    Ok,
    Error,
    Unsupported,  // capability not offered by this driver (e.g. write on a read-only driver)
};

struct DriverDescriptor {
    std::string_view type_id;   // e.g. "aero.driver.tcp"
    bool writable = false;      // supports device writes (actuators / OTA transfer, 006 §7)
};

// Connection / behaviour knobs handed to open() (006 §11 open question: full schema aligns with the
// Quark 013 config + the 010 device registry later). Phase-2 keeps the minimal set the built-in
// drivers need; a driver reads only the fields it understands.
struct DriverConfig {
    std::string_view endpoint;       // device address / poll target / connection string
    std::uint32_t    frame_count = 0;  // bounded producers: frames to emit (0 => run until StopToken)
    std::uint32_t    rate_hz = 0;      // advisory produce rate (0 => as fast as backpressure allows)
};

// A device-directed write (actuator setpoint / OTA transfer chunk, 006 §7). Fenced + authorized at
// the actor before it reaches the driver (D4); the payload shape firms up with the 010 device model.
struct DeviceCommand {
    std::string_view target;
    std::int64_t     value = 0;
};

// StopToken — the cooperative stop signal a driver's run loop polls (006 §8 graceful stop). A thin
// view over an atomic flag the owning actor/runtime raises on drain/stop; the driver finishes the
// in-flight frame and returns. AeroEdge owns no timer/thread here — the flag is set by Quark's
// lifecycle path (007/010); this is only the read side the driver sees.
class StopToken {
public:
    StopToken() noexcept = default;
    explicit StopToken(const std::atomic<bool>* flag) noexcept : flag_(flag) {}

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_ != nullptr && flag_->load(std::memory_order_acquire);
    }

private:
    const std::atomic<bool>* flag_ = nullptr;
};

// StreamSink<F> — a thin handle over the actor's StreamChannel<F> PRODUCER side (the Quark 024
// single-producer token). Exactly ONE driver binds one stream: the token comes from
// `quark::open_stream(activation)` and a second bind is a typed 007 error (024 SPSC precondition, D1).
// `try_push` is LOSSLESS backpressure — false == credit depleted, so the driver must back off and
// retry the SAME frame, never drop it (D2, 006 §3). Move-only (it IS the single-writer token).
template <class F>
class StreamSink {
public:
    StreamSink() noexcept = default;
    explicit StreamSink(quark::StreamRef<F> ref) noexcept : ref_(std::move(ref)) {}

    StreamSink(StreamSink&&) noexcept = default;
    StreamSink& operator=(StreamSink&&) noexcept = default;
    StreamSink(const StreamSink&) = delete;
    StreamSink& operator=(const StreamSink&) = delete;

    [[nodiscard]] bool valid() const noexcept { return ref_.valid(); }

    // Non-blocking, lossless. false == no credit -> the driver stalls/backs off, NEVER sheds (§3/D2).
    [[nodiscard]] bool try_push(const F& frame) noexcept { return ref_.try_push(frame); }

    // Blocking, lossless: stall on credit depletion and wake on the credit-return edge (024 producer
    // un-stall). Only safe when a consumer drives the exec-state drain (poll_unstall); the push-with-
    // backoff form (try_push + yield) is the portable Phase-2 default the built-in drivers use.
    void push_blocking(const F& frame) noexcept { ref_.push_blocking(frame); }

private:
    quark::StreamRef<F> ref_;  // move-only single-producer token (024)
};

// The Driver contract (006 §5). open/run/poll/write/close/descriptor. Concrete drivers live in
// aero/drivers/. run() and poll() feed frames into the bound StreamSink honoring backpressure (§3).
class IDriver {
public:
    virtual ~IDriver() = default;

    // Connect/authenticate to the device (006 §8). Called on actor activation and on re-activation
    // after a fenced migration (010 §3). May block — runs on the driver's I/O lane, never a flow (D6).
    virtual DriverStatus open(const DriverConfig& cfg) noexcept = 0;

    // Producer loop for PUSH drivers (TCP/Serial/MQTT/Camera, §6.2): read -> frame -> try_push into
    // the bound sink, honoring backpressure. Returns when the StopToken is raised or the connection
    // drops. Runs on a Quark BlockingHandler lane (ADR-015), never a scheduler worker (D6).
    virtual DriverStatus run(StreamSink<Frame> sink, StopToken stop) noexcept = 0;

    // One read for PULL drivers (PLC poll, §6.1): triggered by a Command/Timer. Produces zero or more
    // frames into the sink. Non-looping. Default: this driver is push-only.
    virtual DriverStatus poll(StreamSink<Frame> /*sink*/) noexcept { return DriverStatus::Unsupported; }

    // Write toward the device (actuators, OTA transfer §7). Fenced + authorized at the actor (D4).
    virtual DriverStatus write(const DeviceCommand& /*cmd*/) noexcept { return DriverStatus::Unsupported; }

    virtual void close() noexcept = 0;
    virtual const DriverDescriptor& descriptor() const noexcept = 0;   // capabilities (§7)
};

}  // namespace aero
