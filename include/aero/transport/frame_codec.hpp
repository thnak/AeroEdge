// AeroEdge Transport — the on-the-wire codec for `quark::MessageFrame` (014 §3, feeds the real backends).
//
// WHY THIS EXISTS: Quark's `MessageFrame` (quark/core/transport.hpp) is the byte-moving unit the
// `Transport` seam carries. The in-process `LoopbackTransport` moves it by pointer, so it never needs a
// codec. A REAL transport (TCP/MQTT/gRPC) puts the frame on a socket and must (de)serialize it losslessly
// — every field the receiving node needs to find the target actor, decode the payload, and re-establish
// the propagated deadline/trace/principal (014 §3 C4 "header fidelity"). This header is that codec and
// nothing else: a stable, self-describing, little-endian record + length-prefixed payload. It is shared
// by all three real adapters so the wire format is defined in exactly one place.
//
// WIRE LAYOUT (all integers little-endian; the frame body, WITHOUT any transport framing):
//   u64 from | u64 to | u64 target.type | u64 target.key | u64 msg_type
//   u8 mode  | u8 kind | i64 deadline_ns | u64 trace_id
//   u64 principal.subject | u64 principal.rights
//   u32 payload_len | payload_len bytes
// Fixed header = 8*9 + 1 + 1 + 4 = 78 bytes, then the payload. A `decode` is fully bounds-checked and
// returns nullopt on any truncation/overflow — a hostile or corrupt peer can never read out of bounds
// (R5 honesty: a real backend must fail closed, never fake a frame). The format is transport-agnostic:
// TCP prefixes it with a u32 length, MQTT puts it after an 8-byte seq in the PUBLISH payload, gRPC wraps
// it in the 5-byte gRPC length-delimited message — the body bytes here are identical in all three.
//
// OFF THE HOT PATH (R0/layering): this runs only on the socket edge of an optional transport adapter,
// never on the flow steady path — ordinary std::vector, no 0-alloc discipline owed (contrast INode).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "quark/core/ids.hpp"        // NodeId, ActorId, TypeKey
#include "quark/core/principal.hpp"  // Principal
#include "quark/core/transport.hpp"  // MessageFrame, FrameKind
#include "quark/core/wire.hpp"       // WireMode

namespace aero::transport {

using quark::MessageFrame;

// The fixed portion preceding the variable payload (see WIRE LAYOUT above). A decode needs at least this
// many bytes before it may read the payload length.
inline constexpr std::size_t kFrameHeaderBytes = 8 * 9 + 1 + 1 + 4;  // = 78

namespace detail {

// --- little-endian primitive appenders (host-endian-agnostic: byte math, never a reinterpret) --------
inline void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
inline void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
inline void put_u8(std::vector<std::byte>& out, std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

// --- bounds-checked little-endian readers over a cursor into a byte span ------------------------------
struct Reader {
    std::span<const std::byte> buf;
    std::size_t pos = 0;

    [[nodiscard]] bool need(std::size_t n) const noexcept { return pos + n <= buf.size(); }

    [[nodiscard]] std::uint64_t u64() noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buf[pos + i])) << (8 * i);
        pos += 8;
        return v;
    }
    [[nodiscard]] std::uint32_t u32() noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buf[pos + i])) << (8 * i);
        pos += 4;
        return v;
    }
    [[nodiscard]] std::uint8_t u8() noexcept { return std::to_integer<std::uint8_t>(buf[pos++]); }
};

}  // namespace detail

// Serialize a MessageFrame to a self-contained byte record (no transport framing — see header note).
[[nodiscard]] inline std::vector<std::byte> encode_frame(const MessageFrame& f) {
    std::vector<std::byte> out;
    out.reserve(kFrameHeaderBytes + f.payload.size());
    detail::put_u64(out, f.from.value);
    detail::put_u64(out, f.to.value);
    detail::put_u64(out, f.target.type.value);
    detail::put_u64(out, f.target.key);
    detail::put_u64(out, f.msg_type.value);
    detail::put_u8(out, static_cast<std::uint8_t>(f.mode));
    detail::put_u8(out, static_cast<std::uint8_t>(f.kind));
    detail::put_u64(out, static_cast<std::uint64_t>(f.deadline_ns));  // i64 bit-pattern via u64
    detail::put_u64(out, f.trace_id);
    detail::put_u64(out, f.principal.subject);
    detail::put_u64(out, f.principal.rights);
    detail::put_u32(out, static_cast<std::uint32_t>(f.payload.size()));
    out.insert(out.end(), f.payload.begin(), f.payload.end());
    return out;
}

// Deserialize a frame body produced by encode_frame. Fully bounds-checked: returns nullopt if `bytes` is
// truncated or the declared payload length runs past the buffer (fail-closed, never a partial frame).
[[nodiscard]] inline std::optional<MessageFrame> decode_frame(std::span<const std::byte> bytes) {
    detail::Reader r{bytes};
    if (!r.need(kFrameHeaderBytes)) return std::nullopt;
    MessageFrame f;
    f.from.value = r.u64();
    f.to.value = r.u64();
    f.target.type.value = r.u64();
    f.target.key = r.u64();
    f.msg_type.value = r.u64();
    f.mode = static_cast<quark::WireMode>(r.u8());
    f.kind = static_cast<quark::FrameKind>(r.u8());
    f.deadline_ns = static_cast<std::int64_t>(r.u64());
    f.trace_id = r.u64();
    f.principal.subject = r.u64();
    f.principal.rights = r.u64();
    const std::uint32_t plen = r.u32();
    if (!r.need(plen)) return std::nullopt;  // declared payload runs past the buffer → reject
    f.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(r.pos),
                     bytes.begin() + static_cast<std::ptrdiff_t>(r.pos) + static_cast<std::ptrdiff_t>(plen));
    return f;
}

}  // namespace aero::transport
