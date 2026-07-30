// AeroEdge Transport — `TcpTransport`, a REAL socket adapter over Quark's `Transport` seam (014 §4, X1).
//
// WHAT THIS IS: the concrete, wire-carrying realization of "Quark's coordinator-free default fabric"
// (014 §4 B2 / §7 Tcp). Unlike the MQTT/gRPC adapters — whose backends need an external broker / gRPC
// stack and so ship as honest offline gates — TCP needs NOTHING but sockets, which are always present.
// So this adapter is fully real and runs end-to-end offline: two nodes on distinct NodeIds exchange
// genuine `MessageFrame`s over loopback (or any) TCP, and the phase-7 §2 cross-node path (tested
// in-process over Quark's LoopbackTransport) is here proven over an actual socket.
//
// THE MODEL (mirrors Quark 019/021's documented design so this is a faithful stand-in, not a divergence):
//   * ONE multiplexed connection per peer, length-prefixed frames (021 §"framing"). A u32 little-endian
//     length precedes each `encode_frame` body; the reader reassembles exact frame boundaries off the
//     byte stream.
//   * Per-`(from → target)` FIFO (C1) for FREE: a single ordered TCP connection per peer delivers a
//     sender's frames in send order, and `send()` serializes writes — so, like gRPC and UNLIKE MQTT,
//     TcpTransport needs NO resequencer. TCP IS the flow-controlled, order-preserving substrate the
//     resequencer exists to synthesize for brokers.
//   * Lazy dial on first cross-node send (021): the peer socket is opened on demand and cached; a broken
//     connection is dropped and redialed on the next send (fire-and-forget — a lost frame surfaces up the
//     010 delivery table as an ask error / dead-letter, never a return code here).
//   * Fire-and-forget `send` (Transport contract): no delivery result; errors are counted for health.
//
// SCOPE (honest): this is a straightforward blocking-thread-per-connection implementation chosen for
// CLARITY and testability, not the epoll/io_uring event loop Quark 019 will ship. It carries real bytes
// over real sockets with correct framing, ordering, and shutdown — enough to prove the seam and back the
// TransportSelector — but it is a reference adapter, not the tuned production fabric.
//
// PORTABILITY (019/AeroEdge PAL): every socket call goes through QuarkCpp's own PAL (`pal/net.hpp`,
// reachable here because aero-transport links quark::quark — see CMakeLists.txt) rather than raw
// POSIX/Winsock headers directly, so this compiles and runs on both backends Quark's PAL covers. Quark's
// socket primitives are non-blocking by design (its own reactor's contract); this adapter's blocking
// read/write helpers below wrap them with a small `would_block()`-retry loop, using `aero/pal/poll.hpp`
// (the one readiness primitive Quark's reactor-only PAL doesn't expose standalone) for the wait.
//
// OFF THE HOT PATH / layering (R0): an optional aero-transport backend behind the seam; never pulled into
// aero-core or the flow steady path. Uses ordinary threads/among std containers, no 0-alloc discipline.
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pal/net.hpp"  // quark::pal::* — fd_t, tcp_listen/tcp_connect/accept_one/recv_some/send_some/...

#if !defined(_WIN32)
#include <arpa/inet.h>  // inet_pton (Windows gets it transitively via pal/net.hpp's ws2tcpip.h)
#include <netdb.h>       // getaddrinfo/freeaddrinfo (Windows: same, via ws2tcpip.h)
#endif

#include "aero/pal/poll.hpp"
#include "aero/transport/frame_codec.hpp"
#include "aero/transport/transport.hpp"

namespace aero::transport {

class TcpTransport final : public ITransport {
public:
    // A peer's dial address (host + TCP port). Populated per deployment (014 §8 reachability).
    struct Peer {
        std::string host;
        std::uint16_t port = 0;
    };

    struct Config {
        NodeId self{};                                     // this node's identity (stamped on nothing here;
                                                           // frames already carry `from` — used for diag)
        std::string bind_host = "127.0.0.1";               // interface to listen on
        std::uint16_t listen_port = 0;                     // 0 ⇒ ephemeral; resolved after bind()
        std::unordered_map<std::uint64_t, Peer> peers;     // NodeId.value → dial address
    };

    // A frame larger than this is treated as a protocol error and drops the connection (anti-OOM, R5).
    static constexpr std::uint32_t kMaxFrameBytes = 64u * 1024u * 1024u;
    // dial()'s non-blocking connect must complete within this window or the candidate is abandoned —
    // Quark's `tcp_connect` starts the connect and returns immediately (EINPROGRESS), so unlike the old
    // blocking `::connect()` this needs an explicit bound.
    static constexpr int kConnectTimeoutMs = 5000;

    explicit TcpTransport(Config cfg) : cfg_(std::move(cfg)) {}
    ~TcpTransport() override { stop(); }

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Bind + listen + spawn the accept loop. Set on_receive() BEFORE start() so no inbound frame races an
    // unset sink. Returns the documented error string on any socket failure (fail-closed, never throws).
    [[nodiscard]] std::expected<void, std::string> start() {
        ::in_addr bind_addr{};
        if (::inet_pton(AF_INET, cfg_.bind_host.c_str(), &bind_addr) != 1)
            return std::unexpected("tcp transport: invalid bind_host '" + cfg_.bind_host + "'");
        const std::uint64_t addr_u64 = ::ntohl(bind_addr.s_addr);

        auto lfd = quark::pal::tcp_listen(addr_u64, cfg_.listen_port, 16);
        if (!lfd) return err("tcp_listen", lfd.error());
        listen_fd_ = *lfd;

        // Resolve the actual port (needed when listen_port==0 so peers can be told where to dial).
        if (auto p = quark::pal::local_port(listen_fd_)) resolved_port_ = *p;

        running_.store(true, std::memory_order_release);
        accept_thread_ = std::thread([this] { accept_loop(); });
        return {};
    }

    // Stop the accept loop, close all sockets, join every thread. Idempotent; also run by the destructor.
    // ORDER MATTERS (TSan-clean): flip running_ → false, JOIN the accept thread FIRST (it exits within one
    // poll timeout), and only THEN close/clear listen_fd_. The accept loop only ever READS listen_fd_ while
    // it runs; nothing writes the fd until after this join — so there is no concurrent access to it, and no
    // close-during-poll fd-reuse hazard.
    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (accept_thread_.joinable()) accept_thread_.join();
        if (listen_fd_ != quark::pal::invalid_fd) {
            quark::pal::close_fd(listen_fd_);
            listen_fd_ = quark::pal::invalid_fd;
        }
        {
            std::lock_guard<std::mutex> g(conn_mu_);
            for (std::thread& t : conn_threads_)
                if (t.joinable()) t.join();
            conn_threads_.clear();
        }
        {
            std::lock_guard<std::mutex> g(send_mu_);
            for (auto& [k, fd] : peer_fd_)
                if (fd != quark::pal::invalid_fd) quark::pal::close_fd(fd);
            peer_fd_.clear();
        }
    }

    // The port this node actually listens on (== listen_port unless it was 0/ephemeral).
    [[nodiscard]] std::uint16_t listen_port() const noexcept { return resolved_port_; }

    // Advertise a peer's dial address after construction — needed when peers bind ephemeral ports (0) and
    // only learn each other's resolved port() post-start (014 §8 reachability learned at deploy time).
    void add_peer(NodeId peer, std::string host, std::uint16_t port) {
        std::lock_guard<std::mutex> g(send_mu_);
        cfg_.peers[peer.value] = Peer{std::move(host), port};
    }

    // --- Transport seam (010) --------------------------------------------------------------------------
    void send(NodeId to, MessageFrame frame) override {
        const std::vector<std::byte> body = encode_frame(frame);
        if (body.size() > kMaxFrameBytes) {
            send_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::lock_guard<std::mutex> g(send_mu_);  // one ordered writer per node ⇒ per-peer FIFO (C1)
        const quark::pal::fd_t fd = ensure_peer(to.value);
        if (fd == quark::pal::invalid_fd) {
            send_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::byte lenbuf[4];
        put_le32(lenbuf, static_cast<std::uint32_t>(body.size()));
        if (!write_all(fd, lenbuf, 4) || !write_all(fd, body.data(), body.size())) {
            quark::pal::close_fd(fd);
            peer_fd_[to.value] = quark::pal::invalid_fd;  // mark for lazy redial on the next send
            send_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_receive(std::function<void(MessageFrame)> cb) override { cb_ = std::move(cb); }

    [[nodiscard]] std::string_view name() const noexcept override { return "tcp"; }
    [[nodiscard]] TransportClass transport_class() const noexcept override { return TransportClass::Tcp; }

    // --- diagnostics -----------------------------------------------------------------------------------
    [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_recv_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t send_errors() const noexcept { return send_errors_.load(std::memory_order_relaxed); }

private:
    // --- byte helpers ----------------------------------------------------------------------------------
    static void put_le32(std::byte* p, std::uint32_t v) noexcept {
        for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
    static std::uint32_t get_le32(const std::byte* p) noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
        return v;
    }

    std::unexpected<std::string> err(std::string_view what, std::error_code ec) {
        return std::unexpected("tcp transport: " + std::string(what) + " failed: " + ec.message());
    }

    // --- write all n bytes (handles partial writes + backpressure on a non-blocking socket) ------------
    static bool write_all(quark::pal::fd_t fd, const std::byte* buf, std::size_t n) {
        std::size_t sent = 0;
        while (sent < n) {
            auto w = quark::pal::send_some(fd, buf + sent, n - sent);
            if (w) {
                sent += *w;
                continue;
            }
            if (w.error() != quark::pal::would_block()) return false;
            if (!aero::pal::wait_writable(fd, 200)) return false;  // poll itself failed
        }
        return true;
    }

    // Read exactly n bytes, polling with a timeout so the loop notices stop() (running_ → false) promptly.
    static bool read_exact(quark::pal::fd_t fd, std::byte* buf, std::size_t n, std::atomic<bool>& running) {
        std::size_t got = 0;
        while (got < n) {
            if (!running.load(std::memory_order_acquire)) return false;
            const auto ready = aero::pal::wait_readable(fd, 200);
            if (!ready) return false;   // poll failed
            if (!*ready) continue;      // timeout → re-check running_
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

    void accept_loop() {
        while (running_.load(std::memory_order_acquire)) {
            const auto ready = aero::pal::wait_readable(listen_fd_, 200);
            if (!ready || !*ready) continue;  // timeout/err → re-check running_ (stop() closes listen_fd_)
            auto cfd = quark::pal::accept_one(listen_fd_);  // already non-blocking + nodelay
            if (!cfd) continue;
            std::lock_guard<std::mutex> g(conn_mu_);
            conn_threads_.emplace_back([this, fd = *cfd] { reader_loop(fd); });
        }
    }

    // One inbound connection == one sender's ordered stream. Reassemble length-prefixed frames, decode,
    // hand up to the sink. cb_ is fixed before start(), so reading it here is race-free (R0 off hot path).
    void reader_loop(quark::pal::fd_t fd) {
        for (;;) {
            std::byte lenbuf[4];
            if (!read_exact(fd, lenbuf, 4, running_)) break;
            const std::uint32_t len = get_le32(lenbuf);
            if (len == 0 || len > kMaxFrameBytes) break;  // malformed → drop the connection (fail-closed)
            std::vector<std::byte> body(len);
            if (!read_exact(fd, body.data(), len, running_)) break;
            frames_recv_.fetch_add(1, std::memory_order_relaxed);
            if (auto f = decode_frame(body); f && cb_) cb_(std::move(*f));
        }
        quark::pal::close_fd(fd);
    }

    // Return a live connected socket to peer `to`, dialing lazily. Caller holds send_mu_. invalid_fd ⇒
    // unroutable.
    quark::pal::fd_t ensure_peer(std::uint64_t to) {
        const auto it = peer_fd_.find(to);
        if (it != peer_fd_.end() && it->second != quark::pal::invalid_fd) return it->second;
        const auto pit = cfg_.peers.find(to);
        if (pit == cfg_.peers.end()) return quark::pal::invalid_fd;  // no advertised address → relay elsewhere
        const quark::pal::fd_t fd = dial(pit->second.host, pit->second.port);
        peer_fd_[to] = fd;
        return fd;
    }

    static quark::pal::fd_t dial(const std::string& host, std::uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string portstr = std::to_string(port);
        if (::getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res) != 0 || res == nullptr)
            return quark::pal::invalid_fd;

        quark::pal::fd_t fd = quark::pal::invalid_fd;
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
            const std::uint64_t addr_u64 = ::ntohl(sin->sin_addr.s_addr);
            auto attempt = quark::pal::tcp_connect(addr_u64, port);
            if (!attempt) continue;
            fd = *attempt;
            const auto writable = aero::pal::wait_writable(fd, kConnectTimeoutMs);
            if (writable && *writable && quark::pal::connect_result(fd)) break;  // connected
            quark::pal::close_fd(fd);
            fd = quark::pal::invalid_fd;
        }
        ::freeaddrinfo(res);
        return fd;
    }

    Config cfg_;
    quark::pal::fd_t listen_fd_ = quark::pal::invalid_fd;
    std::uint16_t resolved_port_ = 0;
    std::atomic<bool> running_{false};

    std::function<void(MessageFrame)> cb_;  // set once, before start()

    std::thread accept_thread_;
    std::mutex conn_mu_;
    std::vector<std::thread> conn_threads_;

    std::mutex send_mu_;                                            // serializes writes ⇒ per-peer FIFO (C1)
    std::unordered_map<std::uint64_t, quark::pal::fd_t> peer_fd_;   // NodeId.value → cached connected socket

    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_recv_{0};
    std::atomic<std::uint64_t> send_errors_{0};
};

}  // namespace aero::transport
