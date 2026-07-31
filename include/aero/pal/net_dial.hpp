// AeroEdge PAL — `dial_tcp`, the shared "dial out to a remote host:port" idiom (non-blocking connect +
// poll-with-timeout). Factored out of `MqttClientTransport::dial()` (transport/mqtt_client_transport.hpp)
// and `MqttBridgeSink::dial()` (broker/bridge.hpp), which each carried this exact ~25-line body
// duplicated verbatim (both banners cross-reference the other as "the same shape" — this header is that
// shape, named once). Behavior-preserving: same getaddrinfo → try-each-candidate → non-blocking connect
// → poll-writable(timeout) → connect_result() shape, same "first candidate that connects wins" retry.
//
// ERROR IDIOM: `std::expected<fd_t, std::string>`, matching this codebase's other PAL-adjacent headers
// (aero/pal/tls.hpp's `TlsServerContext::create`/`accept` — "std::expected, not exceptions, for expected
// failure paths", CONVENTIONS.md) rather than the bare invalid_fd-on-failure the two duplicated dial()s
// used — a caller that wants the old bool-ish shape can just check `.has_value()`, but now gets a
// diagnostic string for free (both original call sites folded that reason into their own gate_err()/
// false-return anyway, just discarding WHY internally).
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "pal/net.hpp"  // quark::pal::* — fd_t, tcp_connect/close_fd/connect_result/invalid_fd

#if !defined(_WIN32)
#include <netdb.h>  // getaddrinfo/freeaddrinfo (Windows: transitively via pal/net.hpp's ws2tcpip.h)
#endif

#include "aero/pal/poll.hpp"  // aero::pal::wait_writable

namespace aero::pal {

// Non-blocking dial-out to `host:port`, bounded by `timeout_ms` PER CANDIDATE address (mirrors the two
// call sites this replaces — each candidate from getaddrinfo gets its own full timeout budget, not a
// shared one). IPv4 only (both original call sites hard-coded AF_INET; preserved here, not broadened).
[[nodiscard]] inline std::expected<quark::pal::fd_t, std::string> dial_tcp(std::string_view host,
                                                                            std::uint16_t port,
                                                                            int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string host_s(host);
    if (::getaddrinfo(host_s.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return std::unexpected("getaddrinfo failed for '" + host_s + "'");

    quark::pal::fd_t fd = quark::pal::invalid_fd;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
        const std::uint64_t addr_u64 = ::ntohl(sin->sin_addr.s_addr);
        auto attempt = quark::pal::tcp_connect(addr_u64, port);
        if (!attempt) continue;
        fd = *attempt;
        const auto writable = aero::pal::wait_writable(fd, timeout_ms);
        if (writable && *writable && quark::pal::connect_result(fd)) break;  // connected
        quark::pal::close_fd(fd);
        fd = quark::pal::invalid_fd;
    }
    ::freeaddrinfo(res);

    if (fd == quark::pal::invalid_fd)
        return std::unexpected("cannot dial " + host_s + ":" + std::to_string(port));
    return fd;
}

}  // namespace aero::pal
