// AeroEdge PAL — serial port (COM/tty) primitives, the transport ModbusRtuDriver (drivers/
// modbus_rtu_driver.hpp, M9.1 PR H, 018 §8) needs and this codebase has never had before (confirmed
// greenfield — no prior termios/DCB/SetCommState anywhere in this tree). Mirrors this directory's own
// style (aero/pal/net_dial.hpp, aero/pal/poll.hpp): a single header, `namespace detail { #if _WIN32 ...
// #else ... #endif }` for the per-OS bodies, `std::expected<T, std::string>` for the fallible calls — NOT
// QuarkCpp's own PAL convention (per-OS directories dispatched via `pal/net.hpp`), which is QuarkCpp's
// own layering for socket primitives specifically; this is an AeroEdge-owned PAL header for a primitive
// QuarkCpp has no concept of at all (a byte-oriented serial device, not a socket `fd_t`) — a distinct
// `serial_handle_t` (HANDLE on Windows, POSIX `int` fd otherwise), never `quark::pal::fd_t`.
//
// TIMEOUT MODEL: the read timeout is a property of the OPEN handle (`SerialConfig::read_timeout_ms`,
// configured once at `open_serial()` — Windows COMMTIMEOUTS and POSIX termios VMIN/VTIME are both
// per-handle knobs, not per-call), not a per-call parameter like `aero::pal::wait_readable`'s timeout_ms.
// `read_serial()` blocks up to that configured timeout and returns however many bytes actually arrived —
// 0 is a timeout, NOT an error (mirrors a non-blocking socket `recv_some()`'s "0 means try again" shape,
// letting a caller build the same bounded recv_exact()-over-partial-reads loop ModbusTcpDriver already
// has, just polling read_serial() in a loop instead of send/recv+wait_readable).
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace aero::pal {

#if defined(_WIN32)
using serial_handle_t = HANDLE;
inline const serial_handle_t invalid_serial_handle = INVALID_HANDLE_VALUE;
#else
using serial_handle_t = int;
inline constexpr serial_handle_t invalid_serial_handle = -1;
#endif

// Modbus RTU is always 8 data bits (spec) — data_bits isn't a knob here, only what real field devices
// actually vary: baud, parity, stop bits. `read_timeout_ms` is the per-READ-CALL timeout (see file
// banner) — Modbus RTU response times are typically well under a second, so a caller's overall
// transaction deadline (ModbusRtuDriver's own kIoTimeoutMs) is built from several of these, not one.
struct SerialConfig {
    std::uint32_t baud_rate = 9600;
    char parity = 'N';           // 'N' none, 'E' even, 'O' odd — the only three Modbus RTU allows
    std::uint8_t stop_bits = 1;  // 1 or 2
    int read_timeout_ms = 200;
};

namespace detail {
#if defined(_WIN32)

[[nodiscard]] inline std::expected<serial_handle_t, std::string> open_serial_impl(
    std::string_view port, const SerialConfig& cfg) noexcept {
    // "\\\\.\\COMn" is required for COM10+ (bare "COMn" only works for COM1-COM9) — always safe to use.
    const std::string path = "\\\\.\\" + std::string(port);
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::unexpected("CreateFileA failed for " + std::string(port));

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!::GetCommState(h, &dcb)) {
        ::CloseHandle(h);
        return std::unexpected("GetCommState failed for " + std::string(port));
    }
    dcb.BaudRate = cfg.baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity = cfg.parity == 'E' ? EVENPARITY : (cfg.parity == 'O' ? ODDPARITY : NOPARITY);
    dcb.fParity = cfg.parity != 'N';
    dcb.StopBits = cfg.stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!::SetCommState(h, &dcb)) {
        ::CloseHandle(h);
        return std::unexpected("SetCommState failed for " + std::string(port));
    }

    // ReadIntervalTimeout=MAXDWORD + ReadTotalTimeoutConstant>0 is the documented Win32 idiom for "return
    // immediately with whatever's already buffered, else wait up to the constant for at least one byte" —
    // matches this header's "0 == timeout, not an error" read_serial() contract exactly.
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(cfg.read_timeout_ms);
    timeouts.WriteTotalTimeoutConstant = 2000;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    if (!::SetCommTimeouts(h, &timeouts)) {
        ::CloseHandle(h);
        return std::unexpected("SetCommTimeouts failed for " + std::string(port));
    }

    ::PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return h;
}

inline void close_serial_impl(serial_handle_t h) noexcept {
    if (h != invalid_serial_handle) ::CloseHandle(h);
}

[[nodiscard]] inline std::expected<std::size_t, std::string> write_serial_impl(serial_handle_t h,
                                                                                const std::uint8_t* buf,
                                                                                std::size_t len) noexcept {
    DWORD written = 0;
    if (!::WriteFile(h, buf, static_cast<DWORD>(len), &written, nullptr)) {
        return std::unexpected("WriteFile failed");
    }
    return static_cast<std::size_t>(written);
}

[[nodiscard]] inline std::expected<std::size_t, std::string> read_serial_impl(serial_handle_t h,
                                                                               std::uint8_t* buf,
                                                                               std::size_t len) noexcept {
    DWORD got = 0;
    if (!::ReadFile(h, buf, static_cast<DWORD>(len), &got, nullptr)) {
        return std::unexpected("ReadFile failed");
    }
    return static_cast<std::size_t>(got);  // 0 == the configured ReadTotalTimeoutConstant elapsed
}

#else  // POSIX

[[nodiscard]] inline std::expected<speed_t, std::string> baud_to_speed(std::uint32_t baud) noexcept {
    // Deliberately NOT a silent fallback to some default baud on an unrecognized value — a wrong baud
    // rate on a real serial link is a silent-garbage-data bug, not a "close enough" degradation.
    switch (baud) {
        case 1200: return B1200;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return std::unexpected("unsupported baud rate: " + std::to_string(baud));
    }
}

[[nodiscard]] inline std::expected<serial_handle_t, std::string> open_serial_impl(
    std::string_view port, const SerialConfig& cfg) noexcept {
    const auto speed = baud_to_speed(cfg.baud_rate);
    if (!speed) return std::unexpected(speed.error());

    const std::string path(port);
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) return std::unexpected("open() failed for " + path);

    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        return std::unexpected("tcgetattr failed for " + path);
    }
    ::cfmakeraw(&tio);  // 8N1 raw mode baseline — parity/stop bits below override N/1 as configured
    ::cfsetispeed(&tio, *speed);
    ::cfsetospeed(&tio, *speed);

    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= static_cast<tcflag_t>(~CSIZE);
    tio.c_cflag |= CS8;  // 8 data bits — see struct banner, not a knob
    if (cfg.parity == 'N') {
        tio.c_cflag &= static_cast<tcflag_t>(~PARENB);
    } else {
        tio.c_cflag |= PARENB;
        if (cfg.parity == 'O') tio.c_cflag |= PARODD;
        else tio.c_cflag &= static_cast<tcflag_t>(~PARODD);
    }
    if (cfg.stop_bits == 2) tio.c_cflag |= CSTOPB;
    else tio.c_cflag &= static_cast<tcflag_t>(~CSTOPB);

    // VMIN=0, VTIME=N (deciseconds): read() returns as soon as ANY byte is available, or after N*100ms
    // with zero bytes — the standard POSIX idiom matching this header's "0 == timeout" read contract.
    // VTIME is a single byte (0-255); clamp rather than silently wrap a longer configured timeout.
    const int deciseconds = cfg.read_timeout_ms / 100;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = static_cast<cc_t>(deciseconds < 1 ? 1 : (deciseconds > 255 ? 255 : deciseconds));

    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        ::close(fd);
        return std::unexpected("tcsetattr failed for " + path);
    }
    ::tcflush(fd, TCIOFLUSH);
    return fd;
}

inline void close_serial_impl(serial_handle_t h) noexcept {
    if (h != invalid_serial_handle) ::close(h);
}

[[nodiscard]] inline std::expected<std::size_t, std::string> write_serial_impl(serial_handle_t h,
                                                                                const std::uint8_t* buf,
                                                                                std::size_t len) noexcept {
    const ssize_t n = ::write(h, buf, len);
    if (n < 0) return std::unexpected("write() failed, errno=" + std::to_string(errno));
    return static_cast<std::size_t>(n);
}

[[nodiscard]] inline std::expected<std::size_t, std::string> read_serial_impl(serial_handle_t h,
                                                                               std::uint8_t* buf,
                                                                               std::size_t len) noexcept {
    const ssize_t n = ::read(h, buf, len);
    if (n < 0) return std::unexpected("read() failed, errno=" + std::to_string(errno));
    return static_cast<std::size_t>(n);  // 0 == VTIME elapsed with nothing available
}

#endif
}  // namespace detail

[[nodiscard]] inline std::expected<serial_handle_t, std::string> open_serial(
    std::string_view port, const SerialConfig& cfg) noexcept {
    return detail::open_serial_impl(port, cfg);
}

inline void close_serial(serial_handle_t h) noexcept { detail::close_serial_impl(h); }

[[nodiscard]] inline std::expected<std::size_t, std::string> write_serial(serial_handle_t h,
                                                                           const std::uint8_t* buf,
                                                                           std::size_t len) noexcept {
    return detail::write_serial_impl(h, buf, len);
}

// Blocks up to `SerialConfig::read_timeout_ms` (configured at open_serial() time). Returns the number of
// bytes actually read — 0 means the timeout elapsed with nothing available, NOT an error (see file
// banner); only a genuine device I/O failure is `unexpected`.
[[nodiscard]] inline std::expected<std::size_t, std::string> read_serial(serial_handle_t h,
                                                                          std::uint8_t* buf,
                                                                          std::size_t len) noexcept {
    return detail::read_serial_impl(h, buf, len);
}

}  // namespace aero::pal
