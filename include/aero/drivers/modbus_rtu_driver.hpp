// AeroEdge M9.1 PR H driver — ModbusRtuDriver: the serial/RTU counterpart to ModbusTcpDriver (018 §8's
// "Modbus RTU / serial transport" backlog item), over aero::pal::serial (aero/pal/serial.hpp, this
// PR's new PAL primitive — this codebase had ZERO serial I/O before it). Same PULL-only posture as every
// other driver in this file's family (006 §6.1): `run()` is a hard Unsupported, every read/write happens
// inside one poll()/write() call.
//
// NOT A REFACTOR OF ModbusTcpDriver: its FC-byte-encoding logic is entangled with MBAP framing and
// `quark::pal::fd_t` throughout do_transaction()/do_write_transaction()/do_write_multiple_transaction() —
// pulling it apart would mean touching a shipped, working, tested driver for this PR's sake. RTU framing
// is genuinely different (`[addr:1][PDU][CRC16-LE:2]`, no length field, no transaction id — the request/
// response pairing is implicit in half-duplex serial ordering) and short enough (~10-30 lines per
// function) that duplicating the FC-byte shapes here, with RTU framing instead of MBAP, is the smaller
// and safer move — this codebase's own stated posture ("three similar lines beats premature abstraction").
//
// TESTABILITY: unlike a TCP loopback server (real, portable, no hardware needed), there is no portable
// way to fake a COM port without real hardware or a third-party virtual-COM driver — so this header
// defines `ISerialTransport`, a minimal seam `ModbusRtuDriver` talks to instead of `aero::pal::serial`
// directly. Production code gets `RealSerialTransport` (thin wrapper over the PAL); tests inject an
// in-memory fake that echoes canned RTU frames (tests/drivers/modbus_rtu_driver.cpp). This is the ONE
// abstraction this PR adds beyond duplicating ModbusTcpDriver's shape, and it exists purely because
// hardware-free serial testing has no other option — not a speculative "might need it later" seam.
//
// RECONNECT (006 §8): same bounded-backoff shape as every other driver here — opening a serial port can
// fail (device unplugged, wrong path) exactly like a TCP dial can, so poll()/write() retry it lazily with
// the same 200ms-doubling-to-5s-cap backoff, reset on success.
#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "aero/pal/serial.hpp"
#include "aero/sdk/driver.hpp"

namespace aero::drivers {

// The seam a real serial port and a test fake both implement — see file banner. `open()`/`is_open()`
// mirror a connect/dial lifecycle (ModbusRtuDriver's ensure_connected() calls `open()` lazily, exactly
// like ModbusTcpDriver's dial_tcp() call); `write_all`/`read_some` are the byte-level primitives
// ModbusRtuDriver's own send/recv loops are built from.
class ISerialTransport {
public:
    virtual ~ISerialTransport() = default;
    [[nodiscard]] virtual bool open() noexcept = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    // Blocking, whole-buffer write. false == a genuine I/O failure (caller treats as connection-lost).
    [[nodiscard]] virtual bool write_all(const std::uint8_t* data, std::size_t len) noexcept = 0;
    // Blocks up to the transport's own configured read timeout; returns bytes actually read (0 ==
    // timeout, not an error — mirrors aero::pal::read_serial's own contract, see its file banner).
    [[nodiscard]] virtual std::size_t read_some(std::uint8_t* buf, std::size_t len) noexcept = 0;
};

// Production ISerialTransport: a thin wrapper over aero::pal::serial. Owns the port name/config; the
// actual OS handle is acquired lazily in open() (never in the constructor), matching ModbusTcpDriver's
// own "opened_ armed before the dial attempt" posture.
class RealSerialTransport final : public ISerialTransport {
public:
    RealSerialTransport(std::string port, aero::pal::SerialConfig cfg) noexcept
        : port_(std::move(port)), cfg_(cfg) {}

    ~RealSerialTransport() override { close(); }

    [[nodiscard]] bool open() noexcept override {
        auto r = aero::pal::open_serial(port_, cfg_);
        if (!r) return false;
        handle_ = *r;
        return true;
    }

    void close() noexcept override {
        if (handle_ != aero::pal::invalid_serial_handle) {
            aero::pal::close_serial(handle_);
            handle_ = aero::pal::invalid_serial_handle;
        }
    }

    [[nodiscard]] bool is_open() const noexcept override { return handle_ != aero::pal::invalid_serial_handle; }

    [[nodiscard]] bool write_all(const std::uint8_t* data, std::size_t len) noexcept override {
        std::size_t sent = 0;
        while (sent < len) {
            auto w = aero::pal::write_serial(handle_, data + sent, len - sent);
            if (!w || *w == 0) return false;  // 0 from a write is not the timeout idiom read has — a stall
            sent += *w;
        }
        return true;
    }

    [[nodiscard]] std::size_t read_some(std::uint8_t* buf, std::size_t len) noexcept override {
        auto r = aero::pal::read_serial(handle_, buf, len);
        return r ? *r : 0;  // an I/O error collapses to "0 bytes this call" — the caller's own overall
                             // deadline (ModbusRtuDriver::recv_exact) is what ultimately times the request
                             // out; a real device error and a real timeout look the same to a poll caller.
    }

private:
    std::string port_;
    aero::pal::SerialConfig cfg_;
    aero::pal::serial_handle_t handle_ = aero::pal::invalid_serial_handle;
};

// ModbusRtuDriver — FC01/02/03/04 read + FC06/FC16 write, over RTU framing (see file banner). Construction
// takes ownership of its config, same posture as ModbusTcpDriver (DriverConfig's endpoint/frame_count
// fields don't fit this driver's shape either, beyond the advisory rate_hz).
class ModbusRtuDriver final : public IDriver {
public:
    enum class ReadFunction : std::uint8_t {
        Coils = 0x01,
        DiscreteInputs = 0x02,
        HoldingRegisters = 0x03,
        InputRegisters = 0x04,
    };

    // Production constructor: opens `port` (e.g. "COM3" on Windows, "/dev/ttyUSB0" on POSIX) lazily, in
    // open()/ensure_connected() — never here.
    ModbusRtuDriver(std::string port, std::uint32_t baud_rate, std::uint8_t slave_address,
                     std::uint16_t start_address, std::uint16_t register_count,
                     ReadFunction read_fn = ReadFunction::HoldingRegisters, char parity = 'N',
                     std::uint8_t stop_bits = 1) noexcept
        : slave_address_(slave_address),
          start_address_(start_address),
          register_count_(register_count),
          read_fn_(read_fn),
          transport_(std::make_unique<RealSerialTransport>(
              std::move(port),
              aero::pal::SerialConfig{baud_rate, parity, stop_bits, kReadTimeoutMs})) {}

    // Test-only constructor (tests/drivers/modbus_rtu_driver.cpp): inject a fake transport in place of a
    // real COM port — see file banner on why no real-hardware-free alternative exists.
    ModbusRtuDriver(std::unique_ptr<ISerialTransport> transport, std::uint8_t slave_address,
                     std::uint16_t start_address, std::uint16_t register_count,
                     ReadFunction read_fn = ReadFunction::HoldingRegisters) noexcept
        : slave_address_(slave_address),
          start_address_(start_address),
          register_count_(register_count),
          read_fn_(read_fn),
          transport_(std::move(transport)) {}

    // Config-time rejection, same reasoning/formula as ModbusTcpDriver::open() (Part 1's
    // Frame::payload_len/kMaxFramePayload budget — never a silently truncated frame).
    DriverStatus open(const DriverConfig& cfg) noexcept override {
        if (expected_response_bytes(read_fn_, register_count_) > aero::kMaxFramePayload) {
            return DriverStatus::Error;
        }
        rate_hz_ = cfg.rate_hz;  // advisory poll-interval hint only (see class banner)
        opened_ = true;

        backoff_ms_ = kInitialBackoffMs;
        next_attempt_at_ = std::chrono::steady_clock::now();
        return transport_->open() ? DriverStatus::Ok : DriverStatus::Error;
    }

    // PULL driver only (006 §6.1) — this class never loops; poll() is the whole surface.
    DriverStatus run(StreamSink<Frame> /*sink*/, StopToken /*stop*/) noexcept override {
        return DriverStatus::Unsupported;
    }

    // One Modbus-RTU request/response, non-looping. Same io_ok/reconnect posture as ModbusTcpDriver::
    // poll(): a transport/framing failure closes the port so the NEXT poll() reconnects; a clean Modbus
    // exception response is a healthy transport reporting a device-level error (transport stays open).
    DriverStatus poll(StreamSink<Frame> sink) noexcept override {
        if (!opened_) return DriverStatus::Error;
        if (!transport_->is_open() && !ensure_connected()) return DriverStatus::Error;

        Frame frame{};
        bool io_ok = true;
        const DriverStatus st = do_transaction(frame, io_ok);
        if (!io_ok) transport_->close();  // (006 §8) -> reconnect w/ backoff on a later poll()
        if (st != DriverStatus::Ok) return st;

        while (!sink.try_push(frame)) {
            std::this_thread::yield();  // lossless backpressure (006 §3): stall, never drop
        }
        return DriverStatus::Ok;
    }

    // FC06 (Write Single Register) or FC16 (Write Multiple Registers) — identical `cmd.target` encoding
    // to ModbusTcpDriver::write() ("addr" bare, or "addr,v1,v2,..." comma-separated), same
    // kMaxWriteMultipleRegisters cap (Modbus's own FC16 implementation limit, protocol-level, not
    // transport-specific).
    DriverStatus write(const DeviceCommand& cmd) noexcept override {
        if (!opened_) return DriverStatus::Error;

        const auto comma = cmd.target.find(',');
        if (comma == std::string_view::npos) {
            if (cmd.value < 0 || cmd.value > 0xFFFF) return DriverStatus::Error;
            std::uint16_t address = 0;
            if (!parse_u16(cmd.target, address)) return DriverStatus::Error;

            if (!transport_->is_open() && !ensure_connected()) return DriverStatus::Error;
            bool io_ok = true;
            const DriverStatus st =
                do_write_transaction(address, static_cast<std::uint16_t>(cmd.value), io_ok);
            if (!io_ok) transport_->close();
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

        if (!transport_->is_open() && !ensure_connected()) return DriverStatus::Error;
        bool io_ok = true;
        const DriverStatus st = do_write_multiple_transaction(address, values.data(), count, io_ok);
        if (!io_ok) transport_->close();
        return st;
    }

    void close() noexcept override { transport_->close(); }

    const DriverDescriptor& descriptor() const noexcept override { return kDesc; }

    // Observability, same shape as ModbusTcpDriver::last_exception_code().
    [[nodiscard]] std::uint8_t last_exception_code() const noexcept { return last_exception_code_; }

    static constexpr DriverDescriptor kDesc{"aero.driver.modbus_rtu", /*writable*/ true, /*poll_driven*/ true};

private:
    static constexpr int kReadTimeoutMs = 200;    // per-call transport read timeout (aero::pal::serial)
    static constexpr int kIoTimeoutMs = 1000;      // bounded total time for one send/recv-exact call
    static constexpr int kInitialBackoffMs = 200;
    static constexpr int kMaxBackoffMs = 5000;
    static constexpr std::uint8_t kFcWriteSingleRegister = 0x06;
    static constexpr std::uint8_t kFcWriteMultipleRegisters = 0x10;
    static constexpr std::uint8_t kFcExceptionBit = 0x80;
    static constexpr std::size_t kMaxWriteMultipleRegisters = 123;  // Modbus's own FC16 limit

    static bool parse_u16(std::string_view s, std::uint16_t& out) noexcept {
        if (s.empty()) return false;
        const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
        return res.ec == std::errc{} && res.ptr == s.data() + s.size();
    }

    static constexpr bool is_bit_packed(ReadFunction fn) noexcept {
        return fn == ReadFunction::Coils || fn == ReadFunction::DiscreteInputs;
    }
    static constexpr std::size_t expected_response_bytes(ReadFunction fn, std::uint16_t count) noexcept {
        return is_bit_packed(fn) ? (static_cast<std::size_t>(count) + 7) / 8
                                  : static_cast<std::size_t>(count) * 2;
    }

    bool ensure_connected() noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_attempt_at_) return false;

        if (!transport_->open()) {
            next_attempt_at_ = now + std::chrono::milliseconds(backoff_ms_);
            backoff_ms_ = backoff_ms_ * 2 < kMaxBackoffMs ? backoff_ms_ * 2 : kMaxBackoffMs;
            return false;
        }
        backoff_ms_ = kInitialBackoffMs;  // reset on success
        next_attempt_at_ = now;
        return true;
    }

    static void put_u16_be(std::uint8_t* p, std::uint16_t v) noexcept {
        p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        p[1] = static_cast<std::uint8_t>(v & 0xFF);
    }
    static std::uint16_t get_u16_be(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
    }
    // Modbus's own CRC is little-endian on the wire (low byte transmitted first) — distinct from the
    // register/address fields above, which are big-endian like the rest of the protocol.
    static std::uint16_t get_u16_le(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
    }

    // Modbus RTU CRC-16: poly 0xA001 (reflected), init 0xFFFF, no final XOR. Deliberately NOT reusing
    // aero::nodes::CrcNode (compute_nodes.hpp) — that one is CRC-16/CCITT-FALSE (poly 0x1021, MSB-first),
    // an entirely different algorithm; Modbus's own CRC is a distinct, protocol-mandated variant. Verified
    // against the Modbus spec's own worked example in tests/drivers/modbus_rtu_driver.cpp.
    static std::uint16_t modbus_crc16(const std::uint8_t* data, std::size_t len) noexcept {
        std::uint16_t crc = 0xFFFF;
        for (std::size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int b = 0; b < 8; ++b) {
                if (crc & 0x0001) crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xA001);
                else crc = static_cast<std::uint16_t>(crc >> 1);
            }
        }
        return crc;
    }

    bool write_all(const std::uint8_t* buf, std::size_t n) noexcept { return transport_->write_all(buf, n); }

    // Bounded recv-exact over the transport, mirroring ModbusTcpDriver's own recv_exact() shape: loop
    // read_some() (which already blocks up to its own per-call timeout) until `n` bytes are collected or
    // this transaction's OVERALL deadline elapses. false == I/O failure or timed out — connection-lost.
    bool recv_exact(std::uint8_t* buf, std::size_t n) noexcept {
        std::size_t got = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
        while (got < n) {
            const std::size_t r = transport_->read_some(buf + got, n - got);
            if (r == 0) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                continue;
            }
            got += r;
        }
        return true;
    }

    // One FC01/02/03/04 request/response transaction. `io_ok` false ONLY for a transport/framing problem
    // (send/recv failure, wrong slave address, CRC mismatch, unexpected function code) — the caller
    // resets the transport on that. A clean Modbus EXCEPTION response is a well-formed answer over a
    // healthy transport: io_ok stays true, this just returns Error with last_exception_code_ recorded.
    DriverStatus do_transaction(Frame& out, bool& io_ok) noexcept {
        std::array<std::uint8_t, 8> req{};
        req[0] = slave_address_;
        req[1] = static_cast<std::uint8_t>(read_fn_);
        put_u16_be(&req[2], start_address_);
        put_u16_be(&req[4], register_count_);
        const std::uint16_t crc = modbus_crc16(req.data(), 6);
        req[6] = static_cast<std::uint8_t>(crc & 0xFF);
        req[7] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);

        if (!write_all(req.data(), req.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }

        std::array<std::uint8_t, 2> head{};  // addr(1) + fc(1)
        if (!recv_exact(head.data(), head.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }
        if (head[0] != slave_address_) {
            io_ok = false;  // a different/no slave replied — desynced, don't trust the byte stream
            return DriverStatus::Error;
        }

        const std::uint8_t fc = head[1];
        if ((fc & kFcExceptionBit) != 0) {
            std::array<std::uint8_t, 3> rest{};  // exception_code(1) + CRC(2)
            if (!recv_exact(rest.data(), rest.size())) {
                io_ok = false;
                return DriverStatus::Error;
            }
            const std::array<std::uint8_t, 3> to_crc{head[0], head[1], rest[0]};
            if (modbus_crc16(to_crc.data(), to_crc.size()) != get_u16_le(&rest[1])) {
                io_ok = false;  // corrupt frame, not a device-level answer we can trust
                return DriverStatus::Error;
            }
            last_exception_code_ = rest[0];
            return DriverStatus::Error;  // well-formed exception; io_ok stays true
        }
        if (fc != static_cast<std::uint8_t>(read_fn_)) {
            io_ok = false;  // an unexpected function code is a framing problem, not a device error
            return DriverStatus::Error;
        }

        std::uint8_t byte_count = 0;
        if (!recv_exact(&byte_count, 1)) {
            io_ok = false;
            return DriverStatus::Error;
        }
        const std::size_t expected_bytes = expected_response_bytes(read_fn_, register_count_);
        if (byte_count != expected_bytes) {
            io_ok = false;  // byte-count field doesn't match what we asked for — desynced
            return DriverStatus::Error;
        }

        std::array<std::uint8_t, aero::kMaxFramePayload + 2> data_and_crc{};  // data(N) + CRC(2)
        if (!recv_exact(data_and_crc.data(), expected_bytes + 2)) {
            io_ok = false;
            return DriverStatus::Error;
        }

        std::array<std::uint8_t, 3 + aero::kMaxFramePayload> to_crc{};  // addr+fc+byte_count+data
        to_crc[0] = head[0];
        to_crc[1] = head[1];
        to_crc[2] = byte_count;
        for (std::size_t i = 0; i < expected_bytes; ++i) to_crc[3 + i] = data_and_crc[i];
        if (modbus_crc16(to_crc.data(), 3 + expected_bytes) != get_u16_le(&data_and_crc[expected_bytes])) {
            io_ok = false;
            return DriverStatus::Error;
        }

        out.payload_len = static_cast<std::uint16_t>(expected_bytes);
        for (std::size_t i = 0; i < expected_bytes; ++i) {
            out.payload[i] = static_cast<std::byte>(data_and_crc[i]);
        }
        // Same Frame::raw padding-bit stash as ModbusTcpDriver — see ModbusBitsDecodeNode.
        if (is_bit_packed(read_fn_)) out.raw = static_cast<std::int64_t>(register_count_);
        return DriverStatus::Ok;
    }

    // One FC06 request/response transaction. Same io_ok/exception contract as do_transaction().
    DriverStatus do_write_transaction(std::uint16_t address, std::uint16_t value, bool& io_ok) noexcept {
        std::array<std::uint8_t, 8> req{};
        req[0] = slave_address_;
        req[1] = kFcWriteSingleRegister;
        put_u16_be(&req[2], address);
        put_u16_be(&req[4], value);
        const std::uint16_t crc = modbus_crc16(req.data(), 6);
        req[6] = static_cast<std::uint8_t>(crc & 0xFF);
        req[7] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);

        if (!write_all(req.data(), req.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }

        std::array<std::uint8_t, 2> head{};
        if (!recv_exact(head.data(), head.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }
        if (head[0] != slave_address_) {
            io_ok = false;
            return DriverStatus::Error;
        }

        const std::uint8_t fc = head[1];
        if ((fc & kFcExceptionBit) != 0) {
            std::array<std::uint8_t, 3> rest{};
            if (!recv_exact(rest.data(), rest.size())) {
                io_ok = false;
                return DriverStatus::Error;
            }
            const std::array<std::uint8_t, 3> to_crc{head[0], head[1], rest[0]};
            if (modbus_crc16(to_crc.data(), to_crc.size()) != get_u16_le(&rest[1])) {
                io_ok = false;
                return DriverStatus::Error;
            }
            last_exception_code_ = rest[0];
            return DriverStatus::Error;
        }
        if (fc != kFcWriteSingleRegister) {
            io_ok = false;
            return DriverStatus::Error;
        }

        std::array<std::uint8_t, 6> rest{};  // addr(2) + value(2) + CRC(2)
        if (!recv_exact(rest.data(), rest.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }
        std::array<std::uint8_t, 6> to_crc{head[0], head[1], rest[0], rest[1], rest[2], rest[3]};
        if (modbus_crc16(to_crc.data(), to_crc.size()) != get_u16_le(&rest[4])) {
            io_ok = false;
            return DriverStatus::Error;
        }
        // A conformant slave echoes address+value back on success; a mismatch means desync, not a
        // device-level error — treat it as a framing problem like the rest of this function.
        if (get_u16_be(&rest[0]) != address || get_u16_be(&rest[2]) != value) {
            io_ok = false;
            return DriverStatus::Error;
        }
        return DriverStatus::Ok;
    }

    // One FC16 request/response transaction. Same io_ok/exception contract as do_write_transaction().
    // `count` is already bounded to kMaxWriteMultipleRegisters by the caller (write()).
    DriverStatus do_write_multiple_transaction(std::uint16_t address, const std::uint16_t* values,
                                                std::size_t count, bool& io_ok) noexcept {
        const auto byte_count = static_cast<std::uint8_t>(count * 2);
        std::array<std::uint8_t, 9 + 2 * kMaxWriteMultipleRegisters> req{};
        req[0] = slave_address_;
        req[1] = kFcWriteMultipleRegisters;
        put_u16_be(&req[2], address);
        put_u16_be(&req[4], static_cast<std::uint16_t>(count));
        req[6] = byte_count;
        for (std::size_t i = 0; i < count; ++i) put_u16_be(&req[7 + 2 * i], values[i]);
        const std::size_t body_len = 7 + byte_count;  // addr+fc+addr+qty+bytecount+data, before CRC
        const std::uint16_t crc = modbus_crc16(req.data(), body_len);
        req[body_len] = static_cast<std::uint8_t>(crc & 0xFF);
        req[body_len + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);

        if (!write_all(req.data(), body_len + 2)) {
            io_ok = false;
            return DriverStatus::Error;
        }

        std::array<std::uint8_t, 2> head{};
        if (!recv_exact(head.data(), head.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }
        if (head[0] != slave_address_) {
            io_ok = false;
            return DriverStatus::Error;
        }

        const std::uint8_t fc = head[1];
        if ((fc & kFcExceptionBit) != 0) {
            std::array<std::uint8_t, 3> rest{};
            if (!recv_exact(rest.data(), rest.size())) {
                io_ok = false;
                return DriverStatus::Error;
            }
            const std::array<std::uint8_t, 3> to_crc{head[0], head[1], rest[0]};
            if (modbus_crc16(to_crc.data(), to_crc.size()) != get_u16_le(&rest[1])) {
                io_ok = false;
                return DriverStatus::Error;
            }
            last_exception_code_ = rest[0];
            return DriverStatus::Error;
        }
        if (fc != kFcWriteMultipleRegisters) {
            io_ok = false;
            return DriverStatus::Error;
        }

        std::array<std::uint8_t, 6> rest{};  // addr(2) + qty(2) + CRC(2)
        if (!recv_exact(rest.data(), rest.size())) {
            io_ok = false;
            return DriverStatus::Error;
        }
        std::array<std::uint8_t, 6> to_crc{head[0], head[1], rest[0], rest[1], rest[2], rest[3]};
        if (modbus_crc16(to_crc.data(), to_crc.size()) != get_u16_le(&rest[4])) {
            io_ok = false;
            return DriverStatus::Error;
        }
        if (get_u16_be(&rest[0]) != address || get_u16_be(&rest[2]) != count) {
            io_ok = false;
            return DriverStatus::Error;
        }
        return DriverStatus::Ok;
    }

    std::uint8_t slave_address_;
    std::uint16_t start_address_;
    std::uint16_t register_count_;
    ReadFunction read_fn_;
    std::uint32_t rate_hz_ = 0;  // advisory only (see class banner)

    std::unique_ptr<ISerialTransport> transport_;
    bool opened_ = false;
    std::uint8_t last_exception_code_ = 0;

    int backoff_ms_ = kInitialBackoffMs;
    std::chrono::steady_clock::time_point next_attempt_at_{};
};

}  // namespace aero::drivers
