// AeroEdge Transport — `GrpcClientTransport`, a REAL gRPC-over-HTTP/2 client behind Quark's `Transport`
// seam (014 §6, X2). This is the concrete backend the offline `NullGrpcTransport` stands in for: a hand-
// written HTTP/2 client (connection preface, SETTINGS exchange, a single bidi stream opened with a
// minimal-HPACK HEADERS block, DATA frames carrying gRPC length-delimited messages, PING/WINDOW_UPDATE
// housekeeping) that moves genuine `MessageFrame`s over an ACTUAL gRPC server. No gRPC/protoc/HTTP2
// library is linked — every byte of the framing is produced here — and the test points it at a real
// `grpcio` server (tests/py/grpc_echo_server.py) launched via uv.
//
// THE §6 DESIGN, now realized:
//   * ONE bidirectional HTTP/2 stream per peer carries MessageFrames (mirrors Quark's one-connection-per-
//     peer TCP model). HTTP/2 stream ordering gives per-`(from → target)` FIFO (C1) on a single stream
//     for FREE — so, UNLIKE MqttTransport, this needs NO resequencer: the substrate is itself order-
//     preserving and flow-controlled. That is the deliberate design choice (single stream per peer).
//   * A gRPC message == [1-byte compression flag = 0][4-byte big-endian length][message], the message
//     being one `encode_frame` body. Sent as HTTP/2 DATA on the stream; reassembled on receive.
//   * mTLS would satisfy C5 in a real build; the Quark 020 Principal already rides IN the frame (X6).
//     This reference client speaks h2c (cleartext HTTP/2) to keep the offline test dependency-free —
//     TLS is a real-build concern, called out but not implemented (honest scope).
//   * Backpressure (X7 / §7): HTTP/2 flow control makes gRPC a flow-controlled transport (stall-not-drop),
//     like TCP and unlike a broker — good for high-rate cross-node streams. (Posture, not stressed here.)
//
// SCOPE (honest): a minimal but correct HTTP/2 client — enough of the protocol to open one bidi gRPC
// stream and exchange messages reliably with a conforming server on a healthy link. HPACK is ENCODE-ONLY
// and uses literal-header-field-without-indexing with no Huffman and no dynamic table (spec-legal); the
// client never DECODES inbound HPACK (it skips HEADERS/trailer blocks entirely — it needs only the DATA),
// so there is no HPACK decoder state to keep. It does not implement TLS, multi-stream multiplexing, or
// full flow-control accounting beyond keeping windows open with WINDOW_UPDATE. The parts this phase is
// about — real HTTP/2 framing, a real gRPC message stream, single-stream FIFO — are real and exercised.
//
// OFF THE HOT PATH (R0): optional aero-transport backend; std threads/containers, no 0-alloc discipline.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aero/transport/frame_codec.hpp"
#include "aero/transport/grpc_transport.hpp"  // GrpcTransport base (Config/shape) + (via mqtt) kTransportGate
#include "aero/transport/transport.hpp"

namespace aero::transport {

class GrpcClientTransport final : public GrpcTransport {
public:
    explicit GrpcClientTransport(Config cfg = {}) : GrpcTransport(std::move(cfg)) {}
    ~GrpcClientTransport() override { stop(); }

    GrpcClientTransport(const GrpcClientTransport&) = delete;
    GrpcClientTransport& operator=(const GrpcClientTransport&) = delete;

    // Dial the server, complete the HTTP/2 preface + SETTINGS, open the bidi gRPC stream, spawn the
    // reader. Set on_receive() BEFORE start(). Returns the documented error on failure (fail-closed).
    [[nodiscard]] std::expected<void, std::string> start() override {
        std::string host;
        std::uint16_t port = 0;
        if (!parse_target(cfg_.target, host, port))
            return gate_err("malformed grpc target '" + cfg_.target + "'");
        authority_ = host + ":" + std::to_string(port);

        fd_ = dial(host, port);
        if (fd_ < 0) return gate_err("cannot dial grpc server " + authority_);
        running_.store(true, std::memory_order_release);

        // (1) client connection preface, then (2) an (empty) client SETTINGS frame.
        static constexpr char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        if (!write_raw(reinterpret_cast<const std::byte*>(kPreface), sizeof(kPreface) - 1))
            return gate_err("preface write failed");
        if (!write_frame(kSettings, 0, 0, {})) return gate_err("SETTINGS write failed");

        // (3) open stream 1 with the gRPC request HEADERS (END_HEADERS, NOT END_STREAM — bidi stays open).
        if (!write_headers()) return gate_err("HEADERS write failed");

        reader_thread_ = std::thread([this] { reader_loop(); });
        return {};
    }

    // Half-close our send side (empty DATA + END_STREAM), stop the reader, close the socket, join.
    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (fd_ >= 0) (void)write_frame(kData, kEndStream, kStreamId, {});  // best-effort half-close
        if (reader_thread_.joinable()) reader_thread_.join();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // --- Transport seam (010): send one frame as a gRPC message (HTTP/2 DATA) on the bidi stream -------
    void send(NodeId, MessageFrame frame) override {
        const std::vector<std::byte> body = encode_frame(frame);
        // gRPC message framing: [compressed=0][u32 BE length][message bytes].
        std::vector<std::byte> msg;
        msg.reserve(5 + body.size());
        msg.push_back(std::byte{0x00});
        put_u32_be(msg, static_cast<std::uint32_t>(body.size()));
        msg.insert(msg.end(), body.begin(), body.end());
        if (write_frame(kData, 0, kStreamId, msg))
            frames_sent_.fetch_add(1, std::memory_order_relaxed);
        else
            send_errors_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_receive(std::function<void(MessageFrame)> cb) override { cb_ = std::move(cb); }

    // --- diagnostics -----------------------------------------------------------------------------------
    [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_recv_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t send_errors() const noexcept { return send_errors_.load(std::memory_order_relaxed); }

private:
    // HTTP/2 frame types + flags used here.
    static constexpr std::uint8_t kData = 0x0, kHeaders = 0x1, kRstStream = 0x3, kSettings = 0x4,
                                  kPing = 0x6, kGoAway = 0x7, kWindowUpdate = 0x8, kContinuation = 0x9;
    static constexpr std::uint8_t kEndStream = 0x1, kEndHeaders = 0x4, kPadded = 0x8, kAck = 0x1;
    static constexpr std::uint32_t kStreamId = 1;  // single client-initiated stream (odd id)

    // ===== byte helpers ================================================================================
    static void put_u32_be(std::vector<std::byte>& out, std::uint32_t v) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
    static std::uint32_t get_u32_be(const std::byte* p) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | std::to_integer<std::uint8_t>(p[i]);
        return v;
    }

    std::unexpected<std::string> gate_err(std::string_view what) {
        running_.store(false, std::memory_order_release);
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        return std::unexpected(std::string(kTransportGate) + " [grpc: " + std::string(what) + "]");
    }

    // ===== connection ==================================================================================
    // Accept "dns:///host:port", "host:port", or "ipv4:host:port"; extract host + port.
    static bool parse_target(std::string_view t, std::string& host, std::uint16_t& port) {
        for (std::string_view pfx : {std::string_view("dns:///"), std::string_view("ipv4:"),
                                     std::string_view("dns:")}) {
            if (t.substr(0, pfx.size()) == pfx) { t.remove_prefix(pfx.size()); break; }
        }
        const auto colon = t.rfind(':');
        if (colon == std::string_view::npos) return false;
        host = std::string(t.substr(0, colon));
        const std::string ps(t.substr(colon + 1));
        if (host.empty() || ps.empty()) return false;
        port = static_cast<std::uint16_t>(std::stoul(ps));
        return port != 0;
    }

    static int dial(const std::string& host, std::uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return -1;
        int fd = -1;
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                int one = 1;
                ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                break;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(res);
        return fd;
    }

    // ===== raw socket I/O ==============================================================================
    bool write_raw(const std::byte* buf, std::size_t n) {
        std::lock_guard<std::mutex> g(io_mu_);
        if (fd_ < 0) return false;
        std::size_t sent = 0;
        while (sent < n) {
            const ssize_t w = ::send(fd_, buf + sent, n - sent, MSG_NOSIGNAL);
            if (w < 0) { if (errno == EINTR) continue; return false; }
            sent += static_cast<std::size_t>(w);
        }
        return true;
    }

    bool read_n(std::byte* buf, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            if (!running_.load(std::memory_order_acquire)) return false;
            pollfd pfd{fd_, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr < 0) { if (errno == EINTR) continue; return false; }
            if (pr == 0) continue;
            const ssize_t r = ::recv(fd_, buf + got, n - got, 0);
            if (r == 0) return false;
            if (r < 0) { if (errno == EINTR) continue; return false; }
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    // ===== HTTP/2 frame writer =========================================================================
    // 9-byte frame header (length:24 | type:8 | flags:8 | stream:32) + payload, written atomically.
    bool write_frame(std::uint8_t type, std::uint8_t flags, std::uint32_t stream_id,
                     std::span<const std::byte> payload) {
        std::vector<std::byte> f;
        f.reserve(9 + payload.size());
        const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
        f.push_back(static_cast<std::byte>((len >> 16) & 0xFF));
        f.push_back(static_cast<std::byte>((len >> 8) & 0xFF));
        f.push_back(static_cast<std::byte>(len & 0xFF));
        f.push_back(static_cast<std::byte>(type));
        f.push_back(static_cast<std::byte>(flags));
        f.push_back(static_cast<std::byte>((stream_id >> 24) & 0x7F));  // R bit = 0
        f.push_back(static_cast<std::byte>((stream_id >> 16) & 0xFF));
        f.push_back(static_cast<std::byte>((stream_id >> 8) & 0xFF));
        f.push_back(static_cast<std::byte>(stream_id & 0xFF));
        f.insert(f.end(), payload.begin(), payload.end());
        return write_raw(f.data(), f.size());
    }

    // ===== HPACK encode (literal header field without indexing, no Huffman, no dynamic table) ==========
    static void put_hpack_str(std::vector<std::byte>& out, std::string_view s) {
        out.push_back(static_cast<std::byte>(s.size() & 0x7F));  // H=0, length (all our strings < 127)
        for (char c : s) out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    }
    static void put_hpack_kv(std::vector<std::byte>& out, std::string_view name, std::string_view value) {
        out.push_back(std::byte{0x00});  // 0b0000 = literal without indexing, new name
        put_hpack_str(out, name);
        put_hpack_str(out, value);
    }

    bool write_headers() {
        std::vector<std::byte> hb;
        put_hpack_kv(hb, ":method", "POST");
        put_hpack_kv(hb, ":scheme", "http");
        put_hpack_kv(hb, ":path", "/aero.Transport/Exchange");
        put_hpack_kv(hb, ":authority", authority_);
        put_hpack_kv(hb, "content-type", "application/grpc");
        put_hpack_kv(hb, "te", "trailers");
        return write_frame(kHeaders, kEndHeaders, kStreamId, hb);  // END_HEADERS, keep stream open
    }

    // ===== inbound loop ================================================================================
    void reader_loop() {
        while (running_.load(std::memory_order_acquire)) {
            std::byte hdr[9];
            if (!read_n(hdr, 9)) break;
            const std::uint32_t len = (std::to_integer<std::uint8_t>(hdr[0]) << 16) |
                                      (std::to_integer<std::uint8_t>(hdr[1]) << 8) |
                                      std::to_integer<std::uint8_t>(hdr[2]);
            const std::uint8_t type = std::to_integer<std::uint8_t>(hdr[3]);
            const std::uint8_t flags = std::to_integer<std::uint8_t>(hdr[4]);
            std::vector<std::byte> payload(len);
            if (len > 0 && !read_n(payload.data(), len)) break;

            switch (type) {
                case kSettings:
                    if (!(flags & kAck)) (void)write_frame(kSettings, kAck, 0, {});  // ACK server SETTINGS
                    break;
                case kPing:
                    if (!(flags & kAck)) (void)write_frame(kPing, kAck, 0, payload);  // echo PING as ACK
                    break;
                case kData:
                    handle_data(flags, payload);
                    break;
                case kGoAway:
                case kRstStream:
                    running_.store(false, std::memory_order_release);  // server ended the stream/conn
                    break;
                case kHeaders:
                case kContinuation:
                case kWindowUpdate:
                default:
                    break;  // response headers/trailers skipped (we need only DATA); flow-control ignored
            }
        }
    }

    // Strip HTTP/2 DATA padding, append to the reassembly buffer, and pull out every complete gRPC
    // message ([1-byte flag][u32 BE len][body]); decode each body to a MessageFrame and emit it. A single
    // ordered stream ⇒ frames arrive in send order (C1) with no resequencer.
    void handle_data(std::uint8_t flags, const std::vector<std::byte>& payload) {
        std::size_t begin = 0, end = payload.size();
        if (flags & kPadded) {
            if (payload.empty()) return;
            const std::size_t pad = std::to_integer<std::uint8_t>(payload[0]);
            begin = 1;
            if (pad > end - begin) return;  // malformed padding → drop the frame
            end -= pad;
        }
        recv_buf_.insert(recv_buf_.end(), payload.begin() + static_cast<std::ptrdiff_t>(begin),
                         payload.begin() + static_cast<std::ptrdiff_t>(end));

        // Keep the server's receive window open so it can send more echoes (robust for larger volumes).
        if (end > begin) {
            std::vector<std::byte> incr;
            put_u32_be(incr, static_cast<std::uint32_t>(end - begin));
            (void)write_frame(kWindowUpdate, 0, 0, incr);          // connection window
            (void)write_frame(kWindowUpdate, 0, kStreamId, incr);  // stream window
        }

        std::size_t off = 0;
        while (recv_buf_.size() - off >= 5) {
            const std::uint32_t mlen = get_u32_be(recv_buf_.data() + off + 1);
            if (recv_buf_.size() - off - 5 < mlen) break;  // message not fully arrived yet
            std::span<const std::byte> mbytes(recv_buf_.data() + off + 5, mlen);
            if (auto f = decode_frame(mbytes)) {
                frames_recv_.fetch_add(1, std::memory_order_relaxed);
                if (cb_) cb_(std::move(*f));
            }
            off += 5 + mlen;
        }
        if (off > 0) recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + static_cast<std::ptrdiff_t>(off));
    }

    int fd_ = -1;
    std::string authority_;
    std::atomic<bool> running_{false};
    std::function<void(MessageFrame)> cb_;  // set once, before start()

    std::thread reader_thread_;
    std::mutex io_mu_;                 // serializes all socket writes (frame atomicity)
    std::vector<std::byte> recv_buf_;  // gRPC message reassembly (reader thread only — no lock needed)

    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_recv_{0};
    std::atomic<std::uint64_t> send_errors_{0};
};

}  // namespace aero::transport
