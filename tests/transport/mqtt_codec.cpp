// AeroEdge 017 Phase 3 gate: `try_parse_packet()` (aero/transport/mqtt_codec.hpp), the buffered-read
// counterpart to read_packet()/read_n() that NativeBroker::session_loop() now drives directly instead of
// doing one recv_some()-or-poll cycle per byte (017-Native-Broker-Performance-Redesign.md §2.4
// Experiment A / §3.1). Pure function, no sockets — this is Phase 1's test-coverage gap #4
// ("multi-packet-per-recv() burst framing — untested") closed at the unit level, proven in isolation
// BEFORE it drives the broker's only ingestion path (see the redesign doc's §3.0 sequencing rationale:
// prove the parser correct standalone first).
//
// Deterministic, exit-code-gated (0 = pass), no sockets, no threads.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "aero/transport/mqtt_codec.hpp"

namespace mqtt = aero::transport::mqtt;

namespace {

bool g_ok = true;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        g_ok = false;
    }
}

std::vector<std::byte> bytes(std::initializer_list<int> vals) {
    std::vector<std::byte> out;
    out.reserve(vals.size());
    for (int v : vals) out.push_back(static_cast<std::byte>(v));
    return out;
}

// One well-formed CONNECT-shaped packet for reuse across cases: type_flags=0x10 (CONNECT), a 5-byte
// body. Remaining length 5 encodes as a single byte (no continuation), matching put_remaining_length()'s
// own encoding for len < 128.
std::vector<std::byte> one_packet() { return bytes({0x10, 0x05, 'h', 'e', 'l', 'l', 'o'}); }

}  // namespace

int main() {
    // --- empty buffer: Incomplete, pos unchanged ------------------------------------------------------
    {
        std::vector<std::byte> buf;
        std::size_t pos = 0;
        auto r = mqtt::try_parse_packet(buf, pos);
        expect(!r.has_value() && r.error() == mqtt::ParseStatus::Incomplete, "empty buffer -> Incomplete");
        expect(pos == 0, "empty buffer leaves pos unchanged");
    }

    // --- partial fixed header (zero bytes at all is covered above; this is "buffer has ONLY the fixed
    //     header byte, nothing else yet") -------------------------------------------------------------
    {
        std::vector<std::byte> buf = bytes({0x10});
        std::size_t pos = 0;
        auto r = mqtt::try_parse_packet(buf, pos);
        expect(!r.has_value() && r.error() == mqtt::ParseStatus::Incomplete,
               "fixed header only -> Incomplete");
        expect(pos == 0, "partial fixed header leaves pos unchanged");
    }

    // --- partial remaining-length varint: fixed header + a continuation byte (high bit set) but no
    //     terminator byte yet ------------------------------------------------------------------------
    {
        std::vector<std::byte> buf = bytes({0x10, 0x80});  // 0x80: continuation bit set, more expected
        std::size_t pos = 0;
        auto r = mqtt::try_parse_packet(buf, pos);
        expect(!r.has_value() && r.error() == mqtt::ParseStatus::Incomplete,
               "partial (continuing) remaining-length varint -> Incomplete");
        expect(pos == 0, "partial varint leaves pos unchanged");
    }

    // --- fixed header + complete 1-byte remaining-length (value 5) but body not yet buffered ----------
    {
        std::vector<std::byte> buf = bytes({0x10, 0x05});
        std::size_t pos = 0;
        auto r = mqtt::try_parse_packet(buf, pos);
        expect(!r.has_value() && r.error() == mqtt::ParseStatus::Incomplete,
               "complete varint, zero body bytes buffered -> Incomplete");
        expect(pos == 0, "pos unchanged when body isn't buffered yet");
    }

    // --- partial body: some but not all of the 5 declared body bytes are present ----------------------
    {
        std::vector<std::byte> buf = bytes({0x10, 0x05, 'h', 'e'});  // only 2 of 5 body bytes
        std::size_t pos = 0;
        auto r = mqtt::try_parse_packet(buf, pos);
        expect(!r.has_value() && r.error() == mqtt::ParseStatus::Incomplete, "partial body -> Incomplete");
        expect(pos == 0, "partial body leaves pos unchanged");
    }

    // --- exactly one complete packet, nothing more in the buffer --------------------------------------
    {
        std::vector<std::byte> buf = one_packet();
        std::size_t pos = 0;
        auto r = mqtt::try_parse_packet(buf, pos);
        expect(r.has_value(), "exactly-one-packet parses successfully");
        if (r) {
            expect(r->type_flags == 0x10, "exactly-one-packet: type_flags correct");
            expect(r->body.size() == 5, "exactly-one-packet: body length correct");
            expect(std::memcmp(r->body.data(), "hello", 5) == 0, "exactly-one-packet: body bytes correct");
        }
        expect(pos == buf.size(), "pos advances past the whole packet");

        // A second call at the now-fully-consumed position must report Incomplete, not re-parse/loop.
        auto r2 = mqtt::try_parse_packet(buf, pos);
        expect(!r2.has_value() && r2.error() == mqtt::ParseStatus::Incomplete,
               "re-parsing at end of buffer -> Incomplete, not a phantom packet");
    }

    // --- two-or-more packets buffered at once (the actual "burst" scenario a buffered reader creates,
    //     e.g. a fast publisher whose bytes all arrive in one recv_some() burst) ----------------------
    {
        std::vector<std::byte> buf = one_packet();
        std::vector<std::byte> second = bytes({0x30, 0x02, 'h', 'i'});  // a PUBLISH-shaped 2nd packet
        buf.insert(buf.end(), second.begin(), second.end());

        std::size_t pos = 0;
        auto r1 = mqtt::try_parse_packet(buf, pos);
        expect(r1.has_value() && r1->type_flags == 0x10, "burst: first packet parses (CONNECT-shaped)");
        expect(pos == 7, "burst: pos sits exactly at the second packet's start after the first parse");

        auto r2 = mqtt::try_parse_packet(buf, pos);
        expect(r2.has_value() && r2->type_flags == 0x30, "burst: second packet parses (PUBLISH-shaped)");
        expect(r2.has_value() && r2->body.size() == 2 && std::memcmp(r2->body.data(), "hi", 2) == 0,
               "burst: second packet's body correct");
        expect(pos == buf.size(), "burst: pos advances past both packets");

        auto r3 = mqtt::try_parse_packet(buf, pos);
        expect(!r3.has_value() && r3.error() == mqtt::ParseStatus::Incomplete,
               "burst: no phantom third packet");
    }

    // --- zero-length body (legal — e.g. a PINGREQ-shaped packet, remaining length 0) ------------------
    {
        std::vector<std::byte> buf = bytes({0xC0, 0x00});
        std::size_t pos = 0;
        auto r = mqtt::try_parse_packet(buf, pos);
        expect(r.has_value(), "zero-length body parses successfully");
        if (r) {
            expect(r->type_flags == 0xC0, "zero-length body: type_flags correct");
            expect(r->body.empty(), "zero-length body: body is empty, not garbage");
        }
        expect(pos == 2, "zero-length body: pos advances past exactly the 2-byte header");
    }

    // --- malformed remaining-length varint: 5 continuation bytes, exceeding MQTT's own 4-byte cap
    //     (§1.5.5) — every one of the 4 checked bytes has its continuation bit set, meaning a real
    //     encoder would need a 5th byte, which the spec forbids -----------------------------------------
    {
        std::vector<std::byte> buf = bytes({0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});  // 5 bytes, all continuing
        std::size_t pos = 0;
        auto r = mqtt::try_parse_packet(buf, pos);
        expect(!r.has_value() && r.error() == mqtt::ParseStatus::Malformed,
               "5-continuation-byte varint -> Malformed");
        expect(pos == 0, "malformed varint leaves pos unchanged");
    }

    // --- a large remaining length spanning multiple varint bytes, parsed correctly end to end ---------
    {
        // 300 encodes as [0xAC, 0x02] per MQTT's varint encoding (300 = 0x2C + 0x80 continuation, then 0x02).
        std::vector<std::byte> buf = bytes({0x30, 0xAC, 0x02});
        std::vector<std::byte> body(300, std::byte{0x41});  // 300 'A' bytes
        buf.insert(buf.end(), body.begin(), body.end());

        std::size_t pos = 0;
        auto r = mqtt::try_parse_packet(buf, pos);
        expect(r.has_value(), "multi-byte remaining-length parses successfully");
        if (r) {
            expect(r->body.size() == 300, "multi-byte remaining-length: body size correct");
        }
        expect(pos == buf.size(), "multi-byte remaining-length: pos advances past the whole packet");
    }

    std::printf("mqtt_codec: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
