// AeroEdge Transport — `TcpTransport`, a REAL socket adapter over Quark's `Transport` seam (014 §4, X1).
//
// WHAT THIS IS: the concrete, wire-carrying realization of "Quark's coordinator-free default fabric"
// (014 §4 B2 / §7 Tcp). Unlike the MQTT/gRPC adapters — whose backends need an external broker / gRPC
// stack and so ship as honest offline gates — TCP needs NOTHING but POSIX sockets, which are always
// present. So this adapter is fully real and runs end-to-end offline: two nodes on distinct NodeIds
// exchange genuine `MessageFrame`s over loopback (or any) TCP, and the phase-7 §2 cross-node path
// (tested in-process over Quark's LoopbackTransport) is here proven over an actual socket.
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
// SCOPE (honest): this is a straightforward blocking-socket + per-connection-reader-thread implementation
// chosen for CLARITY and testability, not the epoll/io_uring event loop Quark 019 will ship. It carries
// real bytes over real sockets with correct framing, ordering, and shutdown — enough to prove the seam
// and back the TransportSelector — but it is a reference adapter, not the tuned production fabric.
//
// OFF THE HOT PATH / layering (R0): an optional aero-transport backend behind the seam; never pulled into
// aero-core or the flow steady path. Uses ordinary threads/among std containers, no 0-alloc discipline.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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

    explicit TcpTransport(Config cfg) : cfg_(std::move(cfg)) {}
    ~TcpTransport() override { stop(); }

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Bind + listen + spawn the accept loop. Set on_receive() BEFORE start() so no inbound frame races an
    // unset sink. Returns the documented error string on any socket failure (fail-closed, never throws).
    [[nodiscard]] std::expected<void, std::string> start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return err("socket");
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cfg_.listen_port);
        if (::inet_pton(AF_INET, cfg_.bind_host.c_str(), &addr.sin_addr) != 1)
            return close_and_err("inet_pton(bind_host)");
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            return close_and_err("bind");
        if (::listen(listen_fd_, 16) != 0) return close_and_err("listen");

        // Resolve the actual port (needed when listen_port==0 so peers can be told where to dial).
        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
            resolved_port_ = ntohs(bound.sin_port);

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
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
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
                if (fd >= 0) ::close(fd);
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
        const int fd = ensure_peer(to.value);
        if (fd < 0) {
            send_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::byte lenbuf[4];
        put_le32(lenbuf, static_cast<std::uint32_t>(body.size()));
        if (!write_all(fd, lenbuf, 4) || !write_all(fd, body.data(), body.size())) {
            ::close(fd);
            peer_fd_[to.value] = -1;  // mark for lazy redial on the next send
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

    std::unexpected<std::string> err(std::string_view what) {
        return std::unexpected("tcp transport: " + std::string(what) + " failed: " + std::strerror(errno));
    }
    std::unexpected<std::string> close_and_err(std::string_view what) {
        auto e = err(what);
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        return e;
    }

    // --- write all n bytes (handles partial writes), MSG_NOSIGNAL so a dead peer never SIGPIPEs us -----
    static bool write_all(int fd, const std::byte* buf, std::size_t n) {
        std::size_t sent = 0;
        while (sent < n) {
            const ssize_t w = ::send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            sent += static_cast<std::size_t>(w);
        }
        return true;
    }

    // Read exactly n bytes, polling with a timeout so the loop notices stop() (running_ → false) promptly.
    static bool read_exact(int fd, std::byte* buf, std::size_t n, std::atomic<bool>& running) {
        std::size_t got = 0;
        while (got < n) {
            if (!running.load(std::memory_order_acquire)) return false;
            pollfd pfd{fd, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (pr == 0) continue;  // timeout → re-check running_
            const ssize_t r = ::recv(fd, buf + got, n - got, 0);
            if (r == 0) return false;  // peer closed
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    void accept_loop() {
        while (running_.load(std::memory_order_acquire)) {
            pollfd pfd{listen_fd_, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr <= 0) continue;  // timeout/err → re-check running_ (stop() closes listen_fd_)
            const int cfd = ::accept(listen_fd_, nullptr, nullptr);
            if (cfd < 0) continue;
            int one = 1;
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            std::lock_guard<std::mutex> g(conn_mu_);
            conn_threads_.emplace_back([this, cfd] { reader_loop(cfd); });
        }
    }

    // One inbound connection == one sender's ordered stream. Reassemble length-prefixed frames, decode,
    // hand up to the sink. cb_ is fixed before start(), so reading it here is race-free (R0 off hot path).
    void reader_loop(int fd) {
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
        ::close(fd);
    }

    // Return a live connected socket to peer `to`, dialing lazily. Caller holds send_mu_. -1 ⇒ unroutable.
    int ensure_peer(std::uint64_t to) {
        const auto it = peer_fd_.find(to);
        if (it != peer_fd_.end() && it->second >= 0) return it->second;
        const auto pit = cfg_.peers.find(to);
        if (pit == cfg_.peers.end()) return -1;  // no advertised address for this peer → relay elsewhere
        const int fd = dial(pit->second.host, pit->second.port);
        peer_fd_[to] = fd;
        return fd;
    }

    static int dial(const std::string& host, std::uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string portstr = std::to_string(port);
        if (::getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res) != 0 || res == nullptr) return -1;
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

    Config cfg_;
    int listen_fd_ = -1;
    std::uint16_t resolved_port_ = 0;
    std::atomic<bool> running_{false};

    std::function<void(MessageFrame)> cb_;  // set once, before start()

    std::thread accept_thread_;
    std::mutex conn_mu_;
    std::vector<std::thread> conn_threads_;

    std::mutex send_mu_;                                 // serializes writes ⇒ per-peer FIFO (C1)
    std::unordered_map<std::uint64_t, int> peer_fd_;     // NodeId.value → cached connected socket

    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_recv_{0};
    std::atomic<std::uint64_t> send_errors_{0};
};

}  // namespace aero::transport
