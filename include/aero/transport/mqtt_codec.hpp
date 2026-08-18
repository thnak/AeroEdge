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
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

// ===== MQTT 5 Properties (017 M7) =======================================================================
// Bounded, skip-unknown property codec — the same tier as the byte helpers above (put_str/
// put_remaining_length), shared by NativeBroker's v5 CONNECT/Will/PUBLISH/SUBSCRIBE parsing (017 N3
// precedent: one codec, not a copy per caller). v1 (M7) surfaced exactly ONE property (Session Expiry
// Interval); M7.1 added a second (Topic Alias, for inbound PUBLISH alias resolution); M7.2 PR A adds two
// more (Message Expiry Interval, Maximum Packet Size) plus an outbound PropertyWriter (below) so
// NativeBroker::publish_to() can finally write a real v5 Properties block — everything else in the type
// table below exists only so its bytes can be correctly SKIPPED (a receiver must know a property's WIRE
// TYPE to know its length even for a property it never acts on; guessing wrong silently corrupts every
// field that follows it).

// Reads an MQTT Variable Byte Integer (§1.5.5) starting at `body[pos]`, advancing `pos` past it on
// success. IDENTICAL encoding to the fixed header's Remaining Length (put_remaining_length above is the
// write-side counterpart) — 1-4 bytes, 7 bits of value per byte, high bit = continuation. `nullopt` on a
// malformed encoding (more than 4 continuation bytes) or running past `body.size()`.
inline std::optional<std::uint32_t> read_varint(const std::vector<std::byte>& body, std::size_t& pos) {
    std::uint32_t mult = 1, value = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= body.size()) return std::nullopt;
        const std::uint8_t e = std::to_integer<std::uint8_t>(body[pos]);
        ++pos;
        value += static_cast<std::uint32_t>(e & 0x7F) * mult;
        if ((e & 0x80) == 0) return value;
        mult *= 128;
    }
    return std::nullopt;  // 5th continuation byte — malformed (spec caps this at 4 bytes)
}

// MQTT 5 property wire types (§2.2.2) — only the shapes actually needed to skip a property's value.
enum class PropWireType : std::uint8_t {
    Byte,
    TwoByteInt,
    FourByteInt,
    VarInt,
    Binary,
    Utf8String,
    Utf8StringPair,
};

// Property-ID -> wire-type lookup, covering every ID that can legally appear in the four packet-property
// sets NativeBroker parses: CONNECT, CONNECT Will, PUBLISH, SUBSCRIBE (MQTT 5 §2.2.2, cross-checked
// against §3.1.2.11/§3.1.3.2/§3.3.2.3/§3.8.2.1). An ID outside this table is one this codec has never
// seen documented for these four packet types — read_properties() below treats that as unrecoverable
// (it has no way to know how many bytes to skip) rather than guessing.
inline std::optional<PropWireType> property_wire_type(std::uint32_t id) noexcept {
    switch (id) {
        case 0x01: return PropWireType::Byte;            // Payload Format Indicator (PUBLISH/Will)
        case 0x02: return PropWireType::FourByteInt;      // Message Expiry Interval (PUBLISH/Will)
        case 0x03: return PropWireType::Utf8String;       // Content Type (PUBLISH/Will)
        case 0x08: return PropWireType::Utf8String;       // Response Topic (PUBLISH/Will)
        case 0x09: return PropWireType::Binary;            // Correlation Data (PUBLISH/Will)
        case 0x0B: return PropWireType::VarInt;             // Subscription Identifier (PUBLISH/SUBSCRIBE)
        case 0x11: return PropWireType::FourByteInt;      // Session Expiry Interval (CONNECT) — the one
                                                            // property this codec surfaces, see below
        case 0x15: return PropWireType::Utf8String;       // Authentication Method (CONNECT)
        case 0x16: return PropWireType::Binary;             // Authentication Data (CONNECT)
        case 0x17: return PropWireType::Byte;             // Request Problem Information (CONNECT)
        case 0x18: return PropWireType::FourByteInt;      // Will Delay Interval (Will)
        case 0x19: return PropWireType::Byte;             // Request Response Information (CONNECT)
        case 0x21: return PropWireType::TwoByteInt;       // Receive Maximum (CONNECT)
        case 0x22: return PropWireType::TwoByteInt;       // Topic Alias Maximum (CONNECT)
        case 0x23: return PropWireType::TwoByteInt;       // Topic Alias (PUBLISH)
        case 0x26: return PropWireType::Utf8StringPair;   // User Property (CONNECT/Will/PUBLISH/SUBSCRIBE,
                                                            // repeatable)
        case 0x27: return PropWireType::FourByteInt;      // Maximum Packet Size (CONNECT)
        default: return std::nullopt;
    }
}

// The properties this codec's callers currently need out of a Properties block: Session Expiry Interval
// (017 M7.1 — TTL enforcement lives in NativeBroker, this codec just surfaces the parsed value), Topic
// Alias (017 M7.1 — inbound PUBLISH alias resolution, also NativeBroker-side), Message Expiry Interval +
// Maximum Packet Size (017 M7.2 PR A — PUBLISH TTL and per-session outbound size cap, both enforced in
// NativeBroker), and, as of 017 M7.2 PR B, Response Topic + Correlation Data + User Properties (the MQTT 5
// request/response pattern, §3.3.2.3.5-§3.3.2.3.7 — semantic validation such as Response Topic's
// no-wildcards rule lives in NativeBroker, this codec only stores the raw values). Every other recognized
// property is recognized only so it can be skipped (never stored anywhere).
struct ParsedProperties {
    std::optional<std::uint32_t> session_expiry_interval;
    std::optional<std::uint16_t> topic_alias;
    std::optional<std::uint32_t> message_expiry_interval;   // 017 M7.2 PR A — 0x02
    std::optional<std::uint32_t> maximum_packet_size;        // 017 M7.2 PR A — 0x27
    std::optional<std::string> response_topic;                // 017 M7.2 PR B — 0x08
    std::optional<std::vector<std::byte>> correlation_data;    // 017 M7.2 PR B — 0x09
    std::vector<std::pair<std::string, std::string>> user_properties;  // 017 M7.2 PR B — 0x26, repeatable
};

// Reads a Property Length varint, then walks exactly that many bytes as a sequence of
// `{property_id (varint), value}` TLV records, using property_wire_type() to know each value's length.
// Stores session_expiry_interval (0x11), message_expiry_interval (0x02), maximum_packet_size (0x27),
// response_topic (0x08), correlation_data (0x09), and user_properties (0x26, repeatable — every
// occurrence is appended, not overwritten: duplicate keys with different values are legal per
// §3.3.2.3.7 and a subscriber needs to see every instance) when their IDs appear; every other recognized
// ID is just skipped past.
// `nullopt` on: a malformed Property Length varint, a truncated record, an ID not in the type table
// above (this codec has no way to know its length, so it fails closed rather than guessing), or a
// records total that doesn't exactly fill the declared Property Length. Advances `pos` to just past the
// whole Properties block on success.
inline std::optional<ParsedProperties> read_properties(const std::vector<std::byte>& body, std::size_t& pos) {
    const auto prop_len = read_varint(body, pos);
    if (!prop_len) return std::nullopt;
    if (pos + *prop_len > body.size()) return std::nullopt;
    const std::size_t end = pos + *prop_len;

    ParsedProperties result;
    while (pos < end) {
        const auto id = read_varint(body, pos);
        if (!id || pos > end) return std::nullopt;
        const auto wire_type = property_wire_type(*id);
        if (!wire_type) return std::nullopt;  // unrecognized ID — length unknown, fail closed

        switch (*wire_type) {
            case PropWireType::Byte:
                if (pos + 1 > end) return std::nullopt;
                pos += 1;
                break;
            case PropWireType::TwoByteInt: {
                if (pos + 2 > end) return std::nullopt;
                if (*id == 0x23) {  // Topic Alias
                    const std::uint16_t v = static_cast<std::uint16_t>(
                        (std::to_integer<std::uint8_t>(body[pos]) << 8) |
                        std::to_integer<std::uint8_t>(body[pos + 1]));
                    result.topic_alias = v;
                }
                pos += 2;
                break;
            }
            case PropWireType::FourByteInt: {
                if (pos + 4 > end) return std::nullopt;
                if (*id == 0x11 || *id == 0x02 || *id == 0x27) {
                    std::uint32_t v = 0;
                    for (std::size_t i = 0; i < 4; ++i)
                        v = (v << 8) | std::to_integer<std::uint8_t>(body[pos + i]);
                    if (*id == 0x11) {         // Session Expiry Interval
                        result.session_expiry_interval = v;
                    } else if (*id == 0x02) {  // Message Expiry Interval (017 M7.2 PR A)
                        result.message_expiry_interval = v;
                    } else {                    // 0x27 — Maximum Packet Size (017 M7.2 PR A)
                        result.maximum_packet_size = v;
                    }
                }
                pos += 4;
                break;
            }
            case PropWireType::VarInt: {
                const auto v = read_varint(body, pos);
                if (!v || pos > end) return std::nullopt;
                break;
            }
            case PropWireType::Binary:
            case PropWireType::Utf8String: {
                if (pos + 2 > end) return std::nullopt;
                const std::uint16_t len = static_cast<std::uint16_t>(
                    (std::to_integer<std::uint8_t>(body[pos]) << 8) | std::to_integer<std::uint8_t>(body[pos + 1]));
                pos += 2;
                if (pos + len > end) return std::nullopt;
                if (*id == 0x08) {  // Response Topic (017 M7.2 PR B)
                    result.response_topic = std::string(
                        reinterpret_cast<const char*>(body.data() + pos), len);
                } else if (*id == 0x09) {  // Correlation Data (017 M7.2 PR B)
                    result.correlation_data.emplace(body.begin() + static_cast<std::ptrdiff_t>(pos),
                                                     body.begin() + static_cast<std::ptrdiff_t>(pos + len));
                }
                pos += len;
                break;
            }
            case PropWireType::Utf8StringPair: {
                std::string key, value;
                for (int k = 0; k < 2; ++k) {
                    if (pos + 2 > end) return std::nullopt;
                    const std::uint16_t len = static_cast<std::uint16_t>(
                        (std::to_integer<std::uint8_t>(body[pos]) << 8) |
                        std::to_integer<std::uint8_t>(body[pos + 1]));
                    pos += 2;
                    if (pos + len > end) return std::nullopt;
                    if (*id == 0x26) {  // User Property (017 M7.2 PR B, repeatable)
                        (k == 0 ? key : value)
                            .assign(reinterpret_cast<const char*>(body.data() + pos), len);
                    }
                    pos += len;
                }
                if (*id == 0x26) {
                    // emplace_back, not assignment: User Property is explicitly repeatable
                    // (§3.3.2.3.7) — duplicate keys with different values are legal and must all
                    // survive so a subscriber sees every instance.
                    result.user_properties.emplace_back(std::move(key), std::move(value));
                }
                break;
            }
        }
    }
    if (pos != end) return std::nullopt;  // records didn't exactly fill the declared Property Length
    return result;
}

// The only properties-WRITING shape v1/M7.1 needed: every v5 ack NativeBroker sent through M7.1 (CONNACK/
// SUBACK/DISCONNECT) carries zero properties (3.4.2.1-style "Success and nothing to say" is legal to
// encode as a bare zero-length Property Length). Still used by CONNACK/SUBACK/DISCONNECT — unchanged.
inline void put_empty_properties(std::vector<std::byte>& out) { put_remaining_length(out, 0); }

// The one non-empty outbound property v1 needs: Topic Alias Maximum (0x22) in CONNACK, so a v5 client
// knows the broker accepts inbound aliases (absent/0 => client MUST NOT send any, MQTT 5 §3.1.2.11.2).
inline void put_topic_alias_max_properties(std::vector<std::byte>& out, std::uint16_t max) {
    std::vector<std::byte> records{std::byte{0x22}};
    put_u16_be(records, max);
    put_remaining_length(out, static_cast<std::uint32_t>(records.size()));
    out.insert(out.end(), records.begin(), records.end());
}

// A general outbound Properties writer — previously deferred (see this file's now-stale prior banner
// above put_empty_properties, which used to say "nothing calls for one yet") because nothing needed to
// combine more than one fixed property per packet. 017 M7.2's PUBLISH properties (Message Expiry from
// PR A; Response Topic/Correlation Data/User Properties from PR B) are each independently optional and
// must combine onto ONE Properties block on NativeBroker::publish_to() — that needs a builder, not more
// fixed-shape put_..._properties() functions. put_u32/put_str were exercised starting with PR A (Message
// Expiry); put_binary/put_str_pair, added in the same pass anyway to avoid a half-built builder, are now
// exercised too, by PR B.
class PropertyWriter {
public:
    void put_u32(std::uint8_t id, std::uint32_t v) {
        records_.push_back(static_cast<std::byte>(id));
        put_u32_be(v);
    }
    void put_str(std::uint8_t id, std::string_view v) {
        records_.push_back(static_cast<std::byte>(id));
        aero::transport::mqtt::put_str(records_, v);  // qualified: the free function, not this member
    }
    void put_binary(std::uint8_t id, std::span<const std::byte> v) {
        records_.push_back(static_cast<std::byte>(id));
        put_u16_be(records_, static_cast<std::uint16_t>(v.size()));
        records_.insert(records_.end(), v.begin(), v.end());
    }
    void put_str_pair(std::uint8_t id, std::string_view k, std::string_view v) {
        records_.push_back(static_cast<std::byte>(id));
        aero::transport::mqtt::put_str(records_, k);
        aero::transport::mqtt::put_str(records_, v);
    }
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
    // Writes a valid Properties block to `out` — a Property Length varint (0 when nothing was added:
    // MQTT 5 §2.2.2.2 makes an empty Properties block legal, exactly the "at least the mandatory
    // zero-length field" fix 017 M7.2 PR A applies to publish_to(), see native_broker.hpp) followed by
    // every record added via put_u32/put_str/put_binary/put_str_pair above, in call order.
    void write(std::vector<std::byte>& out) const {
        put_remaining_length(out, static_cast<std::uint32_t>(records_.size()));
        out.insert(out.end(), records_.begin(), records_.end());
    }

private:
    void put_u32_be(std::uint32_t v) {
        for (int i = 3; i >= 0; --i) records_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
    std::vector<std::byte> records_;
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

// 017 Phase 3 addition: the buffered-read counterpart to read_packet() above — ADDITIVE, does not
// replace it. read_packet()/read_n() do one recv_some()-or-poll cycle PER BYTE of the fixed header and
// remaining-length varint, plus one more for the body — 3+ syscalls per packet, unconditionally
// (measured as the dominant per-packet cost even with zero fan-out, see
// 017-Native-Broker-Performance-Redesign.md §2.4 Experiment A). This function does none of that I/O
// itself: it is a PURE function over a caller-owned buffer, meant to be driven by a caller that fills
// `buf` via its own bulk recv_some() calls (one recv_some() can hand over many packets' worth of bytes
// at once) and repeatedly calls this to carve complete packets out of whatever has accumulated so far.
// `MqttClientTransport`, bridge.hpp, and every test file's hand-rolled client keep using
// read_packet()/read_n() completely unchanged — this is additive, not a replacement (017 N3 precedent:
// don't touch shared code for one caller's needs).
//
// Tries to carve exactly one Packet out of buf[pos, buf.size()). On success: the Packet is returned and
// `pos` is advanced past it (ready for the next call). On Incomplete: `pos` is left UNCHANGED — this is
// not an error, it means "not enough bytes buffered yet for a whole packet"; the caller should recv_some()
// more bytes, append them, and retry. On Malformed: `pos` is left UNCHANGED and the caller must treat
// this exactly like read_packet() returning nullopt today — a fatal framing error, close the connection.
// The only Malformed case is a remaining-length varint exceeding MQTT's own 4-byte encoding cap (§1.5.5,
// the same limit read_varint()/put_remaining_length() enforce elsewhere in this file) — read_packet()'s
// own remaining-length loop above does not actually check for this (it silently stops after 4 bytes
// regardless of whether the 4th byte's continuation bit is still set); this function closes that gap
// rather than reproducing it, since a well-formed sender can never trigger it and doing so is a strict
// improvement, not a behavior change any real caller depends on.
enum class ParseStatus { Incomplete, Malformed };

inline std::expected<Packet, ParseStatus> try_parse_packet(const std::vector<std::byte>& buf,
                                                            std::size_t& pos) {
    std::size_t p = pos;
    if (p >= buf.size()) return std::unexpected(ParseStatus::Incomplete);
    const std::byte b0 = buf[p];
    ++p;

    std::uint32_t mult = 1, len = 0;
    bool have_length = false;
    for (int i = 0; i < 4 && !have_length; ++i) {
        if (p >= buf.size()) return std::unexpected(ParseStatus::Incomplete);
        const std::uint8_t e = std::to_integer<std::uint8_t>(buf[p]);
        ++p;
        len += static_cast<std::uint32_t>(e & 0x7F) * mult;
        if ((e & 0x80) == 0) {
            have_length = true;
        } else {
            mult *= 128;
        }
    }
    if (!have_length) return std::unexpected(ParseStatus::Malformed);  // 5th continuation byte

    if (buf.size() - p < len) return std::unexpected(ParseStatus::Incomplete);  // body not fully buffered

    Packet pkt;
    pkt.type_flags = std::to_integer<std::uint8_t>(b0);
    pkt.body.assign(buf.begin() + static_cast<std::ptrdiff_t>(p),
                    buf.begin() + static_cast<std::ptrdiff_t>(p + len));
    pos = p + len;
    return pkt;
}

// Serializes [fixed-header-byte | remaining-length | body] into a fresh buffer — the exact framing
// write_packet()/write_packet_bounded() below send as-is. Extracted (017 Phase 7) so a caller building a
// packet for later/async transmission (e.g. NativeBroker's reactor outbound queue, which must frame a
// packet once and then track a partial-send byte offset across multiple non-blocking send attempts) can
// reuse the exact same framing logic instead of re-deriving it.
inline std::vector<std::byte> serialize_packet(std::byte type_flags, const std::vector<std::byte>& body) {
    std::vector<std::byte> pkt;
    pkt.reserve(5 + body.size());
    pkt.push_back(type_flags);
    put_remaining_length(pkt, static_cast<std::uint32_t>(body.size()));
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

// Serialize [fixed-header-byte | remaining-length | body] and write it fully to `ch`, retrying on
// would_block. NOT channel-write-serializing by itself — a caller with multiple writer threads on the
// same channel (e.g. a broker Session fanning out a PUBLISH concurrently with its own reader thread
// writing a SUBACK) must hold its own mutex around this call.
template <class Channel>
inline bool write_packet(Channel& ch, std::byte type_flags, const std::vector<std::byte>& body) {
    std::vector<std::byte> pkt = serialize_packet(type_flags, body);
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

// 017 Phase 7 (Critical fix #2): same as write_packet() but gives up (returns false) once `deadline`
// passes, instead of retrying forever like write_packet()'s own unbounded loop. write_packet() itself is
// BYTE-FOR-BYTE UNCHANGED for every existing caller — this is a new, additive overload, not a
// modification. Used by NativeBroker's reactor->legacy-recipient hand-off pool so one persistently slow
// legacy (TLS) recipient can't monopolize a hand-off worker forever (the exact failure mode Phase 6's
// item-count cap failed to prevent — bounding wall-clock time directly, not a proxy for it).
template <class Channel>
inline bool write_packet_bounded(Channel& ch, std::byte type_flags, const std::vector<std::byte>& body,
                                  std::chrono::steady_clock::time_point deadline) {
    std::vector<std::byte> pkt = serialize_packet(type_flags, body);
    std::size_t sent = 0;
    while (sent < pkt.size()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
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

}  // namespace aero::transport::mqtt
