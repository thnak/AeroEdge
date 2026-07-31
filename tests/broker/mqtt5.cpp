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
// NOTE on outbound PUBLISH framing: this milestone's PUBLISH scope cut is INBOUND parsing only
// (handle_publish reads and discards a v5 publisher's Properties block) — the broker's existing
// publish_to() (broker -> subscriber) is untouched by M7 and keeps its pre-M7 wire shape (Topic,
// [Packet Id], Payload, no Properties field) for every subscriber regardless of protocol version. So
// V5TestClient::wait_publish() below deliberately parses inbound PUBLISHes the same way the v4
// TestClient in native_broker.cpp already does — this is not an oversight, it matches the brief's
// actual in-scope task list (protocol negotiation + CONNECT/Will/PUBLISH/SUBSCRIBE properties PARSING,
// not a general properties WRITER for every outbound packet).
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

    [[nodiscard]] RawAck connect_raw(std::uint16_t port, const std::string& client_id,
                                      std::uint8_t protocol_level,
                                      const std::vector<std::byte>& connect_props_records = {},
                                      bool clean_session = true, std::uint16_t keep_alive_s = 60) {
        RawAck result;
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
        std::vector<std::byte> vh;
        mqtt::put_str(vh, "MQTT");
        vh.push_back(static_cast<std::byte>(protocol_level));
        vh.push_back(static_cast<std::byte>(flags));
        mqtt::put_u16_be(vh, keep_alive_s);
        if (protocol_level == 0x05) put_properties(vh, connect_props_records);
        mqtt::put_str(vh, client_id);
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

    // See this file's banner: the broker's outbound PUBLISH framing is untouched by M7 (no Properties
    // field for any subscriber, v4 or v5) — this parses the SAME shape native_broker.cpp's TestClient
    // does, deliberately.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> wait_publish(int timeout_ms = 1500) {
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
        std::string payload(reinterpret_cast<const char*>(b.data() + pos), b.size() - pos);
        return std::make_pair(std::move(topic), std::move(payload));
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
};

// ===== unit coverage: the Properties codec itself, no socket involved ==================================
// Round-trips a CONNECT-shaped properties block: Session Expiry Interval (0x11) + two User Properties
// (0x26, repeatable) + Topic Alias (0x23, M7.1) — proves read_properties surfaces BOTH values this codec
// stores AND advances `pos` exactly past every record, including the ones it doesn't store. A two-byte
// sentinel appended after the properties block confirms `pos` lands exactly at the boundary, not short
// or long. (Topic Alias doesn't legally co-occur with Session Expiry Interval in a real CONNECT — it's a
// PUBLISH-only property — but read_properties() itself is packet-type-agnostic, so this is a fair
// same-codepath test of the codec in isolation.)
bool test_properties_codec() {
    bool ok = true;
    std::vector<std::byte> records;
    put_prop_u32(records, 0x11, 3600);                     // Session Expiry Interval
    put_prop_str_pair(records, 0x26, "k1", "v1");           // User Property #1
    put_prop_str_pair(records, 0x26, "k2", "v2");           // User Property #2
    put_prop_u16(records, 0x23, 42);                        // Topic Alias (M7.1)

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
// correctly resolved topic (outbound PUBLISH framing is untouched by this milestone, see this file's
// banner, so V5TestClient::wait_publish() needs no changes). A fresh connection that never established
// an alias (or sends alias value 0, which is always invalid) gets 0xE0/0x94 DISCONNECT and nothing is
// ever delivered.
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
