// AeroEdge Transport — the MQTT 3.1.1 wire codec shared by every MQTT wire-protocol speaker in this
// tree: `MqttClientTransport` (014 §5, a client to an external broker) and `aero::broker::NativeBroker`
// (017, the native broker's server-side sessions). Fixed-header + remaining-length + variable-header
// framing is identical on both sides of a connection — factoring it out once means a codec fix applies
// to both instead of drifting (017 §9 N3).
//
// Deliberately free functions over an explicit `fd` + `running` flag, not a class: both callers already
// own their own fd/threading model (MqttClientTransport's reader thread + io_mu_; NativeBroker's
// per-connection Session) and just need the byte-level framing, not a second copy of that machinery.
//
// CHANNEL SEAM (M5 TLS+ACL integration pass, io_channel.hpp's own banner predicted this exact change):
// `read_n`/`read_packet`/`write_packet` are templated over a `Channel` type parameter constrained to
// `aero::transport::PlainChannel`/`TlsChannel`'s shared surface (`recv_some`/`send_some`/`fd()`) so
// NativeBroker's per-connection Session can drive either a raw socket or a TLS session through the exact
// same framing code — no second codec, no virtual dispatch. Backward compatibility for every EXISTING
// caller (MqttClientTransport, NativeBroker's own pre-M5 call sites, every hand-rolled test client) is
// kept by ALSO overloading each function on a bare `quark::pal::fd_t`, which just wraps the fd in a
// `PlainChannel` and forwards to the templated version — so no existing call site anywhere in the tree
// needs to change, and the plaintext path costs nothing extra (`PlainChannel`'s methods are trivial
// one-line forwards to the exact same `quark::pal::recv_some`/`send_some` calls this file made directly
// before, and are expected to inline away under any optimizing build). Overload resolution prefers the
// non-template `fd_t` overload over instantiating the template with `Channel = fd_t` when a bare fd is
// passed (a non-template exact match beats a function-template specialization, C++ [over.match.best]),
// so there is no ambiguity and no risk of the template accidentally being instantiated with a type that
// doesn't have `recv_some`/`send_some`/`fd()`.
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "pal/net.hpp"  // quark::pal::* — fd_t, recv_some/send_some/would_block

#include "aero/pal/poll.hpp"
#include "aero/transport/io_channel.hpp"  // PlainChannel — the fd_t-overload backward-compat wrapper

namespace aero::transport::mqtt {

// ===== byte helpers (MQTT 3.1.1 wire types) ===========================================================
inline void put_u16_be(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(v & 0xFF));
}
inline void put_u64_be(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
inline void put_str(std::vector<std::byte>& out, std::string_view s) {
    put_u16_be(out, static_cast<std::uint16_t>(s.size()));
    for (char c : s) out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
}
// MQTT "remaining length": 1-4 bytes, 7 bits each, high bit = continuation.
inline void put_remaining_length(std::vector<std::byte>& out, std::uint32_t len) {
    do {
        std::uint8_t enc = len % 128;
        len /= 128;
        if (len > 0) enc |= 0x80;
        out.push_back(static_cast<std::byte>(enc));
    } while (len > 0);
}

// A parsed packet: the fixed-header byte (type + flags) and everything after the remaining-length
// field (variable header + payload) — callers interpret `body` per packet type.
struct Packet {
    std::uint8_t type_flags = 0;
    std::vector<std::byte> body;
};

// Read exactly n bytes off `ch`: TRY `recv_some` first, and only poll `ch.fd()` (200ms timeout, so the
// caller notices `running` flip to false promptly instead of blocking forever on a stalled peer) when
// that attempt reports would_block. `Channel` is any type exposing
// `recv_some(std::byte*, std::size_t) -> std::expected<std::size_t, std::error_code>` and
// `fd() -> quark::pal::fd_t` (aero::transport::PlainChannel/TlsChannel, io_channel.hpp).
//
// TRY-FIRST, NOT POLL-FIRST (load-bearing for TLS): a `TlsChannel`'s `recv_some` can hand back MORE
// plaintext than one call drains from mbedTLS's OWN internal record buffer, with ZERO further bytes
// pending at the raw socket — mbedTLS decrypts a whole TLS record from a single OS-level recv() and
// buffers whatever the caller didn't consume. Polling the raw fd BEFORE every `recv_some` call (as an
// earlier version of this function did) would then falsely see "not readable" on the next byte — nothing
// NEW has arrived over the wire — and loop forever waiting for a socket event that will never come, even
// though `recv_some` already has bytes ready to hand over with no I/O at all. Trying `recv_some` first
// and polling only on an actual would_block avoids that: it drains buffered plaintext with no wasted
// poll, and for `PlainChannel` (no such buffering layer) it is behaviorally identical to polling first —
// if anything cheaper (skips the poll syscall whenever data is already there) — so this is not a
// plaintext-path regression, see this file's banner.
template <class Channel>
inline bool read_n(Channel& ch, std::byte* buf, std::size_t n, const std::atomic<bool>& running) {
    std::size_t got = 0;
    while (got < n) {
        if (!running.load(std::memory_order_acquire)) return false;
        auto r = ch.recv_some(buf + got, n - got);
        if (r) {
            if (*r == 0) return false;  // peer closed
            got += *r;
            continue;
        }
        if (r.error() != quark::pal::would_block()) return false;
        const auto ready = aero::pal::wait_readable(ch.fd(), 200);
        if (!ready) return false;  // poll failed
        // Ready or timed out, either way loop back: re-check `running`, then retry recv_some (a timeout
        // is exactly the "give running a chance to flip" tick the 200ms poll window exists for).
    }
    return true;
}

// Backward-compat overload: a bare fd behaves exactly as it always has (wraps in a PlainChannel, zero
// added cost on the plaintext path — see this file's banner).
inline bool read_n(quark::pal::fd_t fd, std::byte* buf, std::size_t n, const std::atomic<bool>& running) {
    aero::transport::PlainChannel ch{fd};
    return read_n(ch, buf, n, running);
}

template <class Channel>
inline std::optional<Packet> read_packet(Channel& ch, const std::atomic<bool>& running) {
    std::byte b0;
    if (!read_n(ch, &b0, 1, running)) return std::nullopt;
    std::uint32_t mult = 1, len = 0;
    for (int i = 0; i < 4; ++i) {
        std::byte enc;
        if (!read_n(ch, &enc, 1, running)) return std::nullopt;
        const std::uint8_t e = std::to_integer<std::uint8_t>(enc);
        len += (e & 0x7F) * mult;
        if ((e & 0x80) == 0) break;
        mult *= 128;
    }
    Packet p;
    p.type_flags = std::to_integer<std::uint8_t>(b0);
    p.body.resize(len);
    if (len > 0 && !read_n(ch, p.body.data(), len, running)) return std::nullopt;
    return p;
}

inline std::optional<Packet> read_packet(quark::pal::fd_t fd, const std::atomic<bool>& running) {
    aero::transport::PlainChannel ch{fd};
    return read_packet(ch, running);
}

// Serialize [fixed-header-byte | remaining-length | body] and write it fully to `ch`, retrying on
// would_block. NOT channel-write-serializing by itself — a caller with multiple writer threads on the
// same channel (e.g. a broker Session fanning out a PUBLISH concurrently with its own reader thread
// writing a SUBACK) must hold its own mutex around this call.
template <class Channel>
inline bool write_packet(Channel& ch, std::byte type_flags, const std::vector<std::byte>& body) {
    std::vector<std::byte> pkt;
    pkt.reserve(5 + body.size());
    pkt.push_back(type_flags);
    put_remaining_length(pkt, static_cast<std::uint32_t>(body.size()));
    pkt.insert(pkt.end(), body.begin(), body.end());
    std::size_t sent = 0;
    while (sent < pkt.size()) {
        auto w = ch.send_some(pkt.data() + sent, pkt.size() - sent);
        if (w) {
            sent += *w;
            continue;
        }
        if (w.error() != quark::pal::would_block()) return false;
        if (!aero::pal::wait_writable(ch.fd(), 200)) return false;  // poll itself failed
    }
    return true;
}

inline bool write_packet(quark::pal::fd_t fd, std::byte type_flags, const std::vector<std::byte>& body) {
    aero::transport::PlainChannel ch{fd};
    return write_packet(ch, type_flags, body);
}

}  // namespace aero::transport::mqtt
