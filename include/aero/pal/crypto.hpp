// AeroEdge PAL — real asymmetric sign/verify (ECDSA P-256 over SHA-256) via mbedTLS's `pk` layer, the
// SAME vendored mbedTLS this project already links for TLS (aero/pal/tls.hpp) — no second crypto lib,
// no new vendored dependency. Built for OTA image signing (011 §3/§6, O1): `aero::ota` previously stood
// in with a keyed FNV-1a hash pending a "real" asymmetric primitive (011 §6 filed that under Quark 020's
// secret/crypto scope; Quark hasn't productionized one yet). Same precedent as tls.hpp itself: where
// Quark doesn't yet expose a productionized primitive AeroEdge needs, build directly against the
// already-vendored mbedTLS rather than block on it or fake it.
//
// AERO_TLS_ENABLED=OFF (root CMakeLists.txt's AERO_ENABLE_TLS option) compiles a stub with the IDENTICAL
// function surface that always returns the "not compiled in" error — mirrors tls.hpp's own compiled-out
// shape, so callers never need an `#ifdef` at the call site.
#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "aero/pal/tls.hpp"  // detail::ensure_threading_registered/heap_init/mbed_err + RAII deleters

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#endif

namespace aero::pal::crypto {

struct Error {
    std::string message;
};

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED

namespace detail {

[[nodiscard]] inline std::string hex_encode(const unsigned char* p, std::size_t n) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 0x0F]);
    }
    return out;
}

[[nodiscard]] inline std::expected<std::string, std::string> hex_decode(std::string_view hex) {
    if (hex.empty() || hex.size() % 2 != 0) return std::unexpected("invalid hex signature length");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::unexpected("invalid hex signature digit");
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

// Shared entropy/CTR_DRBG bring-up (mbedTLS boilerplate, identical shape to tls.hpp's own
// TlsServerContext::create) — signing needs an RNG for ECDSA's per-signature nonce.
[[nodiscard]] inline std::expected<
    std::pair<std::unique_ptr<mbedtls_entropy_context, aero::pal::tls::detail::EntropyDeleter>,
              std::unique_ptr<mbedtls_ctr_drbg_context, aero::pal::tls::detail::CtrDrbgDeleter>>,
    std::string>
seeded_rng() {
    aero::pal::tls::detail::ensure_threading_registered();
    std::unique_ptr<mbedtls_entropy_context, aero::pal::tls::detail::EntropyDeleter> entropy(
        aero::pal::tls::detail::heap_init<mbedtls_entropy_context, mbedtls_entropy_init>());
    std::unique_ptr<mbedtls_ctr_drbg_context, aero::pal::tls::detail::CtrDrbgDeleter> ctr_drbg(
        aero::pal::tls::detail::heap_init<mbedtls_ctr_drbg_context, mbedtls_ctr_drbg_init>());
    static constexpr char kPers[] = "aero_ota_crypto";
    const int rc = mbedtls_ctr_drbg_seed(ctr_drbg.get(), mbedtls_entropy_func, entropy.get(),
                                          reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers) - 1);
    if (rc != 0)
        return std::unexpected(aero::pal::tls::detail::mbed_err("ctr_drbg_seed", rc));
    return std::make_pair(std::move(entropy), std::move(ctr_drbg));
}

}  // namespace detail

// Sign `content`'s SHA-256 digest with the EC private key in `private_key_pem` (PEM, SEC1/PKCS8) —
// an offline/build-time operation; the signing key never needs to live on an edge node. Returns a
// hex-encoded DER ECDSA signature.
[[nodiscard]] inline std::expected<std::string, Error> ecdsa_sign_sha256(std::string_view content,
                                                                         std::string_view private_key_pem) {
    auto rng = detail::seeded_rng();
    if (!rng) return std::unexpected(Error{rng.error()});
    auto& [entropy, ctr_drbg] = *rng;

    std::unique_ptr<mbedtls_pk_context, aero::pal::tls::detail::PkDeleter> pk(
        aero::pal::tls::detail::heap_init<mbedtls_pk_context, mbedtls_pk_init>());
    // mbedtls_pk_parse_key requires the PEM buffer to include its trailing NUL — that's how it tells PEM
    // input (a NUL-terminated string) apart from raw DER.
    int rc = mbedtls_pk_parse_key(pk.get(), reinterpret_cast<const unsigned char*>(private_key_pem.data()),
                                   private_key_pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random,
                                   ctr_drbg.get());
    if (rc != 0) return std::unexpected(Error{aero::pal::tls::detail::mbed_err("parse private key", rc)});

    unsigned char hash[32];
    rc = mbedtls_sha256(reinterpret_cast<const unsigned char*>(content.data()), content.size(), hash, 0);
    if (rc != 0) return std::unexpected(Error{aero::pal::tls::detail::mbed_err("sha256", rc)});

    unsigned char sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    std::size_t sig_len = 0;
    rc = mbedtls_pk_sign(pk.get(), MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig), &sig_len,
                          mbedtls_ctr_drbg_random, ctr_drbg.get());
    if (rc != 0) return std::unexpected(Error{aero::pal::tls::detail::mbed_err("pk_sign", rc)});

    return detail::hex_encode(sig, sig_len);
}

// Verify `signature_hex` (as produced by ecdsa_sign_sha256) over `content`'s SHA-256 digest against the
// EC PUBLIC key in `trust_root_public_key_pem` — the operation an edge node actually performs (O1): it
// holds only the public trust root, never a signing key.
[[nodiscard]] inline std::expected<bool, Error> ecdsa_verify_sha256(std::string_view content,
                                                                    std::string_view signature_hex,
                                                                    std::string_view trust_root_public_key_pem) {
    aero::pal::tls::detail::ensure_threading_registered();

    auto sig = detail::hex_decode(signature_hex);
    if (!sig) return false;  // malformed signature encoding — not a hard error, just "does not verify"

    std::unique_ptr<mbedtls_pk_context, aero::pal::tls::detail::PkDeleter> pk(
        aero::pal::tls::detail::heap_init<mbedtls_pk_context, mbedtls_pk_init>());
    const int prc = mbedtls_pk_parse_public_key(
        pk.get(), reinterpret_cast<const unsigned char*>(trust_root_public_key_pem.data()),
        trust_root_public_key_pem.size() + 1);
    if (prc != 0)
        return std::unexpected(Error{aero::pal::tls::detail::mbed_err("parse public key", prc)});

    unsigned char hash[32];
    const int hrc =
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(content.data()), content.size(), hash, 0);
    if (hrc != 0) return std::unexpected(Error{aero::pal::tls::detail::mbed_err("sha256", hrc)});

    const int vrc = mbedtls_pk_verify(pk.get(), MBEDTLS_MD_SHA256, hash, sizeof(hash),
                                       reinterpret_cast<const unsigned char*>(sig->data()), sig->size());
    return vrc == 0;  // any non-zero mbedTLS result (bad sig, wrong key, tampered content) → "no match"
}

#else  // !AERO_TLS_ENABLED — compiled-out stub, identical surface, mirrors tls.hpp's own shape

[[nodiscard]] inline std::expected<std::string, Error> ecdsa_sign_sha256(std::string_view, std::string_view) {
    return std::unexpected(Error{"aero::pal::crypto: not compiled in (AERO_TLS_ENABLED=OFF)"});
}

[[nodiscard]] inline std::expected<bool, Error> ecdsa_verify_sha256(std::string_view, std::string_view,
                                                                    std::string_view) {
    return std::unexpected(Error{"aero::pal::crypto: not compiled in (AERO_TLS_ENABLED=OFF)"});
}

#endif  // AERO_TLS_ENABLED

}  // namespace aero::pal::crypto
