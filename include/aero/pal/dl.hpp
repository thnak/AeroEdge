// AeroEdge PAL — dynamic library loading for the Native extension host loader (spec 008 §2).
//
// QuarkCpp's PAL has nothing here (the actor engine never loads shared libraries), so this is a
// genuinely AeroEdge-owned seam. A portable `void*` handle works on both backends: POSIX's dlopen()
// already returns an opaque `void*`, and Windows' HMODULE is itself a pointer type (typedef struct
// HINSTANCE__* HINSTANCE; typedef HINSTANCE HMODULE), so round-tripping it through void* is safe.
#pragma once

#include <expected>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace aero::pal {

#if defined(_WIN32)

namespace detail {
[[nodiscard]] inline std::string last_error_message() {
    const DWORD err = ::GetLastError();
    char* buf = nullptr;
    const DWORD len = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string msg = len ? std::string(buf, len) : "unknown error " + std::to_string(err);
    if (buf) ::LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return msg;
}
}  // namespace detail

[[nodiscard]] inline std::expected<void*, std::string> dl_open(const std::string& path) {
    HMODULE h = ::LoadLibraryA(path.c_str());
    if (!h) return std::unexpected(detail::last_error_message());
    return reinterpret_cast<void*>(h);
}

[[nodiscard]] inline void* dl_sym(void* handle, const char* name) noexcept {
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
}

inline void dl_close(void* handle) noexcept {
    if (handle) ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
}

#else  // POSIX

[[nodiscard]] inline std::expected<void*, std::string> dl_open(const std::string& path) {
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = ::dlerror();
        return std::unexpected(e ? std::string(e) : "unknown dlopen error");
    }
    return h;
}

[[nodiscard]] inline void* dl_sym(void* handle, const char* name) noexcept {
    return ::dlsym(handle, name);
}

inline void dl_close(void* handle) noexcept {
    if (handle) ::dlclose(handle);
}

#endif

}  // namespace aero::pal
