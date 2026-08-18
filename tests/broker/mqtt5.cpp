// AeroEdge 017 M7 gate: MQTT 5 protocol support in NativeBroker — protocol version negotiation,
// CONNECT/Will/PUBLISH/SUBSCRIBE Properties parsing, and v5 CONNACK/SUBACK reason codes. Mirrors
// tests/broker/native_broker.cpp's TestClient-over-real-socket pattern, extended with a v5-capable
// client that can emit Properties blocks (built on mqtt_codec.hpp's own put_str/put_remaining_length —
// no general properties WRITER exists in mqtt_codec.hpp itself, Task 2's scope is a decoder only, see
// its own comment) and parse the v5-shaped CONNACK/SUBACK bodies.
//
// Proves, over real sockets:
//   (1) a v5 CONNECT (protocol level 0x05) with a small CONNECT Properties block (Session Expiry
//       Interval) gets a v5-shaped CONNACK back: {ack_flags, reason_code, properties...} — not the
//       2-byte v4 {session_present, rc} shape; the Properties block itself now carries Topic Alias
//       Maximum (M7.1, see below), so this is also where that gets decoded/asserted;
//   (2) an unsupported protocol-level byte (neither 0x04 nor 0x05) is rejected with the 3.1.1-shaped
//       2-byte CONNACK {0x00, 0x01} and the connection is then closed by the broker;
//   (3) a v5 PUBLISH carrying a non-trivial Properties block (Payload Format Indicator + a User
//       Property — proving multi-record skip-parsing survives a property type this codec doesn't
//       store) round-trips its PAYLOAD correctly to a subscriber, proving `pos` was advanced exactly
//       past the properties block: no properties bytes leaked into the delivered payload, no payload
//       bytes truncated;
//   (4) a v5 SUBSCRIBE carrying a Subscription Identifier property (§the one Variable-Byte-Integer-
//       typed property reachable from SUBSCRIBE) is correctly skipped and the subscription grants
//       normally (SUBACK reason code == granted QoS, not an error);
//   (5) a v5 SUBSCRIBE to an ACL-denied topic gets SUBACK reason code 0x87 (Not Authorized), not the
//       v4 0x80.
//
// Also unit-tests the Properties codec itself (mqtt_codec.hpp's read_varint/read_properties) directly,
// with no socket involved: round-trips a CONNECT-shaped properties block containing Session Expiry
// Interval plus two User Properties (the one repeatable/variable-count property type in the table),
// confirming read_properties both surfaces the session-expiry value AND advances `pos` exactly past
// everything, including the User Properties it deliberately does not store.
//
// NOTE on outbound PUBLISH framing (M7/M7.1 — SUPERSEDED by 017 M7.2 PR A, see below): through M7.1 the
// PUBLISH scope cut was INBOUND parsing only (handle_publish reads and discards a v5 publisher's
// Properties block) — the broker's publish_to() (broker -> subscriber) was untouched and kept its pre-M7
// wire shape (Topic, [Packet Id], Payload, no Properties field at all) for every subscriber regardless of
// protocol version — this was actually a latent §3.3.1 bug for v5 (an empty Properties field is mandatory,
// not just "nothing to say"). 017 M7.2 PR A fixes this: publish_to() now writes a real v5 Properties block
// (empty when no extras are active, non-empty when e.g. Message Expiry applies). V5TestClient::wait_publish()
// below is updated accordingly (see its own comment) — this is the single highest regression risk in the
// M7.2 PR A change, per that function's comment.
//
// M7.1 (017 Open Questions follow-on) adds three more feature properties on top of M7's negotiation +
// parsing groundwork, tested below in a dedicated section:
//   - Session Expiry Interval TTL enforcement: a persistent session's stored state now actually expires
//     (test_session_expiry_ttl_enforced), with a control case proving the pre-M7.1 "never expires when
//     the property is absent" behavior is unregressed (test_session_expiry_absent_no_regression);
//   - server-initiated DISCONNECT (0xE0) with a reason code, sent for exactly two v5 cases — keep-alive
//     timeout (test_disconnect_reason_keep_alive_timeout, reason 0x8D) and session takeover
//     (test_disconnect_reason_session_taken_over, reason 0x8E) — with a v4 control case proving the
//     "just close the socket, no packet" behavior is unregressed there
//     (test_disconnect_reason_keep_alive_timeout_v4_no_regression);
//   - inbound Topic Alias resolution (test_topic_alias_inbound): establishing + reusing a client-side
//     alias, plus the two invalid-alias DISCONNECT/0x94 paths (unknown alias, alias value 0).
//
// 017 M7.2 PR A (outbound Properties infra + Message Expiry Interval + Maximum Packet Size) is tested in
// its own "M7.2 (PR A)" section below: the publish_to() v5-Properties fix itself (folded into every
// existing v5 wait_publish() call surviving unmodified — see that function's comment), Message Expiry
// Interval delivered-when-fresh / dropped-when-stale, Maximum Packet Size enforcement, and Will Message
// Expiry honored.
//
// 017 M7.2 PR B (Response Topic + Correlation Data + User Properties, end-to-end) is tested in its own
// "017 M7.2 PR B coverage" section below: both fields surviving PUBLISH -> subscriber, User Properties
// (including a duplicate key) surviving in order, a retained replay carrying both fields, Response Topic
// wildcard rejection (DISCONNECT reason 0x90), and a Will carrying Response Topic + a User Property.
//
// 017 M7.2 PR C (outbound Topic Alias — the broker->subscriber compression direction; M7.1 only shipped
// the client->broker half) is tested in "017 M7.2 PR C coverage" further below: a subscriber advertising
// Topic Alias Maximum in CONNECT gets an alias established on first delivery per topic and reused (empty
// topic name) on repeats, capped at its advertised maximum with a legal uncompressed fallback once
// exhausted, and a control subscriber that never advertised one never receives an alias (§3.1.2.11.2).
//
// 017 M7.2 PR D (Shared Subscriptions, `$share/<group>/<filter>`, §4.8.2) is tested in "017 M7.2 PR D
// coverage" further below: two members of the same (group, filter) each get exactly one message out of a
// round-robin sequence (deterministic here — see route_publish()'s own comment on why), an ordinary
// (non-shared) subscriber to the same topic proves regular fan-out is unaffected, a shared member's
// SUBSCRIBE deliberately does NOT get an immediate retained-message replay (§4.8.2's own "no guarantee",
// unlike an ordinary subscriber which still does), and a structurally malformed share (missing ShareName,
// missing TopicFilter after it, or a ShareName containing a wildcard character) is a Protocol Error —
// DISCONNECT 0x82, the SUBSCRIBE never granted.
// Deterministic, exit-code-gated (0 = pass); bounded polling; clean shutdown.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aero/broker/acl.hpp"
#include "aero/broker/native_broker.hpp"
#include "aero/transport/mqtt_codec.hpp"

namespace mqtt = aero::transport::mqtt;
using aero::broker::Config;
using aero::broker::NativeBroker;
using aero::broker::TopicAclAuthorizer;

namespace {

// ===== test-side Properties-block ENCODER (mqtt_codec.hpp only ships a decoder, see its own comment —
//       these are hand-rolled test helpers, not something production code needs) ======================
void put_prop_u32(std::vector<std::byte>& recs, std::uint8_t id, std::uint32_t v) {
    recs.push_back(static_cast<std::byte>(id));
    for (int i = 3; i >= 0; --i) recs.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void put_prop_byte(std::vector<std::byte>& recs, std::uint8_t id, std::uint8_t v) {
    recs.push_back(static_cast<std::byte>(id));
    recs.push_back(static_cast<std::byte>(v));
}
void put_prop_u16(std::vector<std::byte>& recs, std::uint8_t id, std::uint16_t v) {
    recs.push_back(static_cast<std::byte>(id));
    mqtt::put_u16_be(recs, v);
}
void put_prop_str_pair(std::vector<std::byte>& recs, std::uint8_t id, const std::string& k, const std::string& v) {
    recs.push_back(static_cast<std::byte>(id));
    mqtt::put_str(recs, k);
    mqtt::put_str(recs, v);
}
// 017 M7.2 PR B: Response Topic (0x08) is wire-encoded as a plain UTF-8 String — same shape as e.g.
// Content Type, just a length-prefixed string record.
void put_prop_str(std::vector<std::byte>& recs, std::uint8_t id, const std::string& v) {
    recs.push_back(static_cast<std::byte>(id));
    mqtt::put_str(recs, v);
}
// 017 M7.2 PR B: Correlation Data (0x09) is wire-encoded as length-prefixed Binary Data — deliberately
// takes raw bytes (not a std::string) so a test can embed a NUL byte and prove binary safety end-to-end.
void put_prop_binary(std::vector<std::byte>& recs, std::uint8_t id, const std::vector<std::byte>& v) {
    recs.push_back(static_cast<std::byte>(id));
    mqtt::put_u16_be(recs, static_cast<std::uint16_t>(v.size()));
    recs.insert(recs.end(), v.begin(), v.end());
}
void put_prop_varint(std::vector<std::byte>& recs, std::uint8_t id, std::uint32_t v) {
    recs.push_back(static_cast<std::byte>(id));
    mqtt::put_remaining_length(recs, v);
}
// Wraps already-encoded property records with their Property Length varint prefix (same encoding as
// put_remaining_length — this IS the write-side of what read_varint decodes) and appends to `out`.
void put_properties(std::vector<std::byte>& out, const std::vector<std::byte>& records) {
    mqtt::put_remaining_length(out, static_cast<std::uint32_t>(records.size()));
    out.insert(out.end(), records.begin(), records.end());
}

// ===== V5TestClient: like native_broker.cpp's TestClient, but drives protocol level + Properties
//       blocks explicitly on CONNECT/SUBSCRIBE/PUBLISH, and hands back raw CONNACK/SUBACK bodies (the
//       caller interprets the v4-vs-v5 shape — that's exactly what these tests are checking). ========
struct RawAck {
    bool ok = false;  // an ack packet actually arrived
    std::vector<std::byte> body;
};

class V5TestClient {
public:
    ~V5TestClient() { close(); }

    // M7.2 PR A: gained the trailing has_will/will_*/will_props_records knobs (mirrors native_broker.cpp's
    // ConnectOptions pattern for the same need) so test_will_message_expiry_honored below can exercise a
    // v5 Will carrying its own Message Expiry Interval — nothing in the pre-existing M7/M7.1 tests passes
    // these, so they're all unaffected (has_will defaults false, exactly the old no-Will CONNECT shape).
    [[nodiscard]] RawAck connect_raw(std::uint16_t port, const std::string& client_id,
                                      std::uint8_t protocol_level,
                                      const std::vector<std::byte>& connect_props_records = {},
                                      bool clean_session = true, std::uint16_t keep_alive_s = 60,
                                      bool has_will = false, const std::string& will_topic = "",
                                      const std::string& will_message = "", std::uint8_t will_qos = 0,
                                      bool will_retain = false,
                                      const std::vector<std::byte>& will_props_records = {}) {
        RawAck result;
        protocol_level_ = protocol_level;  // remembered so wait_publish() knows whether to expect a v5
                                            // Properties block on inbound PUBLISH (see its own comment)
        auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
        if (!fd) return result;
        fd_ = *fd;
        const auto writable = aero::pal::wait_writable(fd_, 2000);
        if (!writable || !*writable || !quark::pal::connect_result(fd_)) return result;

        running_.store(true, std::memory_order_release);
        peer_closed_.store(false, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });

        std::uint8_t flags = 0;
        if (clean_session) flags |= 0x02;
        if (has_will) {
            flags |= 0x04;
            flags |= static_cast<std::uint8_t>((will_qos & 0x03) << 3);
            if (will_retain) flags |= 0x20;
        }
        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(static_cast<std::byte>(protocol_level));
        vh.push_back(static_cast<std::byte>(flags));
        mqtt::put_u16_be(vh, keep_alive_s);
        if (protocol_level == 0x05) put_properties(vh, connect_props_records);
        mqtt::put_str(vh, client_id);
        if (has_will) {
            // Will Properties (MQTT 5 §3.1.3.2) sit immediately before Will Topic/Will Message — same
            // wire order handle_connect() parses (native_broker.hpp).
            if (protocol_level == 0x05) put_properties(vh, will_props_records);
            mqtt::put_str(vh, will_topic);
            mqtt::put_str(vh, will_message);
        }
        if (!mqtt::write_packet(fd_, std::byte{0x10}, vh)) return result;

        auto ack = wait_for(0x20, 2000);
        if (!ack) return result;
        result.ok = true;
        result.body = ack->body;
        return result;
    }

    // True once the reader thread has observed the broker close the connection (as opposed to this
    // client's own close() asking it to stop) — the protocol-version-reject test's way to confirm the
    // socket was actually torn down, not just that a CONNACK arrived.
    [[nodiscard]] bool peer_closed() const noexcept { return peer_closed_.load(std::memory_order_acquire); }

    [[nodiscard]] std::optional<std::vector<std::byte>> subscribe_v5(
        const std::string& filter, std::uint8_t qos, bool is_v5,
        const std::vector<std::byte>& sub_props_records = {}) {
        std::vector<std::byte> vh;
        mqtt::put_u16_be(vh, next_id());
        if (is_v5) put_properties(vh, sub_props_records);
        mqtt::put_str(vh, filter);
        vh.push_back(static_cast<std::byte>(qos));
        if (!mqtt::write_packet(fd_, std::byte{0x82}, vh)) return std::nullopt;
        auto ack = wait_for(0x90, 2000);
        if (!ack) return std::nullopt;
        return ack->body;
    }

    [[nodiscard]] bool publish_v5(const std::string& topic, const std::string& payload, std::uint8_t qos,
                                  bool retain, bool is_v5,
                                  const std::vector<std::byte>& pub_props_records = {}) {
        std::vector<std::byte> vh;
        mqtt::put_str(vh, topic);
        if (qos > 0) mqtt::put_u16_be(vh, next_id());
        if (is_v5) put_properties(vh, pub_props_records);
        for (char c : payload) vh.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        std::uint8_t pflags = static_cast<std::uint8_t>(qos << 1);
        if (retain) pflags |= 0x01;
        if (!mqtt::write_packet(fd_, static_cast<std::byte>(0x30 | pflags), vh)) return false;
        return qos == 0 || wait_for(0x40, 2000).has_value();
    }

    // MANDATORY M7.2 PR A fix (see this file's banner — the single highest regression risk in that PR):
    // through M7.1 the broker's outbound PUBLISH framing never carried a v5 Properties block at all (a
    // latent §3.3.1 bug PR A fixes), so this used to parse the SAME shape as v4 unconditionally. Now that
    // publish_to() writes a real (possibly-empty) Properties block for v5 sessions, this MUST skip it via
    // mqtt::read_properties() before slicing the payload, or every v5 test's payload would be misparsed —
    // starting with the leading Property Length byte(s) becoming part of a "payload" that no longer
    // matches what was sent. v4 sessions are completely unaffected (protocol_level_ stays 0x04, no
    // Properties block was ever written for them, none is skipped here either — byte-for-byte unchanged).
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> wait_publish(int timeout_ms = 1500) {
        auto got = wait_publish_v5_extras(timeout_ms);
        if (!got) return std::nullopt;
        return std::make_pair(std::move(got->topic), std::move(got->payload));
    }

    // A received PUBLISH's topic/payload PLUS its decoded v5 Properties (message_expiry_interval, in
    // particular) — for tests that need to assert on the properties themselves (017 M7.2 PR A's Message
    // Expiry tests: "delivered with remaining, not original, TTL"), not just the payload. For a v4
    // session, `props` is always a default-constructed (all-nullopt/empty) ParsedProperties — there is no
    // Properties block to decode.
    struct ReceivedPublish {
        std::string topic;
        std::string payload;
        mqtt::ParsedProperties props;
    };
    [[nodiscard]] std::optional<ReceivedPublish> wait_publish_v5_extras(int timeout_ms = 1500) {
        auto pkt = wait_for(0x30, timeout_ms);
        if (!pkt) return std::nullopt;
        const std::vector<std::byte>& b = pkt->body;
        if (b.size() < 2) return std::nullopt;
        const std::uint16_t tlen =
            (std::to_integer<std::uint8_t>(b[0]) << 8) | std::to_integer<std::uint8_t>(b[1]);
        std::size_t pos = 2 + tlen;
        if (pos > b.size()) return std::nullopt;
        std::string topic(reinterpret_cast<const char*>(b.data() + 2), tlen);
        const std::uint8_t qos = (pkt->type_flags >> 1) & 0x03;
        if (qos > 0) {
            if (pos + 2 > b.size()) return std::nullopt;
            const std::uint16_t pid =
                (std::to_integer<std::uint8_t>(b[pos]) << 8) | std::to_integer<std::uint8_t>(b[pos + 1]);
            pos += 2;
            std::vector<std::byte> ackv;
            mqtt::put_u16_be(ackv, pid);
            (void)mqtt::write_packet(fd_, std::byte{0x40}, ackv);
        }
        mqtt::ParsedProperties props;
        if (protocol_level_ == 0x05) {
            auto parsed = mqtt::read_properties(b, pos);
            if (!parsed) return std::nullopt;  // malformed Properties block — broker bug, fail the test
            props = std::move(*parsed);
        }
        if (pos > b.size()) return std::nullopt;
        std::string payload(reinterpret_cast<const char*>(b.data() + pos), b.size() - pos);
        ReceivedPublish result;
        result.topic = std::move(topic);
        result.payload = std::move(payload);
        result.props = std::move(props);
        return result;
    }

    // M7.1: an explicit, well-formed client->server DISCONNECT (0xE0, zero-length body — MQTT 5 §3.14.1
    // "Normal disconnection" is legal with no reason code / properties at all) — distinct from close()
    // below, which just drops the socket with no packet (an ungraceful end, exercising the Will/session-
    // persistence "not a clean disconnect" path instead).
    [[nodiscard]] bool disconnect_v5() { return mqtt::write_packet(fd_, std::byte{0xE0}, {}); }

    // Generic "wait for a packet with this fixed-header type nibble, hand back its raw body" — used by
    // the M7.1 DISCONNECT-reason-code tests (0xE0) where, unlike CONNACK/SUBACK above, there's no
    // dedicated wait_for wrapper yet.
    [[nodiscard]] std::optional<std::vector<std::byte>> wait_for_packet(std::uint8_t type_high_nibble,
                                                                          int timeout_ms = 2000) {
        auto pkt = wait_for(type_high_nibble, timeout_ms);
        if (!pkt) return std::nullopt;
        return pkt->body;
    }

    void close() {
        running_.store(false, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
        if (fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(fd_);
            fd_ = quark::pal::invalid_fd;
        }
    }

private:
    void reader_loop() {
        while (running_.load(std::memory_order_acquire)) {
            auto pkt = mqtt::read_packet(fd_, running_);
            if (!pkt) {
                if (running_.load(std::memory_order_acquire)) peer_closed_.store(true, std::memory_order_release);
                break;
            }
            std::lock_guard<std::mutex> g(mu_);
            inbox_.push_back(std::move(*pkt));
        }
    }
    std::optional<mqtt::Packet> wait_for(std::uint8_t type_high_nibble, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> g(mu_);
                for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
                    if ((it->type_flags & 0xF0) == type_high_nibble) {
                        mqtt::Packet p = std::move(*it);
                        inbox_.erase(it);
                        return p;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::uint16_t next_id() {
        const std::uint16_t id = ++packet_id_;
        return id == 0 ? ++packet_id_ : id;
    }

    quark::pal::fd_t fd_ = quark::pal::invalid_fd;
    std::atomic<bool> running_{false};
    std::atomic<bool> peer_closed_{false};
    std::thread reader_;
    std::mutex mu_;
    std::vector<mqtt::Packet> inbox_;
    std::uint16_t packet_id_ = 0;
    std::uint8_t protocol_level_ = 0x04;  // set in connect_raw — see wait_publish()'s comment
};

// ===== unit coverage: the Properties codec itself, no socket involved ==================================
// Round-trips a CONNECT-shaped properties block: Session Expiry Interval (0x11) + two User Properties
// (0x26, repeatable) + Topic Alias (0x23, M7.1) + Message Expiry Interval (0x02, M7.2 PR A) + Maximum
// Packet Size (0x27, M7.2 PR A) — proves read_properties surfaces every value this codec stores AND
// advances `pos` exactly past every record, including the ones it doesn't store. A two-byte sentinel
// appended after the properties block confirms `pos` lands exactly at the boundary, not short or long.
// (Topic Alias/Message Expiry don't legally co-occur with Session Expiry Interval/Maximum Packet Size in
// a real CONNECT — the former pair are PUBLISH-only — but read_properties() itself is packet-type-
// agnostic, so this is a fair same-codepath test of the codec in isolation.)
bool test_properties_codec() {
    bool ok = true;
    std::vector<std::byte> records;
    put_prop_u32(records, 0x11, 3600);                     // Session Expiry Interval
    put_prop_str_pair(records, 0x26, "k1", "v1");           // User Property #1
    put_prop_str_pair(records, 0x26, "k2", "v2");           // User Property #2
    put_prop_u16(records, 0x23, 42);                        // Topic Alias (M7.1)
    put_prop_u32(records, 0x02, 30);                        // Message Expiry Interval (M7.2 PR A)
    put_prop_u32(records, 0x27, 65536);                     // Maximum Packet Size (M7.2 PR A)
    put_prop_str(records, 0x08, "reply/to/topic");           // Response Topic (M7.2 PR B)
    // Correlation Data (M7.2 PR B) — embeds a NUL byte to prove binary safety (vector<byte>, not a
    // C-string that would silently truncate at the first \0).
    const std::vector<std::byte> corr_data{std::byte{'a'}, std::byte{0x00}, std::byte{'b'}};
    put_prop_binary(records, 0x09, corr_data);

    std::vector<std::byte> body;
    mqtt::put_remaining_length(body, static_cast<std::uint32_t>(records.size()));
    body.insert(body.end(), records.begin(), records.end());
    body.push_back(std::byte{0xAB});  // sentinel: must NOT be consumed by read_properties
    body.push_back(std::byte{0xCD});

    std::size_t pos = 0;
    auto parsed = mqtt::read_properties(body, pos);
    ok &= parsed.has_value();
    ok &= parsed.has_value() && parsed->session_expiry_interval.has_value() &&
          *parsed->session_expiry_interval == 3600u;
    ok &= parsed.has_value() && parsed->topic_alias.has_value() && *parsed->topic_alias == 42u;
    ok &= parsed.has_value() && parsed->message_expiry_interval.has_value() &&
          *parsed->message_expiry_interval == 30u;
    ok &= parsed.has_value() && parsed->maximum_packet_size.has_value() &&
          *parsed->maximum_packet_size == 65536u;
    // 017 M7.2 PR B: the two User Properties (0x26) are asserted now — the pre-existing block above
    // wrote them but never checked them; User Property is repeatable (§3.3.2.3.7), so both must survive
    // in order, not just be de-duplicated/overwritten.
    ok &= parsed.has_value() && parsed->user_properties.size() == 2;
    if (parsed.has_value() && parsed->user_properties.size() == 2) {
        ok &= parsed->user_properties[0].first == "k1" && parsed->user_properties[0].second == "v1";
        ok &= parsed->user_properties[1].first == "k2" && parsed->user_properties[1].second == "v2";
    }
    ok &= parsed.has_value() && parsed->response_topic.has_value() &&
          *parsed->response_topic == "reply/to/topic";
    ok &= parsed.has_value() && parsed->correlation_data.has_value() &&
          *parsed->correlation_data == corr_data;
    ok &= pos == body.size() - 2;  // stopped exactly at the sentinel, not before or past it
    ok &= pos < body.size() && std::to_integer<std::uint8_t>(body[pos]) == 0xAB;

    // Malformed cases: an ID this codec doesn't recognize fails closed (nullopt), and a truncated
    // Property Length also fails closed rather than reading out of bounds.
    std::vector<std::byte> bad_id;
    put_prop_byte(bad_id, 0x99, 0x01);  // 0x99 is not in the type table
    std::vector<std::byte> body_bad_id;
    mqtt::put_remaining_length(body_bad_id, static_cast<std::uint32_t>(bad_id.size()));
    body_bad_id.insert(body_bad_id.end(), bad_id.begin(), bad_id.end());
    std::size_t pos2 = 0;
    ok &= !mqtt::read_properties(body_bad_id, pos2).has_value();

    std::vector<std::byte> truncated{std::byte{0x05}};  // claims 5 property bytes, has none
    std::size_t pos3 = 0;
    ok &= !mqtt::read_properties(truncated, pos3).has_value();

    return ok;
}

// (1) v5 CONNECT with a CONNECT Properties block (Session Expiry Interval) -> v5-shaped CONNACK:
// {ack_flags, reason_code, properties...}, not v4's 2-byte {session_present, rc}. M7.1: the Properties
// block is no longer empty — it now carries Topic Alias Maximum (0x22) = kTopicAliasMax (16), which this
// test decodes and asserts (folds in item (6) of the M7.1 test plan: CONNACK Topic Alias Maximum).
bool test_v5_connect_roundtrip() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    std::vector<std::byte> connect_props;
    put_prop_u32(connect_props, 0x11, 7200);  // Session Expiry Interval

    V5TestClient c;
    auto ack = c.connect_raw(port, "v5-basic", /*protocol_level=*/0x05, connect_props);
    ok &= ack.ok;
    // ack_flags(1) + reason_code(1) + properties: prop-length-varint(1, value=3) + {id(1) + u16 value(2)}.
    ok &= ack.body.size() == 6;
    if (ack.body.size() == 6) {
        ok &= std::to_integer<std::uint8_t>(ack.body[0]) == 0x00;  // ack_flags: no session present
        ok &= std::to_integer<std::uint8_t>(ack.body[1]) == 0x00;  // reason code: Success
        ok &= std::to_integer<std::uint8_t>(ack.body[2]) == 0x03;  // Property Length = 3
        ok &= std::to_integer<std::uint8_t>(ack.body[3]) == 0x22;  // Topic Alias Maximum property id
        const std::uint16_t topic_alias_max = static_cast<std::uint16_t>(
            (std::to_integer<std::uint8_t>(ack.body[4]) << 8) | std::to_integer<std::uint8_t>(ack.body[5]));
        ok &= topic_alias_max == 16;
    }

    c.close();
    broker.stop();
    return ok;
}

// (2) An unsupported protocol-level byte (neither 0x04 nor 0x05) is rejected with the 3.1.1-shaped
// 2-byte CONNACK {0x00, 0x01} and the broker then closes the connection.
bool test_unsupported_protocol_version() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient c;
    auto ack = c.connect_raw(port, "bogus-version", /*protocol_level=*/0x03);
    ok &= ack.ok;
    ok &= ack.body.size() == 2;
    if (ack.body.size() == 2) {
        ok &= std::to_integer<std::uint8_t>(ack.body[0]) == 0x00;
        ok &= std::to_integer<std::uint8_t>(ack.body[1]) == 0x01;  // unacceptable protocol version
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!c.peer_closed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ok &= c.peer_closed();

    c.close();
    broker.stop();
    return ok;
}

// (3) A v5 PUBLISH with a non-trivial Properties block (Payload Format Indicator + one User Property)
// round-trips its PAYLOAD correctly — proves `pos` advanced exactly past a multi-record properties
// block it doesn't store, with no leakage into and no truncation of the actual payload.
bool test_v5_publish_properties_roundtrip() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient sub, pub;
    ok &= sub.connect_raw(port, "v5-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("v5/pub/topic", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    ok &= pub.connect_raw(port, "v5-pub", 0x05).ok;

    std::vector<std::byte> pub_props;
    put_prop_byte(pub_props, 0x01, 0x01);                       // Payload Format Indicator: UTF-8
    put_prop_str_pair(pub_props, 0x26, "trace-id", "abc-123");  // User Property (not stored, must skip)

    const std::string payload = "exact-payload-bytes-42";
    ok &= pub.publish_v5("v5/pub/topic", payload, /*qos=*/1, /*retain=*/false, /*is_v5=*/true, pub_props);

    auto got = sub.wait_publish();
    ok &= got.has_value() && got->first == "v5/pub/topic" && got->second == payload;

    sub.close();
    pub.close();
    broker.stop();
    return ok;
}

// (4) A v5 SUBSCRIBE carrying a Subscription Identifier property (0x0B, the one Variable-Byte-Integer
// property reachable from SUBSCRIBE) is correctly skipped and the subscription grants normally.
bool test_v5_subscribe_with_subscription_identifier() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient c;
    ok &= c.connect_raw(port, "v5-subid", 0x05).ok;

    std::vector<std::byte> sub_props;
    put_prop_varint(sub_props, 0x0B, 7);  // Subscription Identifier = 7

    auto ack = c.subscribe_v5("v5/subid/topic", /*qos=*/1, /*is_v5=*/true, sub_props);
    ok &= ack.has_value();
    // v5 SUBACK shape: Packet Id (2) + Properties (1, empty) + one reason code = 4 bytes.
    ok &= ack.has_value() && ack->size() == 4;
    if (ack.has_value() && ack->size() == 4) {
        ok &= std::to_integer<std::uint8_t>((*ack)[2]) == 0x00;  // empty SUBACK properties
        ok &= std::to_integer<std::uint8_t>((*ack)[3]) == 1;     // granted QoS 1 — not 0x87/0x80
    }

    c.close();
    broker.stop();
    return ok;
}

// (5) A v5 SUBSCRIBE to an ACL-denied topic gets SUBACK reason code 0x87 (Not Authorized) — the
// v5-specific code — not v4's 0x80.
bool test_v5_subscribe_denied() {
    bool ok = true;
    // Default::Closed, zero rules registered -> every SUBSCRIBE is denied (mirrors
    // native_broker_security.cpp's ACL-matrix pattern, minimal here since only the denial path matters).
    auto authorizer = std::make_shared<TopicAclAuthorizer>(TopicAclAuthorizer::Default::Closed);
    Config cfg{"127.0.0.1", /*listen_port=*/0};
    cfg.authorizer = authorizer;
    NativeBroker broker(cfg);
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient c;
    ok &= c.connect_raw(port, "v5-denied", 0x05).ok;
    auto ack = c.subscribe_v5("denied/topic", /*qos=*/1, /*is_v5=*/true);
    ok &= ack.has_value();
    ok &= ack.has_value() && ack->size() == 4;
    if (ack.has_value() && ack->size() == 4) ok &= std::to_integer<std::uint8_t>((*ack)[3]) == 0x87;

    c.close();
    broker.stop();
    return ok;
}

// ===== 017 M7.1 coverage: Session Expiry Interval TTL, server DISCONNECT reason codes, inbound Topic
//       Alias — see this file's banner for M7's own scope; the tests below extend it, not replace it. ===

// (M7.1-1) A persistent (Clean Start=0) v5 session that sent Session Expiry Interval=1 (second) on
// CONNECT, then cleanly DISCONNECTs, has its stored session state (subs/queued messages) discarded if
// the client doesn't reconnect within that 1s TTL — CONNACK's session-present bit must be 0 on the late
// reconnect (not restored), instead of today's "persistent session state never expires" default.
bool test_session_expiry_ttl_enforced() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    std::vector<std::byte> connect_props;
    put_prop_u32(connect_props, 0x11, 1);  // Session Expiry Interval = 1s

    V5TestClient c1;
    auto ack1 = c1.connect_raw(port, "ttl-client", /*protocol_level=*/0x05, connect_props,
                               /*clean_session=*/false);
    ok &= ack1.ok;
    auto suback = c1.subscribe_v5("ttl/topic", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();
    ok &= c1.disconnect_v5();
    c1.close();

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // > the 1s TTL

    V5TestClient c2;
    auto ack2 = c2.connect_raw(port, "ttl-client", /*protocol_level=*/0x05, /*connect_props_records=*/{},
                               /*clean_session=*/false);
    ok &= ack2.ok;
    ok &= !ack2.body.empty() && (std::to_integer<std::uint8_t>(ack2.body[0]) & 0x01) == 0;  // NOT restored

    c2.close();
    broker.stop();
    return ok;
}

// (M7.1-2, control) Same sequence as above but WITHOUT Session Expiry Interval on the first CONNECT —
// confirms today's "a persistent session with no expiry property never expires" behavior is completely
// unregressed by the TTL enforcement added above.
bool test_session_expiry_absent_no_regression() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient c1;
    auto ack1 = c1.connect_raw(port, "no-ttl-client", /*protocol_level=*/0x05, /*connect_props_records=*/{},
                               /*clean_session=*/false);
    ok &= ack1.ok;
    auto suback = c1.subscribe_v5("no-ttl/topic", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();
    ok &= c1.disconnect_v5();
    c1.close();

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // same wait as the enforced case above

    V5TestClient c2;
    auto ack2 = c2.connect_raw(port, "no-ttl-client", /*protocol_level=*/0x05, /*connect_props_records=*/{},
                               /*clean_session=*/false);
    ok &= ack2.ok;
    ok &= !ack2.body.empty() && (std::to_integer<std::uint8_t>(ack2.body[0]) & 0x01) == 1;  // still restored

    c2.close();
    broker.stop();
    return ok;
}

// (M7.1-3) A v5 session that goes silent past 1.5x its 1s keep-alive interval gets an explicit
// server->client DISCONNECT (0xE0) with reason code 0x8D (Keep Alive timeout) before the broker closes
// the socket — check keep_alive_expired()'s 1500ms/keep-alive-second multiplier in native_broker.hpp.
bool test_disconnect_reason_keep_alive_timeout() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient c;
    auto ack = c.connect_raw(port, "v5-keepalive", /*protocol_level=*/0x05, /*connect_props_records=*/{},
                             /*clean_session=*/true, /*keep_alive_s=*/1);
    ok &= ack.ok;

    auto disc = c.wait_for_packet(0xE0, 3000);
    ok &= disc.has_value();
    ok &= disc.has_value() && !disc->empty() && std::to_integer<std::uint8_t>((*disc)[0]) == 0x8D;

    c.close();
    broker.stop();
    return ok;
}

// (M7.1-4, control) Same keep-alive-timeout conditions but a v4 session — MQTT 3.1.1 has no DISCONNECT-
// from-server packet, so this must stay byte-for-byte the pre-M7.1 "just close the socket" behavior: no
// 0xE0 ever arrives, the broker just drops the connection.
bool test_disconnect_reason_keep_alive_timeout_v4_no_regression() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient c;
    auto ack = c.connect_raw(port, "v4-keepalive", /*protocol_level=*/0x04, /*connect_props_records=*/{},
                             /*clean_session=*/true, /*keep_alive_s=*/1);
    ok &= ack.ok;

    auto disc = c.wait_for_packet(0xE0, 3000);
    ok &= !disc.has_value();  // no DISCONNECT packet ever arrives for v4

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!c.peer_closed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ok &= c.peer_closed();  // but the broker did close the socket

    c.close();
    broker.stop();
    return ok;
}

// (M7.1-5) A session-takeover CONNECT (3.1.4) under a client-id already live causes the FIRST session to
// receive an explicit DISCONNECT (0xE0) with reason code 0x8E (Session taken over) before it tears down.
bool test_disconnect_reason_session_taken_over() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient c1;
    auto ack1 = c1.connect_raw(port, "takeover-client", 0x05);
    ok &= ack1.ok;

    V5TestClient c2;
    auto ack2 = c2.connect_raw(port, "takeover-client", 0x05);
    ok &= ack2.ok;

    auto disc = c1.wait_for_packet(0xE0, 2000);
    ok &= disc.has_value();
    ok &= disc.has_value() && !disc->empty() && std::to_integer<std::uint8_t>((*disc)[0]) == 0x8E;

    c1.close();
    c2.close();
    broker.stop();
    return ok;
}

// (M7.1-6) Inbound Topic Alias (MQTT 5 §3.3.2.3.4): a v5 publisher establishes Topic Alias 5 on
// "sensor/x" via a normal PUBLISH (topic name + alias together), then a SECOND PUBLISH with an EMPTY
// topic name and only the alias resolves to the SAME topic — a subscriber sees both deliveries with the
// correctly resolved topic (this test carries no Message Expiry, so it exercises publish_to()'s M7.2 PR A
// Properties-writing path with an empty properties block — a fair unmodified-assertion regression check
// for the wait_publish() fix, see this file's banner). A fresh connection that never established an alias
// (or sends alias value 0, which is always invalid) gets 0xE0/0x94 DISCONNECT and nothing is ever
// delivered.
bool test_topic_alias_inbound() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient sub;
    ok &= sub.connect_raw(port, "alias-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("sensor/x", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    V5TestClient pub;
    ok &= pub.connect_raw(port, "alias-pub", 0x05).ok;

    std::vector<std::byte> establish_props;
    put_prop_u16(establish_props, 0x23, 5);  // Topic Alias = 5, establishing it on "sensor/x"
    ok &= pub.publish_v5("sensor/x", "payload-1", /*qos=*/1, /*retain=*/false, /*is_v5=*/true, establish_props);

    auto got1 = sub.wait_publish();
    ok &= got1.has_value() && got1->first == "sensor/x" && got1->second == "payload-1";

    std::vector<std::byte> alias_only_props;
    put_prop_u16(alias_only_props, 0x23, 5);  // same alias, no topic name this time
    ok &= pub.publish_v5("", "payload-2", /*qos=*/1, /*retain=*/false, /*is_v5=*/true, alias_only_props);

    auto got2 = sub.wait_publish();
    ok &= got2.has_value() && got2->first == "sensor/x" && got2->second == "payload-2";

    pub.close();

    // Unknown alias on a FRESH connection (never established there) -> 0xE0/0x94, nothing delivered.
    V5TestClient unknown_pub;
    ok &= unknown_pub.connect_raw(port, "alias-unknown", 0x05).ok;
    std::vector<std::byte> unknown_props;
    put_prop_u16(unknown_props, 0x23, 5);
    ok &= unknown_pub.publish_v5("", "should-not-arrive", /*qos=*/0, /*retain=*/false, /*is_v5=*/true,
                                 unknown_props);

    auto disc1 = unknown_pub.wait_for_packet(0xE0, 2000);
    ok &= disc1.has_value() && !disc1->empty() && std::to_integer<std::uint8_t>((*disc1)[0]) == 0x94;

    auto stray1 = sub.wait_publish(500);
    ok &= !stray1.has_value();

    unknown_pub.close();

    // Alias value 0 is always invalid (MQTT 5 §3.3.2.3.4), even with a non-empty topic name.
    V5TestClient zero_pub;
    ok &= zero_pub.connect_raw(port, "alias-zero", 0x05).ok;
    std::vector<std::byte> zero_props;
    put_prop_u16(zero_props, 0x23, 0);
    ok &= zero_pub.publish_v5("sensor/x", "should-not-arrive-either", /*qos=*/0, /*retain=*/false,
                              /*is_v5=*/true, zero_props);

    auto disc2 = zero_pub.wait_for_packet(0xE0, 2000);
    ok &= disc2.has_value() && !disc2->empty() && std::to_integer<std::uint8_t>((*disc2)[0]) == 0x94;

    auto stray2 = sub.wait_publish(500);
    ok &= !stray2.has_value();

    zero_pub.close();
    sub.close();
    broker.stop();
    return ok;
}

// ===== 017 M7.2 PR A coverage: outbound Properties infra (publish_to() now writes a real v5 Properties
//       block — fixing the previously-latent §3.3.1 "at least an empty Properties field" bug, see this
//       file's banner), Message Expiry Interval TTL enforcement, and Maximum Packet Size enforcement.
//       Response Topic/Correlation Data/User Properties are covered separately, in the "017 M7.2 PR B
//       coverage" section further below. ==============================================================

// (M7.2-1) A retained PUBLISH with Message Expiry Interval=5s, replayed to a SUBSCRIBE that arrives
// shortly afterward (not stale), is delivered with a REMAINING (not original) Message Expiry — proves
// publish_to()'s choke point 1 recomputes time-to-live at send time rather than re-encoding the original
// interval verbatim (Design §2's `remaining, not original` requirement).
bool test_message_expiry_delivered_when_fresh() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient pub;
    ok &= pub.connect_raw(port, "m72-fresh-pub", 0x05).ok;
    std::vector<std::byte> pub_props;
    put_prop_u32(pub_props, 0x02, 5);  // Message Expiry Interval = 5s
    ok &= pub.publish_v5("m72/fresh", "still-fresh", /*qos=*/1, /*retain=*/true, /*is_v5=*/true, pub_props);
    pub.close();

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // let >1s of the 5s TTL elapse

    V5TestClient sub;
    ok &= sub.connect_raw(port, "m72-fresh-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("m72/fresh", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    auto got = sub.wait_publish_v5_extras();
    ok &= got.has_value() && got->topic == "m72/fresh" && got->payload == "still-fresh";
    ok &= got.has_value() && got->props.message_expiry_interval.has_value() &&
          *got->props.message_expiry_interval < 5u;  // remaining, not the original 5

    sub.close();
    broker.stop();
    return ok;
}

// (M7.2-2) A retained PUBLISH with Message Expiry Interval=1s, replayed to a SUBSCRIBE that arrives AFTER
// that 1s has already elapsed, is silently dropped by publish_to()'s choke point 1 — nothing is ever
// delivered, not even a stale copy.
bool test_message_expiry_dropped_when_stale() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient pub;
    ok &= pub.connect_raw(port, "m72-stale-pub", 0x05).ok;
    std::vector<std::byte> pub_props;
    put_prop_u32(pub_props, 0x02, 1);  // Message Expiry Interval = 1s
    ok &= pub.publish_v5("m72/stale", "should-not-arrive", /*qos=*/1, /*retain=*/true, /*is_v5=*/true,
                         pub_props);
    pub.close();

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // > the 1s TTL

    V5TestClient sub;
    ok &= sub.connect_raw(port, "m72-stale-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("m72/stale", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    auto got = sub.wait_publish(500);
    ok &= !got.has_value();  // retained replay dropped the stale message — timeout is the correct outcome

    sub.close();
    broker.stop();
    return ok;
}

// (M7.2-3) A subscriber CONNECTs with Maximum Packet Size (0x27) set to a small, precisely-computed byte
// budget; a PUBLISH whose serialized wire size would exceed it is dropped (choke point 2 in publish_to()),
// while one that fits exactly at the cap is delivered.
//
// Byte budget derivation for topic "m72/max" (7 chars), QoS 0 (no packet id), v5 (empty Properties block
// when no extras are active — the M7.2 PR A fix — costs exactly 1 byte: a zero Property Length varint):
//   variable header = topic (2-byte length prefix + 7) + properties (1, empty) + payload
//                   = 9 + 1 + payload_len = 10 + payload_len
//   wire packet      = fixed-header byte (1) + remaining-length varint (1, since vh stays < 128 bytes)
//                       + variable header
//                   = 2 + (10 + payload_len) = 12 + payload_len
// Budget = 20 => an 8-byte payload totals exactly 20 (fits, delivered); a 9-byte payload totals 21
// (exceeds, dropped).
bool test_max_packet_size_enforced() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    std::vector<std::byte> sub_connect_props;
    put_prop_u32(sub_connect_props, 0x27, 20);  // Maximum Packet Size = 20 bytes, see comment above

    V5TestClient sub;
    ok &= sub.connect_raw(port, "m72-cap-sub", /*protocol_level=*/0x05, sub_connect_props).ok;
    auto suback = sub.subscribe_v5("m72/max", /*qos=*/0, /*is_v5=*/true);
    ok &= suback.has_value();

    V5TestClient pub;
    ok &= pub.connect_raw(port, "m72-cap-pub", 0x05).ok;

    // Exceeds the cap by 1 byte (total 21 > 20) — must be dropped, not delivered.
    ok &= pub.publish_v5("m72/max", "123456789", /*qos=*/0, /*retain=*/false, /*is_v5=*/true);
    auto too_big = sub.wait_publish(500);
    ok &= !too_big.has_value();

    // Fits exactly at the cap (total 20 == 20) — must be delivered.
    ok &= pub.publish_v5("m72/max", "12345678", /*qos=*/0, /*retain=*/false, /*is_v5=*/true);
    auto fits = sub.wait_publish();
    ok &= fits.has_value() && fits->first == "m72/max" && fits->second == "12345678";

    pub.close();
    sub.close();
    broker.stop();
    return ok;
}

// (M7.2-4) A v5 client's retained Will carries Message Expiry Interval=1s (Design §2's Will-extras wiring
// — Session::will_extras, populated in handle_connect from the Will Properties block, consulted by
// teardown_session's Will-delivery call). After an abrupt disconnect fires the Will (stored retained,
// since will_retain=true), waiting past that 1s and THEN subscribing must NOT deliver the now-stale Will
// PUBLISH — proves the Will path shares deliver_publish()'s Message Expiry choke point exactly like a
// regular PUBLISH, not a silently-dropped feature (leaving will_extras always-empty, as it was before this
// PR, would make this test fail: the Will would be replayed as if it never expires).
bool test_will_message_expiry_honored() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    std::vector<std::byte> will_props;
    put_prop_u32(will_props, 0x02, 1);  // Will's Message Expiry Interval = 1s

    {
        V5TestClient dying;
        auto ack = dying.connect_raw(port, "m72-will-dying", /*protocol_level=*/0x05,
                                     /*connect_props_records=*/{}, /*clean_session=*/true,
                                     /*keep_alive_s=*/60, /*has_will=*/true, /*will_topic=*/"m72/will",
                                     /*will_message=*/"should-be-stale", /*will_qos=*/1,
                                     /*will_retain=*/true, will_props);
        ok &= ack.ok;
        dying.close();  // no DISCONNECT sent — abrupt end, fires the Will (3.1.2.5), stored retained
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // > the Will's 1s Message Expiry

    V5TestClient sub;
    ok &= sub.connect_raw(port, "m72-will-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("m72/will", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    auto got = sub.wait_publish(500);
    ok &= !got.has_value();  // the stale retained Will is dropped by publish_to()'s choke point, not replayed

    sub.close();
    broker.stop();
    return ok;
}

// ===== 017 M7.2 PR B coverage: Response Topic + Correlation Data + User Properties, end-to-end from
//       inbound PUBLISH through delivery to a subscriber (via NativeBroker::PublishExtras, threaded
//       through deliver_publish/publish_to — see native_broker.hpp's PublishExtras/PublishProperties
//       comments) and through retained replay / Will delivery, which share the same delivery path. =====

// (M7.2b-1) Both Response Topic and Correlation Data (the latter carrying an embedded NUL, proving
// binary safety end-to-end — not just at the codec unit level, see test_properties_codec) survive a
// PUBLISH -> subscriber round trip.
bool test_response_topic_and_correlation_data_roundtrip() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient sub, pub;
    ok &= sub.connect_raw(port, "m72b-rtcd-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("m72b/rtcd", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    ok &= pub.connect_raw(port, "m72b-rtcd-pub", 0x05).ok;
    std::vector<std::byte> pub_props;
    put_prop_str(pub_props, 0x08, "m72b/reply-here");
    const std::vector<std::byte> corr_data{std::byte{0x01}, std::byte{0x00}, std::byte{0x02}};
    put_prop_binary(pub_props, 0x09, corr_data);
    ok &= pub.publish_v5("m72b/rtcd", "payload-with-props", /*qos=*/1, /*retain=*/false, /*is_v5=*/true,
                         pub_props);

    auto got = sub.wait_publish_v5_extras();
    ok &= got.has_value() && got->topic == "m72b/rtcd" && got->payload == "payload-with-props";
    ok &= got.has_value() && got->props.response_topic.has_value() &&
          *got->props.response_topic == "m72b/reply-here";
    ok &= got.has_value() && got->props.correlation_data.has_value() &&
          *got->props.correlation_data == corr_data;

    pub.close();
    sub.close();
    broker.stop();
    return ok;
}

// (M7.2b-2) 3+ User Properties, including a DUPLICATE key with a different value, all survive in order —
// proves read_properties()/publish_to() both treat User Property as repeatable (§3.3.2.3.7), not
// overwritten on a later occurrence of the same key.
bool test_user_properties_roundtrip() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient sub, pub;
    ok &= sub.connect_raw(port, "m72b-up-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("m72b/up", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    ok &= pub.connect_raw(port, "m72b-up-pub", 0x05).ok;
    std::vector<std::byte> pub_props;
    put_prop_str_pair(pub_props, 0x26, "device", "plc-1");
    put_prop_str_pair(pub_props, 0x26, "unit", "celsius");
    put_prop_str_pair(pub_props, 0x26, "device", "plc-1-secondary");  // duplicate key, different value
    ok &= pub.publish_v5("m72b/up", "payload", /*qos=*/1, /*retain=*/false, /*is_v5=*/true, pub_props);

    auto got = sub.wait_publish_v5_extras();
    ok &= got.has_value() && got->topic == "m72b/up";
    ok &= got.has_value() && got->props.user_properties.size() == 3;
    if (got.has_value() && got->props.user_properties.size() == 3) {
        ok &= got->props.user_properties[0].first == "device" &&
              got->props.user_properties[0].second == "plc-1";
        ok &= got->props.user_properties[1].first == "unit" &&
              got->props.user_properties[1].second == "celsius";
        ok &= got->props.user_properties[2].first == "device" &&
              got->props.user_properties[2].second == "plc-1-secondary";
    }

    pub.close();
    sub.close();
    broker.stop();
    return ok;
}

// (M7.2b-3) A retained PUBLISH carrying both Response Topic and Correlation Data, replayed to a LATER
// SUBSCRIBE, still carries both — exercises RetainedMessage::extras (threaded by PR A, populated by this
// PR): proves publish_to()'s retained-replay path writes the same Properties block a live fan-out would.
bool test_response_topic_survives_retained_replay() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient pub;
    ok &= pub.connect_raw(port, "m72b-retained-pub", 0x05).ok;
    std::vector<std::byte> pub_props;
    put_prop_str(pub_props, 0x08, "m72b/retained-reply");
    const std::vector<std::byte> corr_data{std::byte{0xAA}, std::byte{0xBB}};
    put_prop_binary(pub_props, 0x09, corr_data);
    ok &= pub.publish_v5("m72b/retained", "sticky-payload", /*qos=*/1, /*retain=*/true, /*is_v5=*/true,
                         pub_props);
    pub.close();

    V5TestClient sub;
    ok &= sub.connect_raw(port, "m72b-retained-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("m72b/retained", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    auto got = sub.wait_publish_v5_extras();
    ok &= got.has_value() && got->topic == "m72b/retained" && got->payload == "sticky-payload";
    ok &= got.has_value() && got->props.response_topic.has_value() &&
          *got->props.response_topic == "m72b/retained-reply";
    ok &= got.has_value() && got->props.correlation_data.has_value() &&
          *got->props.correlation_data == corr_data;

    sub.close();
    broker.stop();
    return ok;
}

// (M7.2b-4) A Response Topic containing a wildcard ('+') is a protocol error (§3.3.2.3.5 — a Response
// Topic must be a valid Topic Name, never a Topic Filter) — the broker sends DISCONNECT reason 0x90
// (Topic Name Invalid) and the PUBLISH is never delivered to a subscriber that would otherwise have
// matched it.
bool test_response_topic_wildcard_rejected() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient sub;
    ok &= sub.connect_raw(port, "m72b-wc-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("m72b/wc", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    V5TestClient pub;
    ok &= pub.connect_raw(port, "m72b-wc-pub", 0x05).ok;
    std::vector<std::byte> pub_props;
    put_prop_str(pub_props, 0x08, "m72b/+/invalid");  // wildcard — illegal in a Response Topic
    // qos=0: publish_v5() only blocks on a PUBACK for qos>0, and the broker never gets that far here
    // (handle_publish rejects before acting on the PUBLISH at all) — a qos>0 wait would just time out.
    ok &= pub.publish_v5("m72b/wc", "should-not-arrive", /*qos=*/0, /*retain=*/false, /*is_v5=*/true,
                         pub_props);

    auto disc = pub.wait_for_packet(0xE0, 2000);
    ok &= disc.has_value() && disc->size() >= 1 && std::to_integer<std::uint8_t>((*disc)[0]) == 0x90;

    auto got = sub.wait_publish(500);
    ok &= !got.has_value();  // never delivered

    pub.close();
    sub.close();
    broker.stop();
    return ok;
}

// (M7.2b-5) Mirrors test_will_message_expiry_honored's pattern: a v5 Will carries Response Topic + a
// User Property (Design §3's Will Properties extension — Session::will_extras now captures these the
// same way it already captured Message Expiry Interval in PR A). An abrupt disconnect fires the Will;
// a later subscriber sees both fields survive — proving the Will/regular-PUBLISH shared delivery path
// invariant this file documents extends to PR B's new fields too.
bool test_will_response_topic_and_user_properties_honored() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    std::vector<std::byte> will_props;
    put_prop_str(will_props, 0x08, "m72b/will-reply");
    put_prop_str_pair(will_props, 0x26, "reason", "abrupt-disconnect");

    {
        V5TestClient dying;
        auto ack = dying.connect_raw(port, "m72b-will-dying", /*protocol_level=*/0x05,
                                     /*connect_props_records=*/{}, /*clean_session=*/true,
                                     /*keep_alive_s=*/60, /*has_will=*/true, /*will_topic=*/"m72b/will",
                                     /*will_message=*/"will-payload", /*will_qos=*/1,
                                     /*will_retain=*/true, will_props);
        ok &= ack.ok;
        dying.close();  // abrupt end — fires the Will (3.1.2.5), stored retained so the LATER subscriber
                        // below (which connects after the Will has already fired) still receives it —
                        // mirrors test_will_message_expiry_honored's own will_retain=true for the same
                        // reason.
    }

    V5TestClient sub;
    ok &= sub.connect_raw(port, "m72b-will-sub", 0x05).ok;
    auto suback = sub.subscribe_v5("m72b/will", /*qos=*/1, /*is_v5=*/true);
    ok &= suback.has_value();

    auto got = sub.wait_publish_v5_extras();
    ok &= got.has_value() && got->topic == "m72b/will" && got->payload == "will-payload";
    ok &= got.has_value() && got->props.response_topic.has_value() &&
          *got->props.response_topic == "m72b/will-reply";
    ok &= got.has_value() && got->props.user_properties.size() == 1;
    if (got.has_value() && got->props.user_properties.size() == 1) {
        ok &= got->props.user_properties[0].first == "reason" &&
              got->props.user_properties[0].second == "abrupt-disconnect";
    }

    sub.close();
    broker.stop();
    return ok;
}

// ===== 017 M7.2 PR C coverage: outbound Topic Alias (broker -> subscriber compression) ================
// A v5 subscriber that advertises Topic Alias Maximum=2 (0x22) in its own CONNECT gets its FIRST
// delivery for each of up to 2 distinct topics with the full topic name AND a freshly-assigned Topic
// Alias (establishing); every SUBSEQUENT delivery for an already-aliased topic gets an EMPTY topic name +
// the SAME alias (reuse — the actual compression, mirrors test_topic_alias_inbound's client->broker
// direction). A THIRD distinct topic, once both alias slots are taken, falls back to a full topic name
// with NO alias (a legal uncompressed delivery, never an error). A control subscriber that never
// advertised a Topic Alias Maximum — the CONNECT shape every other v5 test in this file already uses —
// proves §3.1.2.11.2's "absent => never send me one": every delivery to it keeps its full topic name and
// no alias, unregressed.
bool test_topic_alias_outbound() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    std::vector<std::byte> sub_connect_props;
    put_prop_u16(sub_connect_props, 0x22, 2);  // Topic Alias Maximum = 2
    V5TestClient sub;
    ok &= sub.connect_raw(port, "alias-out-sub", 0x05, sub_connect_props).ok;
    ok &= sub.subscribe_v5("out/#", /*qos=*/1, /*is_v5=*/true).has_value();

    V5TestClient ctrl;  // no Topic Alias Maximum advertised
    ok &= ctrl.connect_raw(port, "alias-out-ctrl", 0x05).ok;
    ok &= ctrl.subscribe_v5("out/#", /*qos=*/1, /*is_v5=*/true).has_value();

    V5TestClient pub;
    ok &= pub.connect_raw(port, "alias-out-pub", 0x05).ok;

    // 1st delivery of "out/a" — establishes alias 1: full topic name + Topic Alias property.
    ok &= pub.publish_v5("out/a", "p1", /*qos=*/1, /*retain=*/false, /*is_v5=*/true);
    auto g1 = sub.wait_publish_v5_extras();
    ok &= g1.has_value() && g1->topic == "out/a" && g1->payload == "p1" &&
          g1->props.topic_alias.has_value() && *g1->props.topic_alias == 1;
    auto gc1 = ctrl.wait_publish_v5_extras();
    ok &= gc1.has_value() && gc1->topic == "out/a" && !gc1->props.topic_alias.has_value();

    // 2nd delivery of "out/a" — reuses alias 1: EMPTY topic name + the SAME Topic Alias property.
    ok &= pub.publish_v5("out/a", "p2", /*qos=*/1, /*retain=*/false, /*is_v5=*/true);
    auto g2 = sub.wait_publish_v5_extras();
    ok &= g2.has_value() && g2->topic.empty() && g2->payload == "p2" &&
          g2->props.topic_alias.has_value() && *g2->props.topic_alias == 1;
    auto gc2 = ctrl.wait_publish_v5_extras();
    ok &= gc2.has_value() && gc2->topic == "out/a" && !gc2->props.topic_alias.has_value();

    // 1st delivery of "out/b" — a NEW topic, second (and last, max=2) alias slot: full topic + alias 2.
    ok &= pub.publish_v5("out/b", "p3", /*qos=*/1, /*retain=*/false, /*is_v5=*/true);
    auto g3 = sub.wait_publish_v5_extras();
    ok &= g3.has_value() && g3->topic == "out/b" && g3->payload == "p3" &&
          g3->props.topic_alias.has_value() && *g3->props.topic_alias == 2;

    // 1st delivery of "out/c" — a THIRD distinct topic, both alias slots already taken: full topic name,
    // NO alias (legal uncompressed fallback, not an error).
    ok &= pub.publish_v5("out/c", "p4", /*qos=*/1, /*retain=*/false, /*is_v5=*/true);
    auto g4 = sub.wait_publish_v5_extras();
    ok &= g4.has_value() && g4->topic == "out/c" && g4->payload == "p4" &&
          !g4->props.topic_alias.has_value();

    pub.close();
    sub.close();
    ctrl.close();
    broker.stop();
    return ok;
}

// ===== 017 M7.2 PR D coverage: Shared Subscriptions (`$share/<group>/<filter>`, MQTT 5 §4.8.2) =========
// Two subscribers join the SAME shared-subscription group+filter — each PUBLISH goes to exactly ONE
// member (round-robin — NOT necessarily subA-then-subB: route_publish()'s candidate order comes from
// topic_index_candidates(), which sorts by shared_ptr<Session> IDENTITY, not SUBSCRIBE order, so which
// member gets picked FIRST is an unpredictable implementation detail. This test discovers that winner
// from the first fresh delivery, then asserts strict alternation from there on — the property under test
// is "exactly one member per message, and it keeps rotating," not "which physical client goes first").
// A THIRD, ORDINARY (non-shared) subscriber to the same topic proves regular fan-out is completely
// unaffected — it gets every single message, unlike the two shared members who only ever get every other
// one. A retained message published BEFORE either shared member subscribes is deliberately NOT replayed to
// them on SUBSCRIBE (§4.8.2's own "no guarantee" — see handle_subscribe's retained-replay loop comment);
// the ordinary subscriber still gets its usual immediate replay, proving that's not accidentally broken.
bool test_shared_subscription_round_robin() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    V5TestClient pub;
    ok &= pub.connect_raw(port, "share-pub", 0x05).ok;
    // Retained message published BEFORE anyone subscribes — establishes the "already retained" precondition.
    ok &= pub.publish_v5("share/work", "stale-retained", /*qos=*/1, /*retain=*/true, /*is_v5=*/true);

    V5TestClient subA;
    ok &= subA.connect_raw(port, "share-a", 0x05).ok;
    ok &= subA.subscribe_v5("$share/grp/share/work", /*qos=*/1, /*is_v5=*/true).has_value();
    ok &= !subA.wait_publish(500).has_value();  // shared member: no immediate retained replay

    V5TestClient subB;
    ok &= subB.connect_raw(port, "share-b", 0x05).ok;
    ok &= subB.subscribe_v5("$share/grp/share/work", /*qos=*/1, /*is_v5=*/true).has_value();
    ok &= !subB.wait_publish(500).has_value();

    V5TestClient ctrl;  // ordinary (non-shared) subscriber to the same topic
    ok &= ctrl.connect_raw(port, "share-ctrl", 0x05).ok;
    ok &= ctrl.subscribe_v5("share/work", /*qos=*/1, /*is_v5=*/true).has_value();
    auto retained_replay = ctrl.wait_publish();  // ordinary subscriber DOES get it, unregressed
    ok &= retained_replay.has_value() && retained_replay->second == "stale-retained";

    // 4 fresh publishes: round-robin distributes them alternately across the two shared members, in an
    // order this test discovers from the first delivery rather than assumes; ctrl gets all 4 regardless.
    V5TestClient* members[2] = {&subA, &subB};
    int winner_idx = -1;  // index into `members` of whoever got message 0 — set on the first iteration
    for (int i = 0; i < 4; ++i) {
        const std::string payload = "m" + std::to_string(i);
        ok &= pub.publish_v5("share/work", payload, /*qos=*/1, /*retain=*/false, /*is_v5=*/true);
        auto c = ctrl.wait_publish();
        ok &= c.has_value() && c->second == payload;

        if (winner_idx < 0) {
            auto a = subA.wait_publish(1500);
            if (a.has_value()) {
                ok &= a->second == payload;
                ok &= !subB.wait_publish(500).has_value();
                winner_idx = 0;
            } else {
                auto b = subB.wait_publish(500);
                ok &= b.has_value() && b->second == payload;
                winner_idx = 1;
            }
            continue;
        }
        const int expected = (winner_idx + i) % 2;
        auto got = members[expected]->wait_publish(1500);
        ok &= got.has_value() && got->second == payload;
        ok &= !members[1 - expected]->wait_publish(500).has_value();
    }

    pub.close();
    subA.close();
    subB.close();
    ctrl.close();
    broker.stop();
    return ok;
}

// A structurally malformed Shared Subscription filter — missing ShareName, missing TopicFilter after the
// ShareName, or a ShareName containing a wildcard character — is a Protocol Error (§4.8.2): the whole
// SUBSCRIBE is rejected via DISCONNECT 0x82 (never a SUBACK), mirroring
// test_response_topic_wildcard_rejected's own Protocol Error pattern on the PUBLISH side.
bool test_shared_subscription_malformed_protocol_error() {
    bool ok = true;
    NativeBroker broker(Config{"127.0.0.1", /*listen_port=*/0});
    ok &= broker.start().has_value();
    const std::uint16_t port = broker.listen_port();

    for (const std::string& bad_filter : {std::string("$share/"), std::string("$share/g"),
                                          std::string("$share/g+r/topic")}) {
        V5TestClient c;
        ok &= c.connect_raw(port, "share-bad", 0x05).ok;
        auto ack = c.subscribe_v5(bad_filter, /*qos=*/1, /*is_v5=*/true);
        ok &= !ack.has_value();  // no SUBACK — the SUBSCRIBE was rejected outright
        auto dc = c.wait_for_packet(0xE0, 1000);
        ok &= dc.has_value() && !dc->empty() && std::to_integer<std::uint8_t>((*dc)[0]) == 0x82;
        c.close();
    }
    broker.stop();
    return ok;
}

}  // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"test_properties_codec", test_properties_codec},
        {"test_v5_connect_roundtrip", test_v5_connect_roundtrip},
        {"test_unsupported_protocol_version", test_unsupported_protocol_version},
        {"test_v5_publish_properties_roundtrip", test_v5_publish_properties_roundtrip},
        {"test_v5_subscribe_with_subscription_identifier", test_v5_subscribe_with_subscription_identifier},
        {"test_v5_subscribe_denied", test_v5_subscribe_denied},
        {"test_session_expiry_ttl_enforced", test_session_expiry_ttl_enforced},
        {"test_session_expiry_absent_no_regression", test_session_expiry_absent_no_regression},
        {"test_disconnect_reason_keep_alive_timeout", test_disconnect_reason_keep_alive_timeout},
        {"test_disconnect_reason_keep_alive_timeout_v4_no_regression",
         test_disconnect_reason_keep_alive_timeout_v4_no_regression},
        {"test_disconnect_reason_session_taken_over", test_disconnect_reason_session_taken_over},
        {"test_topic_alias_inbound", test_topic_alias_inbound},
        {"test_topic_alias_outbound", test_topic_alias_outbound},
        {"test_message_expiry_delivered_when_fresh", test_message_expiry_delivered_when_fresh},
        {"test_message_expiry_dropped_when_stale", test_message_expiry_dropped_when_stale},
        {"test_max_packet_size_enforced", test_max_packet_size_enforced},
        {"test_will_message_expiry_honored", test_will_message_expiry_honored},
        {"test_response_topic_and_correlation_data_roundtrip",
         test_response_topic_and_correlation_data_roundtrip},
        {"test_user_properties_roundtrip", test_user_properties_roundtrip},
        {"test_response_topic_survives_retained_replay", test_response_topic_survives_retained_replay},
        {"test_response_topic_wildcard_rejected", test_response_topic_wildcard_rejected},
        {"test_will_response_topic_and_user_properties_honored",
         test_will_response_topic_and_user_properties_honored},
        {"test_shared_subscription_round_robin", test_shared_subscription_round_robin},
        {"test_shared_subscription_malformed_protocol_error",
         test_shared_subscription_malformed_protocol_error},
    };
    bool ok = true;
    for (const auto& t : tests) {
        const bool r = t.fn();
        std::printf("%s: %s\n", t.name, r ? "OK" : "FAIL");
        ok &= r;
    }

    std::printf("mqtt5: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
