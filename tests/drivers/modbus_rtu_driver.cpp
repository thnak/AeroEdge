// AeroEdge M9.1 PR H gate (018 §8) — ModbusRtuDriver (drivers/modbus_rtu_driver.hpp) against an
// IN-MEMORY FAKE ISerialTransport, not real hardware. Unlike ModbusTcpDriver/OpcUaDriver's fake TCP
// server/UA_Server (both real, portable, no-hardware-needed), there is no portable way to fake a COM
// port without real hardware or a third-party virtual-COM driver — see modbus_rtu_driver.hpp's file
// banner. FakeSerialTransport below independently builds/parses raw Modbus-RTU byte frames (own CRC16
// implementation, deliberately NOT calling into the driver's private one — this is the test's own arm's-
// length fake, mirroring FakeModbusServer's posture in tests/drivers/modbus_tcp_driver.cpp) and records
// exactly what the driver wrote, so both directions (driver -> fake request bytes, fake -> driver
// response bytes) are independently verified.
//
// Covers:
//   (1) modbus_crc16's own worked example from the Modbus spec (slave 1, FC03, start 0, qty 10 -> CRC
//       bytes C5 CD) — a sign the test's own CRC helper (and, by construction, the driver's identical
//       algorithm) is right, not just internally self-consistent.
//   (2) happy path read (FC03): driver.poll() decodes a canned 2-register response correctly, AND the
//       driver's own request bytes (address/function/start/count/CRC) are exactly right.
//   (3) FC06 write round-trip against a canned echo response.
//   (4) FC16 write-multiple round-trip against a canned echo response.
//   (5) a well-formed Modbus EXCEPTION response is a clean Error with last_exception_code() set and the
//       transport left OPEN (a device-level error, not a connection problem).
//   (6) a CRC-corrupted response is a clean Error with the transport CLOSED (reconnect-eligible framing
//       problem), never a crash/hang.
//   (7) a response from the wrong slave address is likewise a clean Error with the transport closed.
//   (8) an oversized register_count is rejected at open(), config-time only, no transport I/O at all.
//   (9) a malformed write() target is rejected without any I/O (no bytes ever written to the transport).
// Deterministic, exit-code-gated (0 = pass), no real time/hardware dependency at all.
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <memory_resource>
#include <optional>
#include <vector>

#include "aero/drivers/modbus_rtu_driver.hpp"
#include "quark/core/stream_activation.hpp"

using aero::Frame;
using aero::drivers::ISerialTransport;
using aero::drivers::ModbusRtuDriver;
using RF = ModbusRtuDriver::ReadFunction;

namespace {

// ===== test's own, independent Modbus CRC16 (same algorithm, deliberately not shared with the driver's
// private one — see file banner) =========================================================================
std::uint16_t crc16(const std::uint8_t* data, std::size_t len) {
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
void append_crc(std::vector<std::uint8_t>& frame) {
    const std::uint16_t crc = crc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));         // low byte first (Modbus wire order)
    frame.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));  // high byte second
}

// ===== FakeSerialTransport: an in-memory ISerialTransport (see file banner) ============================
class FakeSerialTransport final : public ISerialTransport {
public:
    bool open_should_succeed = true;
    std::vector<std::uint8_t> last_written;
    std::deque<std::uint8_t> to_read;

    [[nodiscard]] bool open() noexcept override {
        opened_ = open_should_succeed;
        return opened_;
    }
    void close() noexcept override { opened_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return opened_; }

    [[nodiscard]] bool write_all(const std::uint8_t* data, std::size_t len) noexcept override {
        last_written.assign(data, data + len);
        return true;
    }
    [[nodiscard]] std::size_t read_some(std::uint8_t* buf, std::size_t len) noexcept override {
        const std::size_t n = to_read.size() < len ? to_read.size() : len;
        for (std::size_t i = 0; i < n; ++i) {
            buf[i] = to_read.front();
            to_read.pop_front();
        }
        return n;  // 0 if empty — matches aero::pal::read_serial's "0 == nothing available" contract
    }

    void queue(const std::vector<std::uint8_t>& bytes) { to_read.insert(to_read.end(), bytes.begin(), bytes.end()); }

private:
    bool opened_ = false;
};

// One poll() call against a fresh, throwaway StreamActivation — same one-per-call reasoning as
// modbus_tcp_driver.cpp/opcua_driver.cpp's own poll_once() helpers (a StreamSink is a single-producer,
// single-use token).
struct PollOutcome {
    aero::DriverStatus status = aero::DriverStatus::Error;
    std::optional<Frame> frame;
};
std::optional<Frame> drain_one(quark::StreamChannel<Frame>& ch) {
    if (ch.occupancy() > 0) {
        quark::StreamBatch<Frame> batch(ch, /*budget*/ 1);
        if (const Frame* f = batch.next()) {
            Frame copy = *f;
            batch.retire();
            return copy;
        }
    }
    return std::nullopt;
}
PollOutcome poll_once(ModbusRtuDriver& driver) {
    quark::StreamActivation<Frame>::Config scfg;
    scfg.capacity = 4;
    std::pmr::monotonic_buffer_resource mr;
    quark::StreamActivation<Frame> act(scfg, &mr);
    auto tok = quark::open_stream(act);
    if (!tok) return {};
    aero::StreamSink<Frame> sink(std::move(tok.value()));

    PollOutcome out;
    out.status = driver.poll(std::move(sink));
    if (out.status == aero::DriverStatus::Ok) out.frame = drain_one(act.channel());
    return out;
}

// ---- (1) modbus_crc16's worked example from the Modbus spec -------------------------------------------
bool test_crc16_known_vector() {
    // Slave 1, FC03 (Read Holding Registers), start address 0, quantity 10 -> CRC bytes C5 CD (widely
    // cited worked example, e.g. simplymodbus.ca's own FC03 walkthrough).
    const std::array<std::uint8_t, 6> req{0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    const std::uint16_t crc = crc16(req.data(), req.size());
    const bool ok = (crc & 0xFF) == 0xC5 && ((crc >> 8) & 0xFF) == 0xCD;
    if (!ok) std::printf("crc16_known_vector: got %02X %02X, expected C5 CD\n", crc & 0xFF, (crc >> 8) & 0xFF);
    return ok;
}

// ---- (2) happy path FC03 read: decode + exact request-byte verification -------------------------------
bool test_happy_path_read() {
    auto fake = std::make_unique<FakeSerialTransport>();
    FakeSerialTransport* raw = fake.get();

    // Canned response: slave 1, FC03, byte_count=4, data=[0x000A, 0x0100].
    std::vector<std::uint8_t> resp{0x01, 0x03, 0x04, 0x00, 0x0A, 0x01, 0x00};
    append_crc(resp);
    raw->queue(resp);

    ModbusRtuDriver driver(std::move(fake), /*slave*/ 1, /*start*/ 0, /*count*/ 2, RF::HoldingRegisters);
    aero::DriverConfig cfg{};
    bool ok = driver.open(cfg) == aero::DriverStatus::Ok;

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Ok;
    ok &= outcome.frame.has_value();
    if (outcome.frame) {
        ok &= outcome.frame->payload_len == 4;
        ok &= outcome.frame->payload[0] == static_cast<std::byte>(0x00);
        ok &= outcome.frame->payload[1] == static_cast<std::byte>(0x0A);
        ok &= outcome.frame->payload[2] == static_cast<std::byte>(0x01);
        ok &= outcome.frame->payload[3] == static_cast<std::byte>(0x00);
    }

    // Verify the request the driver actually sent: addr=1, fc=3, start=0, count=2, + a valid trailing CRC.
    std::vector<std::uint8_t> expected_req{0x01, 0x03, 0x00, 0x00, 0x00, 0x02};
    append_crc(expected_req);
    ok &= raw->last_written == expected_req;

    driver.close();
    if (!ok) std::printf("happy_path_read: assertion failed\n");
    return ok;
}

// ---- (3) FC06 write round-trip -------------------------------------------------------------------------
bool test_write_single_register() {
    auto fake = std::make_unique<FakeSerialTransport>();
    FakeSerialTransport* raw = fake.get();

    std::vector<std::uint8_t> resp{0x01, 0x06, 0x00, 0x05, 0x00, 0x2A};  // echo: addr=5, value=42
    append_crc(resp);
    raw->queue(resp);

    ModbusRtuDriver driver(std::move(fake), /*slave*/ 1, /*start*/ 0, /*count*/ 1, RF::HoldingRegisters);
    aero::DriverConfig cfg{};
    bool ok = driver.open(cfg) == aero::DriverStatus::Ok;

    ok &= driver.write(aero::DeviceCommand{"5", 42}) == aero::DriverStatus::Ok;

    std::vector<std::uint8_t> expected_req{0x01, 0x06, 0x00, 0x05, 0x00, 0x2A};
    append_crc(expected_req);
    ok &= raw->last_written == expected_req;

    driver.close();
    if (!ok) std::printf("write_single_register: assertion failed\n");
    return ok;
}

// ---- (4) FC16 write-multiple round-trip ------------------------------------------------------------------
bool test_write_multiple_registers() {
    auto fake = std::make_unique<FakeSerialTransport>();
    FakeSerialTransport* raw = fake.get();

    std::vector<std::uint8_t> resp{0x01, 0x10, 0x00, 0x05, 0x00, 0x03};  // echo: addr=5, qty=3
    append_crc(resp);
    raw->queue(resp);

    ModbusRtuDriver driver(std::move(fake), /*slave*/ 1, /*start*/ 0, /*count*/ 3, RF::HoldingRegisters);
    aero::DriverConfig cfg{};
    bool ok = driver.open(cfg) == aero::DriverStatus::Ok;

    ok &= driver.write(aero::DeviceCommand{"5,10,20,30", 0}) == aero::DriverStatus::Ok;

    std::vector<std::uint8_t> expected_req{0x01, 0x10, 0x00, 0x05, 0x00, 0x03, 0x06,
                                            0x00, 0x0A, 0x00, 0x14, 0x00, 0x1E};
    append_crc(expected_req);
    ok &= raw->last_written == expected_req;

    driver.close();
    if (!ok) std::printf("write_multiple_registers: assertion failed\n");
    return ok;
}

// ---- (5) a well-formed exception response is a clean Error, transport stays open ------------------------
bool test_exception_response() {
    auto fake = std::make_unique<FakeSerialTransport>();
    FakeSerialTransport* raw = fake.get();

    std::vector<std::uint8_t> resp{0x01, 0x83, 0x02};  // FC03 exception, code 0x02 (Illegal Data Address)
    append_crc(resp);
    raw->queue(resp);

    ModbusRtuDriver driver(std::move(fake), /*slave*/ 1, /*start*/ 0, /*count*/ 2, RF::HoldingRegisters);
    aero::DriverConfig cfg{};
    bool ok = driver.open(cfg) == aero::DriverStatus::Ok;

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Error;
    ok &= driver.last_exception_code() == 0x02;
    ok &= raw->is_open();  // device-level error, NOT a connection problem — transport must stay open

    driver.close();
    if (!ok) std::printf("exception_response: assertion failed\n");
    return ok;
}

// ---- (6) a CRC-corrupted response is a clean Error, transport CLOSED (reconnect-eligible) ---------------
bool test_crc_corrupted_response() {
    auto fake = std::make_unique<FakeSerialTransport>();
    FakeSerialTransport* raw = fake.get();

    std::vector<std::uint8_t> resp{0x01, 0x03, 0x02, 0x00, 0x0A, 0xFF, 0xFF};  // deliberately wrong CRC
    raw->queue(resp);

    ModbusRtuDriver driver(std::move(fake), /*slave*/ 1, /*start*/ 0, /*count*/ 1, RF::HoldingRegisters);
    aero::DriverConfig cfg{};
    bool ok = driver.open(cfg) == aero::DriverStatus::Ok;

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Error;
    ok &= !raw->is_open();  // framing problem -> transport reset for a later reconnect

    driver.close();
    if (!ok) std::printf("crc_corrupted_response: assertion failed\n");
    return ok;
}

// ---- (7) a response from the wrong slave address is a clean Error, transport CLOSED ---------------------
bool test_wrong_slave_address() {
    auto fake = std::make_unique<FakeSerialTransport>();
    FakeSerialTransport* raw = fake.get();

    std::vector<std::uint8_t> resp{0x02, 0x03, 0x02, 0x00, 0x0A};  // addr=2, driver configured for addr=1
    append_crc(resp);
    raw->queue(resp);

    ModbusRtuDriver driver(std::move(fake), /*slave*/ 1, /*start*/ 0, /*count*/ 1, RF::HoldingRegisters);
    aero::DriverConfig cfg{};
    bool ok = driver.open(cfg) == aero::DriverStatus::Ok;

    const auto outcome = poll_once(driver);
    ok &= outcome.status == aero::DriverStatus::Error;
    ok &= !raw->is_open();

    driver.close();
    if (!ok) std::printf("wrong_slave_address: assertion failed\n");
    return ok;
}

// ---- (8) an oversized register_count is rejected at open(), no transport I/O at all ---------------------
bool test_oversized_rejected() {
    auto fake = std::make_unique<FakeSerialTransport>();
    FakeSerialTransport* raw = fake.get();

    // 100 holding registers * 2 bytes = 200 > kMaxFramePayload (128).
    ModbusRtuDriver driver(std::move(fake), /*slave*/ 1, /*start*/ 0, /*count*/ 100, RF::HoldingRegisters);
    aero::DriverConfig cfg{};
    const bool ok = driver.open(cfg) == aero::DriverStatus::Error && !raw->is_open();
    if (!ok) std::printf("oversized_rejected: assertion failed\n");
    return ok;
}

// ---- (9) a malformed write() target is rejected without any I/O -----------------------------------------
bool test_write_invalid_target() {
    auto fake = std::make_unique<FakeSerialTransport>();
    FakeSerialTransport* raw = fake.get();

    ModbusRtuDriver driver(std::move(fake), /*slave*/ 1, /*start*/ 0, /*count*/ 1, RF::HoldingRegisters);
    aero::DriverConfig cfg{};
    bool ok = driver.open(cfg) == aero::DriverStatus::Ok;

    ok &= driver.write(aero::DeviceCommand{"not-a-number", 1}) == aero::DriverStatus::Error;
    ok &= raw->last_written.empty();  // parse failure must short-circuit before any transport write

    driver.close();
    if (!ok) std::printf("write_invalid_target: assertion failed\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    const bool crc_ok = test_crc16_known_vector();
    ok &= crc_ok;
    std::printf("[crc16_known_vector] %s\n", crc_ok ? "ok" : "FAIL");

    const bool happy_ok = test_happy_path_read();
    ok &= happy_ok;
    std::printf("[happy_path_read] %s\n", happy_ok ? "ok" : "FAIL");

    const bool write_ok = test_write_single_register();
    ok &= write_ok;
    std::printf("[write_single_register] %s\n", write_ok ? "ok" : "FAIL");

    const bool write_multi_ok = test_write_multiple_registers();
    ok &= write_multi_ok;
    std::printf("[write_multiple_registers] %s\n", write_multi_ok ? "ok" : "FAIL");

    const bool exception_ok = test_exception_response();
    ok &= exception_ok;
    std::printf("[exception_response] %s\n", exception_ok ? "ok" : "FAIL");

    const bool crc_corrupt_ok = test_crc_corrupted_response();
    ok &= crc_corrupt_ok;
    std::printf("[crc_corrupted_response] %s\n", crc_corrupt_ok ? "ok" : "FAIL");

    const bool wrong_slave_ok = test_wrong_slave_address();
    ok &= wrong_slave_ok;
    std::printf("[wrong_slave_address] %s\n", wrong_slave_ok ? "ok" : "FAIL");

    const bool oversized_ok = test_oversized_rejected();
    ok &= oversized_ok;
    std::printf("[oversized_rejected] %s\n", oversized_ok ? "ok" : "FAIL");

    const bool invalid_target_ok = test_write_invalid_target();
    ok &= invalid_target_ok;
    std::printf("[write_invalid_target] %s\n", invalid_target_ok ? "ok" : "FAIL");

    std::printf("modbus_rtu_driver: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
