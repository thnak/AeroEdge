// Test-only helper: pick a free localhost port + spawn/reap a `uv`-hosted Python backend (broker / gRPC
// server) for the REAL transport gates. NOT part of the shipped transport library — lives under tests/.
//
// Model: the C++ test picks a free TCP port, hands it to `uv run --with <pkg> <script> <port>`, waits
// until the port accepts a connection, runs the transport, then SIGTERMs the child's process group on
// teardown. The child runs in its own process group so killing it also kills the python `uv` spawned.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace aero::testutil {

// Bind 127.0.0.1:0, read the assigned port, close — a free ephemeral port (small TOCTOU window; fine on
// loopback for a test). The backend then binds this exact port.
inline std::uint16_t free_port() {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(s); return 0; }
    socklen_t len = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    const std::uint16_t port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

// A spawned `uv run` backend. SIGTERMs its whole process group on destruction.
class UvBackend {
public:
    // uv_bin: absolute path to the uv binary. args: full argv AFTER uv_bin (e.g. {"run","--with","amqtt",
    // script, port}). Child runs in its own process group; stdout/stderr inherit (surface failures).
    UvBackend(const std::string& uv_bin, const std::vector<std::string>& args) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(uv_bin.c_str()));
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);  // new process group ⇒ killpg reaches all
        posix_spawnattr_setpgroup(&attr, 0);
        if (posix_spawn(&pid_, uv_bin.c_str(), nullptr, &attr, argv.data(), environ) != 0) pid_ = -1;
        posix_spawnattr_destroy(&attr);
    }

    ~UvBackend() {
        if (pid_ > 0) {
            ::killpg(pid_, SIGTERM);
            int status = 0;
            for (int i = 0; i < 50; ++i) {  // up to ~5s for graceful exit
                if (::waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            ::killpg(pid_, SIGKILL);
            ::waitpid(pid_, &status, 0);
        }
    }

    UvBackend(const UvBackend&) = delete;
    UvBackend& operator=(const UvBackend&) = delete;

    [[nodiscard]] bool spawned() const noexcept { return pid_ > 0; }

    // Poll until 127.0.0.1:port accepts a TCP connection (backend bound + listening), or timeout.
    static bool wait_for_port(std::uint16_t port, int timeout_ms = 60000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const int s = ::socket(AF_INET, SOCK_STREAM, 0);
            if (s >= 0) {
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                a.sin_port = htons(port);
                const bool ok = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
                ::close(s);
                if (ok) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

private:
    pid_t pid_ = -1;
};

}  // namespace aero::testutil
