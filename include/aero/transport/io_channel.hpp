// AeroEdge Transport — `PlainChannel`/`TlsChannel`, a zero-overhead-on-the-plaintext-path seam letting
// byte-level codec code (today: aero/transport/mqtt_codec.hpp's read_n/read_packet/write_packet, which
// call quark::pal::recv_some/send_some directly on a raw fd_t) work identically over either a raw socket
// or a TLS session (aero/pal/tls.hpp's TlsSession), without a vtable in the hot (plaintext) path.
//
// FUTURE INTEGRATION (not done here — mqtt_codec.hpp is off-limits for this change): a later pass is
// expected to template mqtt_codec.hpp's read_n/read_packet/write_packet over a `Channel` type parameter
// constrained to this exact surface (recv_some/send_some/fd()), so NativeBroker's per-connection Session
// can hold either a PlainChannel or a TlsChannel and every codec call site stays unchanged syntactically.
// Both structs below expose an IDENTICAL surface (same method names/signatures) specifically so that
// template works over either one with no specialization needed. Do not add virtual dispatch here — the
// whole point is that PlainChannel compiles down to the exact same code as today's direct
// quark::pal::recv_some/send_some calls (no indirection at all), while TlsChannel is one non-owning
// pointer indirection to reach TlsSession's own (already non-virtual) methods.
#pragma once

#include <cstddef>
#include <expected>
#include <system_error>

#include "pal/net.hpp"  // quark::pal::* — fd_t, recv_some/send_some

#include "aero/pal/tls.hpp"

namespace aero::transport {

// Wraps a raw quark::pal::fd_t with the recv_some/send_some/fd() surface TlsChannel also exposes, so
// callers can be templated over "any channel" uniformly. Trivially cheap: this is exactly what today's
// direct quark::pal:: calls already do, just named and given a uniform shape to template over.
struct PlainChannel {
    quark::pal::fd_t fd_val = quark::pal::invalid_fd;

    [[nodiscard]] std::expected<std::size_t, std::error_code> recv_some(std::byte* buf,
                                                                         std::size_t n) const {
        return quark::pal::recv_some(fd_val, buf, n);
    }
    [[nodiscard]] std::expected<std::size_t, std::error_code> send_some(const std::byte* buf,
                                                                         std::size_t n) const {
        return quark::pal::send_some(fd_val, buf, n);
    }
    [[nodiscard]] quark::pal::fd_t fd() const noexcept { return fd_val; }
};

// Wraps a live TlsSession* (non-owning — whatever owns the TlsSession, e.g. a broker Session, must
// outlive this). Kept as a raw observer pointer rather than a reference so TlsChannel stays copyable /
// default-constructible like PlainChannel (a reference member would forbid both).
struct TlsChannel {
    aero::pal::tls::TlsSession* session = nullptr;

    [[nodiscard]] std::expected<std::size_t, std::error_code> recv_some(std::byte* buf,
                                                                         std::size_t n) const {
        return session->recv_some(buf, n);
    }
    [[nodiscard]] std::expected<std::size_t, std::error_code> send_some(const std::byte* buf,
                                                                         std::size_t n) const {
        return session->send_some(buf, n);
    }
    [[nodiscard]] quark::pal::fd_t fd() const noexcept { return session->fd(); }
};

}  // namespace aero::transport
