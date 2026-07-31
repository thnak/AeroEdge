// AeroEdge PAL — `TlsServerContext`/`TlsSession`, a minimal non-blocking TLS byte-channel over mbedTLS
// (M5, TLS+ACL milestone). Server-side only: this wraps an already-accepted `quark::pal::fd_t` (from
// e.g. `quark::pal::accept_one`) with TLS record framing, so a caller that already has a plaintext
// accept loop (NativeBroker's, TcpTransport's) can add TLS by handshaking the fd through this instead of
// handing it straight to `mqtt_codec.hpp`'s read/write helpers. WHAT THIS DOES NOT DO (honest scope):
// no TLS client role, no session resumption/tickets, no ALPN, no OCSP — a server accepting connections
// and (optionally) verifying a client certificate (mTLS) is the entire surface M5 needs.
//
// NON-BLOCKING I/O: follows mbedTLS's own documented BIO pattern for non-blocking sockets (see mbedTLS's
// programs/ssl/ssl_server2.c or its "Non-blocking I/O" guide) — `mbedtls_ssl_set_bio()` is given plain
// callbacks that call `quark::pal::send_some`/`recv_some` on the raw fd and translate the result:
// would_block() becomes `MBEDTLS_ERR_SSL_WANT_READ`/`WANT_WRITE`, which `mbedtls_ssl_handshake`/`read`/
// `write` all propagate straight back to their own caller — the exact signal `TlsServerContext::accept`'s
// poll-and-retry loop and `TlsSession::recv_some`/`send_some`'s single-attempt contract need. This is the
// SAME poll-with-a-short-timeout-and-retry idiom already established by `aero/pal/poll.hpp` and
// `aero/transport/mqtt_codec.hpp`'s `read_n` (200ms slices), not a new one invented here.
//
// COMPILED-OUT BUILDS (AERO_ENABLE_TLS=OFF, root CMakeLists.txt): `AERO_TLS_ENABLED` is then undefined,
// and this header compiles a stub with the IDENTICAL class/method surface whose `create()` always
// returns the "not compiled in" error below — so code that unconditionally includes this header and
// calls it still builds, it just gets a clean runtime error instead of a compile failure (no `#ifdef`
// needed at any call site outside this header).
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>

#include "pal/net.hpp"  // quark::pal::* — fd_t, invalid_fd, recv_some/send_some/would_block

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED

#include <chrono>
#include <memory>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>  // MBEDTLS_ERR_NET_SEND_FAILED/RECV_FAILED (not pulled in by ssl.h alone)
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "aero/pal/poll.hpp"

#endif  // AERO_TLS_ENABLED

namespace aero::pal::tls {

// Server-side TLS material. `ca_file` is optional: leave it empty for ordinary server-authenticated TLS
// (the common MQTT-over-TLS case — the device verifies the broker, the broker takes any client); set it
// to a CA bundle to additionally REQUIRE + verify a client certificate (mutual TLS).
struct ServerConfig {
    std::string cert_file;  // PEM server certificate (leaf, optionally + chain)
    std::string key_file;   // PEM private key matching cert_file
    std::string ca_file;    // optional: PEM CA bundle a client cert must chain to (enables mTLS)
};

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED

namespace detail {

// The fd is packed directly INTO the mbedTLS BIO's void* context (not a pointer to a TlsSession member)
// so a TlsSession can be freely moved without invalidating the callback's context pointer. fd_t is an
// `int` (POSIX) or `SOCKET`/UINT_PTR (Windows) — both round-trip exactly through a pointer-sized integer.
[[nodiscard]] inline quark::pal::fd_t bio_ctx_fd(void* ctx) noexcept {
    return static_cast<quark::pal::fd_t>(reinterpret_cast<std::uintptr_t>(ctx));
}
[[nodiscard]] inline void* fd_bio_ctx(quark::pal::fd_t fd) noexcept {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(fd));
}

// mbedTLS's documented non-blocking BIO contract: f_send/f_recv return >=0 bytes transferred, or a
// NEGATIVE mbedtls error code — MBEDTLS_ERR_SSL_WANT_READ/WRITE for "nothing to do right now, retry
// later" (mbedtls_ssl_handshake/read/write propagate that code straight back to their own caller).
inline int bio_send(void* ctx, const unsigned char* buf, std::size_t len) {
    auto r = quark::pal::send_some(bio_ctx_fd(ctx), reinterpret_cast<const std::byte*>(buf), len);
    if (r) return static_cast<int>(*r);
    if (r.error() == quark::pal::would_block()) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
inline int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto r = quark::pal::recv_some(bio_ctx_fd(ctx), reinterpret_cast<std::byte*>(buf), len);
    // Ok(0) (peer EOF) passes through as a 0-byte transfer — mbedTLS reads that as its own record-layer
    // EOF signal and surfaces MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY (or a plain conn-reset) to its caller.
    if (r) return static_cast<int>(*r);
    if (r.error() == quark::pal::would_block()) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

// Stateless deleter pairing an mbedTLS type's `_free` function with `delete`. Every mbedTLS struct this
// header owns is heap-allocated through this (not a plain value member) specifically so its ADDRESS is
// STABLE across a TlsSession/TlsServerContext move: mbedtls_ssl_conf_own_cert()/conf_ca_chain() stash
// raw pointers to the cert/key/config structs they're given inside mbedTLS's own internal bookkeeping,
// so moving those structs BY VALUE (copying their bytes to a new object's storage) would leave that
// bookkeeping pointing at a stale address the moment the old (moved-from) object is destroyed — a
// use-after-free. A `unique_ptr` move only ever moves the POINTER; the pointee's address never changes.
template <typename T, void (*FreeFn)(T*)>
struct MbedDeleter {
    void operator()(T* p) const noexcept {
        if (p) {
            FreeFn(p);
            delete p;
        }
    }
};
using SslCtxDeleter = MbedDeleter<mbedtls_ssl_context, mbedtls_ssl_free>;
using EntropyDeleter = MbedDeleter<mbedtls_entropy_context, mbedtls_entropy_free>;
using CtrDrbgDeleter = MbedDeleter<mbedtls_ctr_drbg_context, mbedtls_ctr_drbg_free>;
using X509CrtDeleter = MbedDeleter<mbedtls_x509_crt, mbedtls_x509_crt_free>;
using PkDeleter = MbedDeleter<mbedtls_pk_context, mbedtls_pk_free>;
using SslConfigDeleter = MbedDeleter<mbedtls_ssl_config, mbedtls_ssl_config_free>;

// Every mbedTLS `_init()` this header calls is documented to zero-fill its struct (equivalent to a
// value-initialized `T{}`) and every corresponding `_free()` is documented safe to call on that
// zero-filled-but-never-set-up state (mbedTLS's own examples always unconditionally free at a cleanup
// label, including on early-exit paths before setup completed) — so a fresh `heap_init<T, Init>()` is
// safe to destroy immediately with no separate "was this actually set up" tracking flag anywhere here.
template <typename T, void (*InitFn)(T*)>
[[nodiscard]] inline T* heap_init() {
    T* p = new T{};
    InitFn(p);
    return p;
}

[[nodiscard]] inline std::string mbed_err(std::string_view what, int rc) {
    char buf[160];
    mbedtls_strerror(rc, buf, sizeof(buf));
    return std::string(what) + ": " + buf + " (mbedtls " + std::to_string(rc) + ")";
}

}  // namespace detail

class TlsServerContext;

// One TlsSession per accepted, handshaken connection. Move-only (the underlying mbedtls_ssl_context is
// heap-owned via unique_ptr — see detail::MbedDeleter's banner above for why — so a move never
// invalidates anything mbedTLS itself is holding a pointer to, and there is never a moment where two
// live TlsSessions' ssl contexts could both reference the same fd).
class TlsSession {
public:
    TlsSession(TlsSession&&) noexcept = default;
    TlsSession& operator=(TlsSession&&) noexcept = default;
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    ~TlsSession() = default;

    // Mirrors quark::pal::recv_some's Ok(n)/would_block() contract EXACTLY — ONE attempt, no internal
    // retry loop — so aero::transport::TlsChannel (io_channel.hpp) can wrap this identically to a raw fd
    // and a generic caller downstream can't tell a TLS session's would-block from a plaintext one's.
    // WANT_READ and WANT_WRITE both collapse to would_block(): a generic non-blocking caller polls and
    // retries either way, it has no different action to take between them. PEER_CLOSE_NOTIFY (mbedTLS's
    // own clean-shutdown alert, TLS's "close_notify") maps to Ok(0) — the same EOF value
    // quark::pal::recv_some uses for an orderly peer close, so downstream framing code treats them alike.
    [[nodiscard]] std::expected<std::size_t, std::error_code> recv_some(std::byte* buf, std::size_t n) {
        const int r = mbedtls_ssl_read(ssl_.get(), reinterpret_cast<unsigned char*>(buf), n);
        if (r >= 0) return static_cast<std::size_t>(r);
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return std::size_t{0};
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            return std::unexpected(quark::pal::would_block());
        return std::unexpected(std::make_error_code(std::errc::connection_aborted));
    }

    [[nodiscard]] std::expected<std::size_t, std::error_code> send_some(const std::byte* buf,
                                                                         std::size_t n) {
        const int r = mbedtls_ssl_write(ssl_.get(), reinterpret_cast<const unsigned char*>(buf), n);
        if (r >= 0) return static_cast<std::size_t>(r);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            return std::unexpected(quark::pal::would_block());
        return std::unexpected(std::make_error_code(std::errc::connection_aborted));
    }

    // The underlying socket, for polling readiness (aero::pal::wait_readable/wait_writable) — mbedTLS
    // itself may need a READ ready before a WRITE can proceed (or vice versa) during re-handshakes, so a
    // caller driving its own poll loop around recv_some/send_some's would_block() needs this either way.
    [[nodiscard]] quark::pal::fd_t fd() const noexcept { return fd_; }

private:
    friend class TlsServerContext;
    TlsSession() = default;

    std::unique_ptr<mbedtls_ssl_context, detail::SslCtxDeleter> ssl_;
    quark::pal::fd_t fd_ = quark::pal::invalid_fd;
};

// One TlsServerContext per listener: holds the loaded server cert/key (+ optional client CA), shared
// (read-only, after create()) across every TlsSession it accept()s. Move-only for the same
// heap-ownership reason as TlsSession (detail::MbedDeleter's banner) — mbedtls_ssl_conf_own_cert() bakes
// raw pointers to server_cert_/pkey_'s addresses into conf_'s internal bookkeeping, so those two structs
// (and conf_ itself, and ca_cert_ if mTLS is enabled) must never move in memory after that call.
class TlsServerContext {
public:
    TlsServerContext(TlsServerContext&&) noexcept = default;
    TlsServerContext& operator=(TlsServerContext&&) noexcept = default;
    TlsServerContext(const TlsServerContext&) = delete;
    TlsServerContext& operator=(const TlsServerContext&) = delete;
    ~TlsServerContext() = default;

    // Loads cert/key (+ CA if mTLS) and builds the shared mbedtls_ssl_config. Never throws; a bad/missing
    // PEM file or key returns a descriptive error string (CONVENTIONS.md: std::expected, not exceptions,
    // for expected failure paths).
    [[nodiscard]] static std::expected<TlsServerContext, std::string> create(const ServerConfig& cfg) {
        TlsServerContext ctx;
        ctx.entropy_.reset(detail::heap_init<mbedtls_entropy_context, mbedtls_entropy_init>());
        ctx.ctr_drbg_.reset(detail::heap_init<mbedtls_ctr_drbg_context, mbedtls_ctr_drbg_init>());
        ctx.server_cert_.reset(detail::heap_init<mbedtls_x509_crt, mbedtls_x509_crt_init>());
        ctx.pkey_.reset(detail::heap_init<mbedtls_pk_context, mbedtls_pk_init>());
        ctx.conf_.reset(detail::heap_init<mbedtls_ssl_config, mbedtls_ssl_config_init>());

        // mbedtls_ssl_config needs a CSPRNG (mbedTLS's standard entropy -> CTR_DRBG seeding chain) for
        // the handshake's random values/ephemeral keys — this is boilerplate mbedTLS requires, not an
        // AeroEdge design choice.
        static constexpr char kPers[] = "aero_tls_server";
        int rc = mbedtls_ctr_drbg_seed(ctx.ctr_drbg_.get(), mbedtls_entropy_func, ctx.entropy_.get(),
                                        reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers) - 1);
        if (rc != 0) return std::unexpected(detail::mbed_err("ctr_drbg_seed", rc));

        rc = mbedtls_x509_crt_parse_file(ctx.server_cert_.get(), cfg.cert_file.c_str());
        if (rc != 0)
            return std::unexpected(detail::mbed_err("parse server cert '" + cfg.cert_file + "'", rc));

        rc = mbedtls_pk_parse_keyfile(ctx.pkey_.get(), cfg.key_file.c_str(), nullptr,
                                       mbedtls_ctr_drbg_random, ctx.ctr_drbg_.get());
        if (rc != 0) return std::unexpected(detail::mbed_err("parse key '" + cfg.key_file + "'", rc));

        rc = mbedtls_ssl_config_defaults(ctx.conf_.get(), MBEDTLS_SSL_IS_SERVER,
                                          MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) return std::unexpected(detail::mbed_err("ssl_config_defaults", rc));

        mbedtls_ssl_conf_rng(ctx.conf_.get(), mbedtls_ctr_drbg_random, ctx.ctr_drbg_.get());

        if (!cfg.ca_file.empty()) {
            ctx.ca_cert_.reset(detail::heap_init<mbedtls_x509_crt, mbedtls_x509_crt_init>());
            rc = mbedtls_x509_crt_parse_file(ctx.ca_cert_.get(), cfg.ca_file.c_str());
            if (rc != 0) return std::unexpected(detail::mbed_err("parse ca '" + cfg.ca_file + "'", rc));
            mbedtls_ssl_conf_ca_chain(ctx.conf_.get(), ctx.ca_cert_.get(), nullptr);
            // mTLS: the client MUST present a certificate that chains to ca_file, or the handshake fails
            // outright — no "optional cert, verify if present" middle ground (M5 wants a hard ACL gate).
            mbedtls_ssl_conf_authmode(ctx.conf_.get(), MBEDTLS_SSL_VERIFY_REQUIRED);
        } else {
            // Server-auth-only TLS (the common MQTT-over-TLS case): no client cert requested at all.
            mbedtls_ssl_conf_authmode(ctx.conf_.get(), MBEDTLS_SSL_VERIFY_NONE);
        }

        rc = mbedtls_ssl_conf_own_cert(ctx.conf_.get(), ctx.server_cert_.get(), ctx.pkey_.get());
        if (rc != 0) return std::unexpected(detail::mbed_err("ssl_conf_own_cert", rc));

        return ctx;
    }

    // Full non-blocking handshake over an already-accepted, non-blocking `fd` (every quark::pal::
    // accept_one() fd already is). Polls readable/writable in 200ms slices — the SAME idiom as
    // aero/transport/mqtt_codec.hpp's read_n — until the handshake completes or `timeout_ms` elapses.
    [[nodiscard]] std::expected<TlsSession, std::string> accept(quark::pal::fd_t fd, int timeout_ms = 5000) {
        TlsSession session;
        session.ssl_.reset(detail::heap_init<mbedtls_ssl_context, mbedtls_ssl_init>());
        session.fd_ = fd;

        int rc = mbedtls_ssl_setup(session.ssl_.get(), conf_.get());
        if (rc != 0) return std::unexpected(detail::mbed_err("ssl_setup", rc));

        mbedtls_ssl_set_bio(session.ssl_.get(), detail::fd_bio_ctx(fd), detail::bio_send, detail::bio_recv,
                             nullptr);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            rc = mbedtls_ssl_handshake(session.ssl_.get());
            if (rc == 0) return session;  // handshake complete
            if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                auto ready = aero::pal::wait_readable(fd, 200);
                if (!ready)
                    return std::unexpected("tls handshake: poll(readable) failed: " + ready.error().message());
            } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                auto ready = aero::pal::wait_writable(fd, 200);
                if (!ready)
                    return std::unexpected("tls handshake: poll(writable) failed: " + ready.error().message());
            } else {
                // Any other negative code is a real handshake failure (bad client cert under mTLS,
                // protocol mismatch, ...) — not a would-block signal, so it is not retried.
                return std::unexpected(detail::mbed_err("ssl_handshake", rc));
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return std::unexpected("tls handshake: timed out after " + std::to_string(timeout_ms) +
                                        "ms");
        }
    }

private:
    TlsServerContext() = default;

    std::unique_ptr<mbedtls_entropy_context, detail::EntropyDeleter> entropy_;
    std::unique_ptr<mbedtls_ctr_drbg_context, detail::CtrDrbgDeleter> ctr_drbg_;
    std::unique_ptr<mbedtls_x509_crt, detail::X509CrtDeleter> server_cert_;
    std::unique_ptr<mbedtls_x509_crt, detail::X509CrtDeleter> ca_cert_;  // set only when mTLS (cfg.ca_file)
    std::unique_ptr<mbedtls_pk_context, detail::PkDeleter> pkey_;
    std::unique_ptr<mbedtls_ssl_config, detail::SslConfigDeleter> conf_;
};

#else  // !AERO_TLS_ENABLED — identical call-site surface, honest "not compiled in" gate.

class TlsSession {
public:
    TlsSession(TlsSession&&) noexcept = default;
    TlsSession& operator=(TlsSession&&) noexcept = default;
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    ~TlsSession() = default;

    [[nodiscard]] std::expected<std::size_t, std::error_code> recv_some(std::byte*, std::size_t) {
        return std::unexpected(std::make_error_code(std::errc::not_supported));
    }
    [[nodiscard]] std::expected<std::size_t, std::error_code> send_some(const std::byte*, std::size_t) {
        return std::unexpected(std::make_error_code(std::errc::not_supported));
    }
    [[nodiscard]] quark::pal::fd_t fd() const noexcept { return quark::pal::invalid_fd; }

private:
    friend class TlsServerContext;
    TlsSession() = default;
};

class TlsServerContext {
public:
    TlsServerContext(TlsServerContext&&) noexcept = default;
    TlsServerContext& operator=(TlsServerContext&&) noexcept = default;
    TlsServerContext(const TlsServerContext&) = delete;
    TlsServerContext& operator=(const TlsServerContext&) = delete;
    ~TlsServerContext() = default;

    [[nodiscard]] static std::expected<TlsServerContext, std::string> create(const ServerConfig&) {
        return std::unexpected("TLS support not compiled in (AERO_ENABLE_TLS=OFF)");
    }
    [[nodiscard]] std::expected<TlsSession, std::string> accept(quark::pal::fd_t, int timeout_ms = 5000) {
        (void)timeout_ms;
        return std::unexpected("TLS support not compiled in (AERO_ENABLE_TLS=OFF)");
    }

private:
    TlsServerContext() = default;
};

#endif  // AERO_TLS_ENABLED

}  // namespace aero::pal::tls
