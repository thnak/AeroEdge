// AeroEdge PAL — single-fd readiness wait with a timeout.
//
// QuarkCpp's own PAL (pal/net.hpp, reachable here because aero-transport links quark::quark) already
// gives every socket primitive AeroEdge's transport adapters need (tcp_listen/tcp_connect/accept_one/
// recv_some/send_some/...), but it is deliberately reactor-only (019 §"Completion (proactor) I/O
// model") and exposes no raw poll() — readiness only flows through IoContext callbacks. AeroEdge's
// transport adapters (tcp/mqtt/grpc) are blocking-thread-per-connection designs, not reactor-driven,
// so they need exactly this one missing primitive: "is this fd ready (or has `timeout_ms` elapsed)?"
//
// Thin shim over ::poll() (POSIX) / ::WSAPoll() (Windows) — near-identical signatures on both.
#pragma once

#include "pal/net.hpp"  // quark::pal::fd_t, quark::pal::last_error()

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <poll.h>
#endif

#include <expected>
#include <system_error>

namespace aero::pal {

namespace detail {
#if defined(_WIN32)
[[nodiscard]] inline int poll_one(quark::pal::fd_t fd, short events, int timeout_ms) noexcept {
    WSAPOLLFD p{};
    p.fd = fd;
    p.events = events;
    return ::WSAPoll(&p, 1, timeout_ms);
}
#else
[[nodiscard]] inline int poll_one(quark::pal::fd_t fd, short events, int timeout_ms) noexcept {
    ::pollfd p{};
    p.fd = fd;
    p.events = events;
    return ::poll(&p, 1, timeout_ms);
}
#endif
}  // namespace detail

// true = ready before the timeout; false = timed out; unexpected = the poll/WSAPoll call itself failed.
[[nodiscard]] inline std::expected<bool, std::error_code> wait_readable(quark::pal::fd_t fd,
                                                                        int timeout_ms) noexcept {
    const int r = detail::poll_one(fd, POLLIN, timeout_ms);
    if (r < 0) return std::unexpected(quark::pal::last_error());
    return r > 0;
}

[[nodiscard]] inline std::expected<bool, std::error_code> wait_writable(quark::pal::fd_t fd,
                                                                        int timeout_ms) noexcept {
    const int r = detail::poll_one(fd, POLLOUT, timeout_ms);
    if (r < 0) return std::unexpected(quark::pal::last_error());
    return r > 0;
}

}  // namespace aero::pal
