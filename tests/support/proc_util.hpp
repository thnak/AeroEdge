// Test-only helper: pick a free localhost port + spawn/reap a `uv`-hosted Python backend (broker / gRPC
// server) for the REAL transport gates. NOT part of the shipped transport library — lives under tests/.
//
// Model: the C++ test picks a free TCP port, hands it to `uv run --with <pkg> <script> <port>`, waits
// until the port accepts a connection, runs the transport, then stops the child's whole process tree on
// teardown (see pal_process.hpp — POSIX process-group / Windows Job Object, so killing `uv` also kills
// the python it spawns).
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "pal/net.hpp"  // quark::pal::* — reused directly rather than re-deriving a socket PAL here

#include "aero/pal/poll.hpp"
#include "pal_process.hpp"

namespace aero::testutil {

// Bind 127.0.0.1:0, read the assigned port, close — a free ephemeral port (small TOCTOU window; fine on
// loopback for a test). The backend then binds this exact port.
inline std::uint16_t free_port() {
    auto lfd = quark::pal::tcp_listen(quark::pal::ipv4_loopback, 0, 1);
    if (!lfd) return 0;
    std::uint16_t port = 0;
    if (auto p = quark::pal::local_port(*lfd)) port = *p;
    quark::pal::close_fd(*lfd);
    return port;
}

// A spawned `uv run` backend. Stops its whole process tree on destruction.
class UvBackend {
public:
    // uv_bin: absolute path to the uv binary. args: full argv AFTER uv_bin (e.g. {"run","--with","amqtt",
    // script, port}).
    UvBackend(const std::string& uv_bin, const std::vector<std::string>& args) {
        (void)proc_.spawn(uv_bin, args);
    }

    ~UvBackend() {
        if (!proc_.spawned()) return;
        proc_.terminate_gracefully();
        if (!proc_.wait_for_exit(5000)) proc_.kill_hard();  // up to ~5s for graceful exit, then hard-kill
    }

    UvBackend(const UvBackend&) = delete;
    UvBackend& operator=(const UvBackend&) = delete;

    [[nodiscard]] bool spawned() const noexcept { return proc_.spawned(); }

    // Poll until 127.0.0.1:port accepts a TCP connection (backend bound + listening), or timeout.
    static bool wait_for_port(std::uint16_t port, int timeout_ms = 60000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            auto fd = quark::pal::tcp_connect(quark::pal::ipv4_loopback, port);
            if (fd) {
                const auto ready = aero::pal::wait_writable(*fd, 200);
                const bool ok = ready && *ready && static_cast<bool>(quark::pal::connect_result(*fd));
                quark::pal::close_fd(*fd);
                if (ok) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

private:
    Process proc_;
};

}  // namespace aero::testutil
