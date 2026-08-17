// AeroEdge M9a driver — ModbusTcpDriver: a real Modbus-TCP PULL driver (018 §Multi-protocol southbound,
// 006 §6.1). PLC/register devices are pull, triggered by `poll(sink)` on an external cadence — NOT a
// looping `run()` (006 §6.1 vs §6.2) — so `run()` here is a hard Unsupported and every read happens
// inside one `poll()` call: build a Modbus-TCP ADU (MBAP header + FC03 Read Holding Registers), send it,
// read + validate the response, and hand the RAW register bytes to the sink. Decoding those bytes into
// named tags is deliberately NOT this driver's job — `aero::nodes::ModbusDecodeNode` (nodes/
// compute_nodes.hpp) already owns that, downstream, over `ProcessingContext::payload`; this driver's
// job stops at delivering bytes (mirrors 006's own note: "a Modbus register-map DECODER over bytes that
// already arrived... ships [as a node]; the socket transport stays gated" — this IS that transport,
// finally landed, and it hands off to the exact same decoder rather than duplicating its logic).
//
// RECONNECT (006 §8): the first driver in this tree to actually implement "reconnect on ConnectionLost
// with bounded backoff" — on any send/recv failure the socket is closed and reconnect is retried lazily
// at the TOP of each subsequent poll() call, gated by a per-instance backoff clock that starts at 200ms
// and doubles up to a 5s cap (reset to 200ms the moment a connect succeeds). This is intentionally
// non-blocking beyond one bounded dial attempt — poll() never sleeps out the full backoff window itself
// (a caller driving poll() from a shared scheduler thread must not be stalled for seconds); instead a
// call that lands before the backoff has elapsed just returns Error immediately without touching the
// socket, and the NEXT call (whenever the caller re-drives it) is the one that actually retries.
#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include "aero/pal/net_dial.hpp"
#include "aero/sdk/driver.hpp"
#include "pal/net.hpp"  // quark::pal::* — fd_t, recv_some/send_some/close_fd/would_block

namespace aero::drivers {

// ModbusTcpDriver — FC03 (Read Holding Registers) and FC04 (Read Input Registers), selected at
// construction (M9.1 PR B, 018 §8; both are wire-identical register reads, so they share one decode
// path — ModbusDecodeNode doesn't care which address space the bytes came from). Construction takes
// ownership of its config (host/port/unit_id/start_address/register_count/read_fn) at build time — the
// factory in runtime.hpp::register_builtins() reads these straight out of the deploy-time JSON, per the
// existing GeneratorDriver-style factory closure pattern; DriverConfig (aero/sdk/driver.hpp)
// intentionally stays untouched (its endpoint/frame_count fields don't fit this driver's shape) beyond
// the optional `rate_hz` poll-interval hint, honored here only as an observable stored value — the
// actual poll cadence is the deployer's call (whatever drives `poll()` externally), not this driver's.
class ModbusTcpDriver final : public IDriver {
public:
    // Which register address space poll() reads. Coils (FC01) and discrete inputs (FC02) are NOT here —
    // they're bit-packed, not word-packed, and would need a decode node of their own (018 §8, backlog).
    enum class ReadFunction : std::uint8_t {
        HoldingRegisters = 0x03,
        InputRegisters = 0x04,
    };

    ModbusTcpDriver(std::string host, std::uint16_t port, std::uint8_t unit_id,
                     std::uint16_t start_address, std::uint16_t register_count,
                     ReadFunction read_fn = ReadFunction::HoldingRegisters) noexcept
        : host_(std::move(host)),
          port_(port),
          unit_id_(unit_id),
          start_address_(start_address),
          register_count_(register_count),
          read_fn_(read_fn) {}

    // Config-time rejection (never a silently truncated frame, per Part 1's Frame::payload_len/payload
    // budget): register_count*2 must fit inside kMaxFramePayload. Otherwise dial now — open() is the
    // ONE point a v1 deployment can observe "device unreachable" synchronously, before anything polls.
    DriverStatus open(const DriverConfig& cfg) noexcept override {
        if (static_cast<std::size_t>(register_count_) * 2 > aero::kMaxFramePayload) {
            return DriverStatus::Error;
        }
        rate_hz_ = cfg.rate_hz;  // advisory poll-interval hint only (see class banner) — not enforced here
        opened_ = true;

        auto r = aero::pal::dial_tcp(host_, port_, kConnectTimeoutMs);
        if (!r) return DriverStatus::Error;
        fd_ = *r;
        backoff_ms_ = kInitialBackoffMs;
        next_attempt_at_ = std::chrono::steady_clock::now();
        return DriverStatus::Ok;
    }

    // PULL driver only (006 §6.1) — this class never loops; poll() is the whole surface.
    DriverStatus run(StreamSink<Frame> /*sink*/, StopToken /*stop*/) noexcept override {
        return DriverStatus::Unsupported;
    }

    // One Modbus-TCP request/response, non-looping. On a connection-lost failure, closes the socket so
    // the NEXT poll() call reconnects (bounded backoff, see class banner) rather than wedging or
    // spinning against a dead fd. On success, pushes exactly one Frame, retrying try_push on backpressure
    // (never dropping a frame, 006 §3) — never blocking forever (the sink's own credit is what gates it).
    DriverStatus poll(StreamSink<Frame> sink) noexcept override {
        if (!opened_) return DriverStatus::Error;
        if (fd_ == quark::pal::invalid_fd && !ensure_connected()) return DriverStatus::Error;

        Frame frame{};
        bool io_ok = true;
        const DriverStatus st = do_transaction(frame, io_ok);
        if (!io_ok) close_socket();  // connection lost (006 §8) -> reconnect w/ backoff on a later poll()
        if (st != DriverStatus::Ok) return st;

        while (!sink.try_push(frame)) {
            std::this_thread::yield();  // lossless backpressure (006 §3): stall, never drop
        }
        return DriverStatus::Ok;
    }

    // FC06 (Write Single Register) or FC16 (Write Multiple Registers), M9.1's write slice (018 §8).
    // `DeviceCommand` has no dedicated address/multi-value field (006 §7, a shared SDK type also used
    // by OTA — not widened for this), so both forms live entirely in `cmd.target`:
    //   - a bare decimal address ("5") -> FC06, value from `cmd.value` (0..65535) — unchanged from PR A.
    //   - "addr,v1,v2,..." (comma-separated, up to kMaxWriteMultipleRegisters values) -> FC16, `cmd.value`
    //     unused in this form.
    // Same connect/reconnect/backoff path as poll() either way (a write and a read share one socket; a
    // failed write drops the connection exactly like a failed read does).
    DriverStatus write(const DeviceCommand& cmd) noexcept override {
        if (!opened_) return DriverStatus::Error;

        const auto comma = cmd.target.find(',');
        if (comma == std::string_view::npos) {
            if (cmd.value < 0 || cmd.value > 0xFFFF) return DriverStatus::Error;
            std::uint16_t address = 0;
            if (!parse_u16(cmd.target, address)) return DriverStatus::Error;

            if (fd_ == quark::pal::invalid_fd && !ensure_connected()) return DriverStatus::Error;
            bool io_ok = true;
            const DriverStatus st = do_write_transaction(address, static_cast<std::uint16_t>(cmd.value), io_ok);
            if (!io_ok) close_socket();  // connection lost (006 §8) -> reconnect w/ backoff on a later call
            return st;
        }

        std::uint16_t address = 0;
        if (!parse_u16(cmd.target.substr(0, comma), address)) return DriverStatus::Error;

        std::array<std::uint16_t, kMaxWriteMultipleRegisters> values{};
        std::size_t count = 0;
        for (std::string_view rest = cmd.target.substr(comma + 1); !rest.empty();) {
            if (count >= kMaxWriteMultipleRegisters) return DriverStatus::Error;
            const auto next = rest.find(',');
            const std::string_view field = next == std::string_view::npos ? rest : rest.substr(0, next);
            std::uint16_t v = 0;
            if (!parse_u16(field, v)) return DriverStatus::Error;
            values[count++] = v;
            rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 1);
        }
        if (count == 0) return DriverStatus::Error;

        if (fd_ == quark::pal::invalid_fd && !ensure_connected()) return DriverStatus::Error;
        bool io_ok = true;
        const DriverStatus st = do_write_multiple_transaction(address, values.data(), count, io_ok);
        if (!io_ok) close_socket();  // connection lost (006 §8) -> reconnect w/ backoff on a later call
        return st;
    }

    void close() noexcept override { close_socket(); }

    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    // Observability: the exception code from the most recent Modbus exception response (0x83/0x86), if
    // any — "log the exception code" without a required stderr dependency in a header (mirrors
    // GeneratorDriver's produced()/stalls() counters). 0 means "none observed yet" (0 is not a valid
    // Modbus exception code).
    [[nodiscard]] std::uint8_t last_exception_code() const noexcept { return last_exception_code_; }

    static constexpr DriverDescriptor kDesc{"aero.driver.modbus_tcp", /*writable*/ true};

private:
    static constexpr int kConnectTimeoutMs = 2000;
    static constexpr int kIoTimeoutMs = 3000;      // bounded total time for one send/recv-exact call
    static constexpr int kInitialBackoffMs = 200;
    static constexpr int kMaxBackoffMs = 5000;
    static constexpr std::uint8_t kFcWriteSingleRegister = 0x06;
    static constexpr std::uint8_t kFcWriteMultipleRegisters = 0x10;
    static constexpr std::uint8_t kFcExceptionBit = 0x80;
    // Modbus's own FC16 implementation limit (not this codebase's — every stack caps it here), so a
    // register-map wider than this needs multiple write() calls, same posture as the 128B Frame payload
    // cap on reads (§3 of spec 018) forcing multiple poll() configs for a wide register map.
    static constexpr std::size_t kMaxWriteMultipleRegisters = 123;

    static bool parse_u16(std::string_view s, std::uint16_t& out) noexcept {
        if (s.empty()) return false;
        const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
        return res.ec == std::errc{} && res.ptr == s.data() + s.size();
    }

    // Lazily reconnect, gated by the backoff clock — see class banner. false == still backing off, or
    // the dial itself failed (in which case the backoff for the NEXT attempt is scheduled here).
    bool ensure_connected() noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_attempt_at_) return false;

        auto r = aero::pal::dial_tcp(host_, port_, kConnectTimeoutMs);
        if (!r) {
            next_attempt_at_ = now + std::chrono::milliseconds(backoff_ms_);
            backoff_ms_ = backoff_ms_ * 2 < kMaxBackoffMs ? backoff_ms_ * 2 : kMaxBackoffMs;
            return false;
        }
        fd_ = *r;
        backoff_ms_ = kInitialBackoffMs;  // reset on success
        next_attempt_at_ = now;
        return true;
    }

    void close_socket() noexcept {
        if (fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(fd_);
            fd_ = quark::pal::invalid_fd;
        }
    }

    static void put_u16_be(std::byte* p, std::uint16_t v) noexcept {
        p[0] = static_cast<std::byte>((v >> 8) & 0xFF);
        p[1] = static_cast<std::byte>(v & 0xFF);
    }
    static std::uint16_t get_u16_be(const std::byte* p) noexcept {
        return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(p[0]) << 8) |
                                           std::to_integer<std::uint16_t>(p[1]));
    }

    // Bounded send-all / recv-exact over the raw fd (same poll-with-timeout idiom as
    // aero/transport/mqtt_codec.hpp's read_n — try the syscall, poll only on would_block, bounded by an
    // overall deadline so a stalled peer can't wedge poll() forever). false == I/O failure (peer closed,
    // socket error, or timed out) — the caller treats that as connection-lost.
    static bool send_all(quark::pal::fd_t fd, const std::byte* buf, std::size_t n) noexcept {
        std::size_t sent = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
        while (sent < n) {
            auto w = quark::pal::send_some(fd, buf + sent, n - sent);
            if (w) {
                sent += *w;
                continue;
            }
            if (w.error() != quark::pal::would_block()) return false;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            if (!aero::pal::wait_writable(fd, 200)) return false;
        }
        return true;
    }
    static bool recv_exact(quark::pal::fd_t fd, std::byte* buf, std::size_t n) noexcept {
        std::size_t got = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
        while (got < n) {
            auto r = quark::pal::recv_some(fd, buf + got, n - got);
            if (r) {
                if (*r == 0) return false;  // peer closed
                got += *r;
                continue;
            }
            if (r.error() != quark::pal::would_block()) return false;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            if (!aero::pal::wait_readable(fd, 200)) return false;
        }
        return true;
    }

    // One request/response transaction. `io_ok` is set false ONLY for a transport-layer/framing problem
    // (send/recv failure, transaction-id mismatch, an unparseable PDU) — the caller closes+reconnects on
    // that. A clean Modbus EXCEPTION response (0x83) is a well-formed answer over a healthy connection:
    // `io_ok` stays true, this just returns Error with last_exception_code_ recorded (never a crash).
    DriverStatus do_transaction(Frame& out, bool& io_ok) noexcept {
        const std::uint16_t txn_id = next_txn_id_++;

        std::array<std::byte, 12> req{};
        put_u16_be(&req[0], txn_id);
        put_u16_be(&req[2], 0x0000);  // protocol id: always 0 (Modbus)
        put_u16_be(&req[4], 0x0006);  // length: unit(1) + fc(1) + addr(2) + count(2)
        req[6] = static_cast<std::byte>(unit_id_);
        req[7] = static_cast<std::byte>(read_fn_);
        put_u16_be(&req[8], start_address_);
        put_u16_be(&req[10], register_count_);

        if (!send_all(fd_, req.data(), req.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }

        std::array<std::byte, 7> hdr{};  // txn(2) + proto(2) + length(2) + unit(1)
        if (!recv_exact(fd_, hdr.data(), hdr.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }
        const std::uint16_t resp_txn = get_u16_be(&hdr[0]);
        const std::uint16_t resp_len = get_u16_be(&hdr[4]);
        if (resp_txn != txn_id || resp_len == 0) {
            io_ok = false;  // desynced/malformed — don't trust the byte stream further
            return DriverStatus::Error;
        }

        const std::size_t pdu_len = static_cast<std::size_t>(resp_len) - 1;  // minus the unit-id byte
        constexpr std::size_t kMaxPdu = 2 + aero::kMaxFramePayload;  // fc + byte_count + data, generous
        if (pdu_len < 1 || pdu_len > kMaxPdu) {
            io_ok = false;
            return DriverStatus::Error;
        }
        std::array<std::byte, kMaxPdu> pdu{};
        if (!recv_exact(fd_, pdu.data(), pdu_len)) {
            io_ok = false;
            return DriverStatus::Error;
        }

        const auto fc = std::to_integer<std::uint8_t>(pdu[0]);
        if ((fc & kFcExceptionBit) != 0) {
            // Well-formed Modbus exception (e.g. 0x83/0x84): connection is fine, just a device-level
            // error — never close the socket for this (see function banner).
            last_exception_code_ = pdu_len >= 2 ? std::to_integer<std::uint8_t>(pdu[1]) : 0;
            return DriverStatus::Error;
        }
        if (fc != static_cast<std::uint8_t>(read_fn_)) {
            io_ok = false;  // an unexpected function code is a framing problem, not a device error
            return DriverStatus::Error;
        }

        const std::size_t expected_bytes = static_cast<std::size_t>(register_count_) * 2;
        if (pdu_len < 2) {
            io_ok = false;
            return DriverStatus::Error;
        }
        const auto byte_count = std::to_integer<std::uint8_t>(pdu[1]);
        if (byte_count != expected_bytes || pdu_len != 2 + expected_bytes) {
            io_ok = false;  // byte-count field doesn't match what we asked for/received — desynced
            return DriverStatus::Error;
        }

        out.payload_len = static_cast<std::uint16_t>(expected_bytes);
        for (std::size_t i = 0; i < expected_bytes; ++i) out.payload[i] = pdu[2 + i];
        return DriverStatus::Ok;
    }

    // One FC06 request/response transaction. Same io_ok contract as do_transaction(): false only for a
    // transport/framing problem (caller reconnects); a clean exception response (0x86) is a healthy
    // connection reporting a device-level error (io_ok stays true).
    DriverStatus do_write_transaction(std::uint16_t address, std::uint16_t value, bool& io_ok) noexcept {
        const std::uint16_t txn_id = next_txn_id_++;

        std::array<std::byte, 12> req{};
        put_u16_be(&req[0], txn_id);
        put_u16_be(&req[2], 0x0000);
        put_u16_be(&req[4], 0x0006);  // length: unit(1) + fc(1) + addr(2) + value(2)
        req[6] = static_cast<std::byte>(unit_id_);
        req[7] = static_cast<std::byte>(kFcWriteSingleRegister);
        put_u16_be(&req[8], address);
        put_u16_be(&req[10], value);

        if (!send_all(fd_, req.data(), req.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }

        std::array<std::byte, 7> hdr{};
        if (!recv_exact(fd_, hdr.data(), hdr.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }
        const std::uint16_t resp_txn = get_u16_be(&hdr[0]);
        const std::uint16_t resp_len = get_u16_be(&hdr[4]);
        if (resp_txn != txn_id || resp_len == 0) {
            io_ok = false;  // desynced/malformed — don't trust the byte stream further
            return DriverStatus::Error;
        }

        const std::size_t pdu_len = static_cast<std::size_t>(resp_len) - 1;
        constexpr std::size_t kMaxWritePdu = 5;  // fc(1) + addr(2) + value(2), the largest well-formed reply
        if (pdu_len < 1 || pdu_len > kMaxWritePdu) {
            io_ok = false;
            return DriverStatus::Error;
        }
        std::array<std::byte, kMaxWritePdu> pdu{};
        if (!recv_exact(fd_, pdu.data(), pdu_len)) {
            io_ok = false;
            return DriverStatus::Error;
        }

        const auto fc = std::to_integer<std::uint8_t>(pdu[0]);
        if ((fc & kFcExceptionBit) != 0) {
            last_exception_code_ = pdu_len >= 2 ? std::to_integer<std::uint8_t>(pdu[1]) : 0;
            return DriverStatus::Error;
        }
        if (fc != kFcWriteSingleRegister || pdu_len != 5) {
            io_ok = false;  // an unexpected function code / short reply is a framing problem
            return DriverStatus::Error;
        }
        // A conformant server echoes address+value back on success; a mismatch means desync, not a
        // device-level error — treat it as a framing problem like the rest of this function.
        if (get_u16_be(&pdu[1]) != address || get_u16_be(&pdu[3]) != value) {
            io_ok = false;
            return DriverStatus::Error;
        }
        return DriverStatus::Ok;
    }

    // One FC16 request/response transaction. Same io_ok/exception contract as do_write_transaction().
    // `count` is already bounded to kMaxWriteMultipleRegisters by the caller (write()).
    DriverStatus do_write_multiple_transaction(std::uint16_t address, const std::uint16_t* values,
                                                std::size_t count, bool& io_ok) noexcept {
        const std::uint16_t txn_id = next_txn_id_++;
        const auto byte_count = static_cast<std::uint8_t>(count * 2);

        std::array<std::byte, 13 + 2 * kMaxWriteMultipleRegisters> req{};
        put_u16_be(&req[0], txn_id);
        put_u16_be(&req[2], 0x0000);
        put_u16_be(&req[4], static_cast<std::uint16_t>(6 + byte_count));  // unit+fc+addr+qty+bytecount+data
        req[6] = static_cast<std::byte>(unit_id_);
        req[7] = static_cast<std::byte>(kFcWriteMultipleRegisters);
        put_u16_be(&req[8], address);
        put_u16_be(&req[10], static_cast<std::uint16_t>(count));
        req[12] = static_cast<std::byte>(byte_count);
        for (std::size_t i = 0; i < count; ++i) put_u16_be(&req[13 + 2 * i], values[i]);
        const std::size_t req_len = 13 + byte_count;

        if (!send_all(fd_, req.data(), req_len)) {
            io_ok = false;
            return DriverStatus::Error;
        }

        std::array<std::byte, 7> hdr{};
        if (!recv_exact(fd_, hdr.data(), hdr.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }
        const std::uint16_t resp_txn = get_u16_be(&hdr[0]);
        const std::uint16_t resp_len = get_u16_be(&hdr[4]);
        if (resp_txn != txn_id || resp_len == 0) {
            io_ok = false;  // desynced/malformed — don't trust the byte stream further
            return DriverStatus::Error;
        }

        const std::size_t pdu_len = static_cast<std::size_t>(resp_len) - 1;
        constexpr std::size_t kMaxWritePdu = 5;  // fc(1) + addr(2) + qty(2), the largest well-formed reply
        if (pdu_len < 1 || pdu_len > kMaxWritePdu) {
            io_ok = false;
            return DriverStatus::Error;
        }
        std::array<std::byte, kMaxWritePdu> pdu{};
        if (!recv_exact(fd_, pdu.data(), pdu_len)) {
            io_ok = false;
            return DriverStatus::Error;
        }

        const auto fc = std::to_integer<std::uint8_t>(pdu[0]);
        if ((fc & kFcExceptionBit) != 0) {
            last_exception_code_ = pdu_len >= 2 ? std::to_integer<std::uint8_t>(pdu[1]) : 0;
            return DriverStatus::Error;
        }
        if (fc != kFcWriteMultipleRegisters || pdu_len != 5) {
            io_ok = false;  // an unexpected function code / short reply is a framing problem
            return DriverStatus::Error;
        }
        // A conformant server echoes address+quantity back on success; a mismatch means desync, not a
        // device-level error — treat it as a framing problem like the rest of this function.
        if (get_u16_be(&pdu[1]) != address || get_u16_be(&pdu[3]) != count) {
            io_ok = false;
            return DriverStatus::Error;
        }
        return DriverStatus::Ok;
    }

    std::string host_;
    std::uint16_t port_;
    std::uint8_t unit_id_;
    std::uint16_t start_address_;
    std::uint16_t register_count_;
    ReadFunction read_fn_;
    std::uint32_t rate_hz_ = 0;  // advisory only (see class banner)

    bool opened_ = false;
    quark::pal::fd_t fd_ = quark::pal::invalid_fd;
    std::uint16_t next_txn_id_ = 1;
    std::uint8_t last_exception_code_ = 0;

    int backoff_ms_ = kInitialBackoffMs;
    std::chrono::steady_clock::time_point next_attempt_at_{};
};

}  // namespace aero::drivers
