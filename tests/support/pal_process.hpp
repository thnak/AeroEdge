// Test-only PAL: spawn/terminate/wait/kill a child process TREE. NOT part of the shipped library —
// lives under tests/, used only by proc_util.hpp's UvBackend to launch the `uv`-hosted MQTT/gRPC
// Python backends the real transport gates exercise.
//
// "Process tree" matters because `uv run ...` itself spawns a python child — killing just the `uv` PID
// would leave that child (and the broker/server it hosts) running. POSIX handles this with process
// groups (spawn into a new group, killpg the group). Windows has no process-group-kill primitive for
// arbitrary processes, so the equivalent here is a Job Object: the child (and everything it spawns,
// which inherits job membership by default) is assigned to a job at spawn time, and TerminateJobObject
// kills the whole tree atomically.
//
// Windows also has no SIGTERM-for-an-arbitrary-process analogue — GenerateConsoleCtrlEvent(CTRL_BREAK_
// EVENT) on a process created with CREATE_NEW_PROCESS_GROUP is the closest "ask nicely" primitive, but
// it only works if the child actually handles Ctrl+Break (best-effort, mirrors the POSIX SIGTERM path's
// own best-effort framing — kill_hard() is the real backstop on both platforms either way).
#pragma once

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace aero::testutil {

class Process {
public:
    Process() = default;
    ~Process() { reset(); }

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Spawn `exe` with argv = {exe, args...}, placed in its own process group (POSIX) / Job Object
    // (Windows) so terminate_gracefully()/kill_hard() reach the whole tree. stdout/stderr inherit
    // (surfaces backend failures in the test log).
    [[nodiscard]] bool spawn(const std::string& exe, const std::vector<std::string>& args) {
        reset();
#if defined(_WIN32)
        std::string cmdline = quote(exe);
        for (const std::string& a : args) {
            cmdline += ' ';
            cmdline += quote(a);
        }
        job_ = ::CreateJobObjectA(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        ZeroMemory(&pi_, sizeof(pi_));
        std::vector<char> buf(cmdline.begin(), cmdline.end());
        buf.push_back('\0');
        // CREATE_SUSPENDED so the child can be assigned to the job BEFORE it runs (and can spawn its
        // own child, e.g. uv's python) — avoids the race where a grandchild escapes job membership.
        const BOOL ok = ::CreateProcessA(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                                          CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED, nullptr, nullptr,
                                          &si, &pi_);
        if (!ok) {
            if (job_) { ::CloseHandle(job_); job_ = nullptr; }
            return false;
        }
        if (job_) ::AssignProcessToJobObject(job_, pi_.hProcess);
        ::ResumeThread(pi_.hThread);
        spawned_ = true;
        return true;
#else
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);  // new process group ⇒ killpg reaches all
        posix_spawnattr_setpgroup(&attr, 0);
        const int rc = posix_spawn(&pid_, exe.c_str(), nullptr, &attr, argv.data(), environ);
        posix_spawnattr_destroy(&attr);
        spawned_ = (rc == 0);
        if (!spawned_) pid_ = -1;
        return spawned_;
#endif
    }

    [[nodiscard]] bool spawned() const noexcept { return spawned_; }

    // Best-effort graceful stop.
    void terminate_gracefully() {
        if (!spawned_) return;
#if defined(_WIN32)
        ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi_.dwProcessId);
#else
        ::killpg(pid_, SIGTERM);
#endif
    }

    // Poll for exit, without blocking longer than timeout_ms. true == exited (and reaped).
    [[nodiscard]] bool wait_for_exit(int timeout_ms) {
        if (!spawned_) return true;
#if defined(_WIN32)
        if (::WaitForSingleObject(pi_.hProcess, static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms)) ==
            WAIT_OBJECT_0) {
            spawned_ = false;
            return true;
        }
        return false;
#else
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        do {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                spawned_ = false;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
#endif
    }

    // Hard-kill the whole process tree; blocks until reaped.
    void kill_hard() {
        if (!spawned_) return;
#if defined(_WIN32)
        if (job_) ::TerminateJobObject(job_, 1);
        else ::TerminateProcess(pi_.hProcess, 1);
        ::WaitForSingleObject(pi_.hProcess, 5000);
        spawned_ = false;
#else
        ::killpg(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        spawned_ = false;
#endif
    }

private:
    void reset() {
        if (spawned_) kill_hard();
#if defined(_WIN32)
        if (pi_.hThread) ::CloseHandle(pi_.hThread);
        if (pi_.hProcess) ::CloseHandle(pi_.hProcess);
        if (job_) ::CloseHandle(job_);
        pi_ = PROCESS_INFORMATION{};
        job_ = nullptr;
#else
        pid_ = -1;
#endif
    }

#if defined(_WIN32)
    // Minimal argv-element quoting for CreateProcess's single command-line string: wrap in quotes when
    // the arg contains a space, escaping embedded quotes. Sufficient for uv/python-style args (paths,
    // flags, port numbers) without exotic characters.
    static std::string quote(const std::string& s) {
        if (!s.empty() && s.find_first_of(" \t\"") == std::string::npos) return s;
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += '\\';
            out += c;
        }
        out += '"';
        return out;
    }

    PROCESS_INFORMATION pi_{};
    HANDLE job_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
    bool spawned_ = false;
};

}  // namespace aero::testutil
