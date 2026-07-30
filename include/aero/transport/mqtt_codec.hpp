// AeroEdge Transport — the MQTT 3.1.1 wire codec shared by every MQTT wire-protocol speaker in this
// tree: `MqttClientTransport` (014 §5, a client to an external broker) and `aero::broker::NativeBroker`
// (017, the native broker's server-side sessions). Fixed-header + remaining-length + variable-header
// framing is identical on both sides of a connection — factoring it out once means a codec fix applies
// to both instead of drifting (017 §9 N3).
//
// Deliberately free functions over an explicit `fd` + `running` flag, not a class: both callers already
// own their own fd/threading model (MqttClientTransport's reader thread + io_mu_; NativeBroker's
// per-connection Session) and just need the byte-level framing, not a second copy of that machinery.
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "pal/net.hpp"  // quark::pal::* — fd_t, recv_some/send_some/would_block

#include "aero/pal/poll.hpp"

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

// Read exactly n bytes off `fd`, polling with a 200ms timeout so the caller notices `running` flip to
// false promptly instead of blocking forever on a stalled peer.
inline bool read_n(quark::pal::fd_t fd, std::byte* buf, std::size_t n,
                   const std::atomic<bool>& running) {
    std::size_t got = 0;
    while (got < n) {
        if (!running.load(std::memory_order_acquire)) return false;
        const auto ready = aero::pal::wait_readable(fd, 200);
        if (!ready) return false;   // poll failed
        if (!*ready) continue;      // timeout → re-check running
        auto r = quark::pal::recv_some(fd, buf + got, n - got);
        if (!r) {
            if (r.error() == quark::pal::would_block()) continue;  // spurious wake
            return false;
        }
        if (*r == 0) return false;  // peer closed
        got += *r;
    }
    return true;
}

inline std::optional<Packet> read_packet(quark::pal::fd_t fd, const std::atomic<bool>& running) {
    std::byte b0;
    if (!read_n(fd, &b0, 1, running)) return std::nullopt;
    std::uint32_t mult = 1, len = 0;
    for (int i = 0; i < 4; ++i) {
        std::byte enc;
        if (!read_n(fd, &enc, 1, running)) return std::nullopt;
        const std::uint8_t e = std::to_integer<std::uint8_t>(enc);
        len += (e & 0x7F) * mult;
        if ((e & 0x80) == 0) break;
        mult *= 128;
    }
    Packet p;
    p.type_flags = std::to_integer<std::uint8_t>(b0);
    p.body.resize(len);
    if (len > 0 && !read_n(fd, p.body.data(), len, running)) return std::nullopt;
    return p;
}

// Serialize [fixed-header-byte | remaining-length | body] and write it fully to `fd`, retrying on
// would_block. NOT fd-write-serializing by itself — a caller with multiple writer threads on the same
// fd (e.g. a broker Session fanning out a PUBLISH concurrently with its own reader thread writing a
// SUBACK) must hold its own mutex around this call.
inline bool write_packet(quark::pal::fd_t fd, std::byte type_flags, const std::vector<std::byte>& body) {
    std::vector<std::byte> pkt;
    pkt.reserve(5 + body.size());
    pkt.push_back(type_flags);
    put_remaining_length(pkt, static_cast<std::uint32_t>(body.size()));
    pkt.insert(pkt.end(), body.begin(), body.end());
    std::size_t sent = 0;
    while (sent < pkt.size()) {
        auto w = quark::pal::send_some(fd, pkt.data() + sent, pkt.size() - sent);
        if (w) {
            sent += *w;
            continue;
        }
        if (w.error() != quark::pal::would_block()) return false;
        if (!aero::pal::wait_writable(fd, 200)) return false;  // poll itself failed
    }
    return true;
}

}  // namespace aero::transport::mqtt
