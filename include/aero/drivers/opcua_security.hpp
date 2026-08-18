// AeroEdge M9.4/M9.5 — OpcUaSecurityConfig + apply_security_config(): shared Sign/SignAndEncrypt +
// cert-based client-auth + session-level X.509 UserIdentityToken wiring for OpcUaDriver
// (opcua_driver.hpp) and OpcUaSubscriptionDriver (opcua_subscription_driver.hpp). See opcua_driver.hpp's
// file banner ("SECURITY POLICIES") for the full M9.4 scope writeup (one trusted peer cert,
// Sign/SignAndEncrypt only, DER files on disk) — this header is intentionally just the config struct + the
// open62541 plumbing, kept out of both driver files so it isn't duplicated between them (mirrors how both
// drivers already each ported their own copy of variant_to_double() rather than needing this treatment —
// the security wiring is larger and genuinely identical between the two, so it earns the shared header
// they don't).
//
// M9.5 (018 §8's remaining backlog item after M9.4) adds `user_certificate_file`/`user_private_key_file`:
// a session-level X.509 UserIdentityToken (OPC-UA Part 4 §7.36.5), NOT the same thing as M9.4's
// certificate_file/private_key_file. M9.4's cert authenticates the SecureChannel itself (the client's cert
// IS the channel's identity, verified during the handshake); THIS is a separate, session-scoped identity
// presented during ActivateSession, orthogonal to whatever the channel's own MessageSecurityMode is — a
// deployment can use one, the other, both, or neither. Built entirely on open62541's own
// `UA_ClientConfig_setAuthenticationCert` (client_config_default.h), which does the real work (constructs
// the X509IdentityToken, and separately provisions `authSecurityPolicies` so ActivateSession can sign the
// server's nonce as proof of private-key possession) — this header just loads the DER files and forwards
// them, the same shape as every other cert-from-disk knob in this file. Same gate as
// `UA_ClientConfig_setAuthenticationCert` itself: `UA_ENABLE_ENCRYPTION_MBEDTLS` (or `_OPENSSL`), already
// satisfied by M9.4's own build requirement below — no new CMake work needed.
//
// REQUIRES UA_ENABLE_ENCRYPTION=MBEDTLS at open62541's build time (root CMakeLists.txt's AERO_ENABLE_OPCUA
// block) — sharing this project's own already-vendored mbedTLS (017 M5), not a second copy. With
// AERO_ENABLE_OPCUA off entirely, this header still compiles (the OpcUaSecurityConfig struct itself needs
// no open62541 types) — only apply_security_config() is gated, matching opcua_driver.hpp/
// opcua_subscription_driver.hpp's own AERO_OPCUA_ENABLED stub split.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace aero::drivers {

// Default-constructed = disabled (every field empty) — every existing OpcUaDriver/OpcUaSubscriptionDriver
// call site and deploy config keeps working unchanged: MessageSecurityMode::None channel, implicit
// Anonymous session (open62541's own default when userIdentityToken is left unset).
struct OpcUaSecurityConfig {
    std::string certificate_file;                    // this client's own cert, DER
    std::string private_key_file;                    // matching private key, DER
    std::string trusted_server_certificate_file;      // ONE trusted peer cert, DER (v1 scope, see banner)
    bool sign_and_encrypt = true;                     // true=SignAndEncrypt, false=Sign
    std::string security_policy_uri;                  // empty -> Basic256Sha256 (below)
    // MUST match certificate_file's own X.509 URI Subject Alternative Name, byte-for-byte — open62541's
    // mbedTLS plugin verifies a peer's claimed ApplicationUri against its cert via a raw substring search
    // of the cert's v3 extension bytes (plugins/crypto/mbedtls/ua_pki_mbedtls.c) and the SERVER side of
    // this same check rejects a client whose claimed clientDescription.applicationUri doesn't match its
    // presented cert with BadCertificateUriInvalid. Empty (the disabled-security default) leaves
    // clientDescription.applicationUri at whatever UA_ClientConfig_setDefault already set.
    std::string application_uri;

    // M9.5 — session-level X.509 UserIdentityToken (see this file's own banner for why this is a SEPARATE
    // mechanism from certificate_file/private_key_file above, not a reuse of them). Empty (the default)
    // means an implicit Anonymous session, exactly pre-M9.5 behavior — independent of certificate_file:
    // either, both, or neither may be set. `user_private_key_file` is never sent on the wire; it only
    // signs the server's nonce during ActivateSession as proof the client actually holds the private key
    // matching `user_certificate_file`'s public key.
    std::string user_certificate_file;   // session identity cert, DER
    std::string user_private_key_file;   // matching private key, DER
};

}  // namespace aero::drivers

#if defined(AERO_OPCUA_ENABLED) && AERO_OPCUA_ENABLED

#include <open62541/client.h>
#include <open62541/client_config_default.h>

#include "aero/pal/tls.hpp"  // ensure_threading_registered() — see apply_security_config()'s banner

namespace aero::drivers {

namespace opcua_security_detail {

[[nodiscard]] inline bool load_der_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size <= 0) return false;
    out.resize(static_cast<std::size_t>(size));
    f.seekg(0);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), size));
}

// Borrows `bytes` — the returned UA_ByteString is only valid as long as `bytes` is alive/unmoved. Safe
// here because every caller below passes a UA_ByteString straight into an open62541 config call that
// copies the bytes internally before returning (confirmed against open62541's own
// examples/encryption/client_encryption.c, which UA_ByteString_clear's its equivalents immediately after
// the UA_ClientConfig_setDefaultEncryption() call returns).
[[nodiscard]] inline UA_ByteString borrow_ua_bytestring(const std::vector<std::uint8_t>& bytes) noexcept {
    UA_ByteString s;
    s.length = bytes.size();
    s.data = const_cast<UA_Byte*>(bytes.data());
    return s;
}

// M9.5: session-level X.509 UserIdentityToken (see this file's own banner). No-op (returns true,
// `cc->userIdentityToken` left untouched -> implicit Anonymous) when `sec.user_certificate_file` is empty
// — every pre-M9.5 caller and deploy config is unaffected regardless of which branch of
// apply_security_config() below it takes. Independent of certificate_file/private_key_file (M9.4's
// SecureChannel identity) by construction — this is called from BOTH of apply_security_config()'s
// branches, not folded into the channel-cert-enabled one.
[[nodiscard]] inline bool apply_user_identity_token(UA_ClientConfig* cc, const OpcUaSecurityConfig& sec) {
    if (sec.user_certificate_file.empty()) return true;

    std::vector<std::uint8_t> cert_bytes, key_bytes;
    if (!load_der_file(sec.user_certificate_file, cert_bytes)) return false;
    if (!load_der_file(sec.user_private_key_file, key_bytes)) return false;

    const UA_ByteString cert = borrow_ua_bytestring(cert_bytes);
    const UA_ByteString key = borrow_ua_bytestring(key_bytes);
    return UA_ClientConfig_setAuthenticationCert(cc, cert, key) == UA_STATUSCODE_GOOD;
}

}  // namespace opcua_security_detail

// Applies `sec` to a freshly-UA_Client_new()'d client's config, in place of the caller's own
// UA_ClientConfig_setDefault() call. `sec.certificate_file.empty()` (the default) == disabled SecureChannel
// cert auth: plain UA_ClientConfig_setDefault, MessageSecurityMode::None — identical to every
// OpcUaDriver/OpcUaSubscriptionDriver behavior before M9.4. `sec.user_certificate_file` (M9.5, session-level
// UserIdentityToken) is independent of that branch and applied either way — see
// apply_user_identity_token()'s own comment. Returns false on any load/config failure (missing/unreadable
// file, open62541 rejects the cert or key) — the caller treats that identically to any other open()-time
// failure (DriverStatus::Error, client left un-deleted for the caller's own cleanup path).
[[nodiscard]] inline bool apply_security_config(UA_ClientConfig* cc, const OpcUaSecurityConfig& sec) {
    // MUST run before any other mbedTLS call this process makes, regardless of which branch below actually
    // touches mbedTLS (M9.4's channel cert, M9.5's user cert, or neither) — cheapest to just always pay it
    // up front. This project's vendored mbedTLS is built with MBEDTLS_THREADING_ALT
    // (cmake/patch_mbedtls.cmake, patch 3 — a real fix for a PSA key-slot-table race under the native
    // broker's own concurrent TLS handshakes), which requires mbedtls_threading_set_alt() to run before any
    // other mbedTLS call, full stop — NOT specific to TLS. Without it, open62541's mbedTLS-backed
    // SecurityPolicy setup (UA_ENABLE_ENCRYPTION=MBEDTLS) fails outright: mbedtls_ctr_drbg_seed() returns
    // MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED for every policy, deterministically, on every call —
    // confirmed empirically standing up a real security-enabled UA_Server/UA_Client pair in this driver's
    // own test (opcua_driver_security.cpp). This is the real, load-bearing answer to 018 §8's open question
    // about mbedTLS-sharing thread-safety: yes, a hazard exists, and the fix is routing every
    // mbedTLS-touching subsystem in this process through the SAME registration call (std::call_once-guarded,
    // so calling it again from aero/pal/tls.hpp's own TlsServerContext::create() is a no-op either order).
    aero::pal::tls::detail::ensure_threading_registered();

    if (sec.certificate_file.empty()) {
        if (UA_ClientConfig_setDefault(cc) != UA_STATUSCODE_GOOD) return false;
        return opcua_security_detail::apply_user_identity_token(cc, sec);
    }

    std::vector<std::uint8_t> cert_bytes, key_bytes, trust_bytes;
    if (!opcua_security_detail::load_der_file(sec.certificate_file, cert_bytes)) return false;
    if (!opcua_security_detail::load_der_file(sec.private_key_file, key_bytes)) return false;

    UA_ByteString trust_entry{};
    bool have_trust = false;
    if (!sec.trusted_server_certificate_file.empty()) {
        if (!opcua_security_detail::load_der_file(sec.trusted_server_certificate_file, trust_bytes)) {
            return false;
        }
        trust_entry = opcua_security_detail::borrow_ua_bytestring(trust_bytes);
        have_trust = true;
    }

    const UA_ByteString cert = opcua_security_detail::borrow_ua_bytestring(cert_bytes);
    const UA_ByteString key = opcua_security_detail::borrow_ua_bytestring(key_bytes);
    const UA_StatusCode rc = UA_ClientConfig_setDefaultEncryption(
        cc, cert, key, have_trust ? &trust_entry : nullptr, have_trust ? 1 : 0, nullptr, 0);
    if (rc != UA_STATUSCODE_GOOD) return false;

    cc->securityMode =
        sec.sign_and_encrypt ? UA_MESSAGESECURITYMODE_SIGNANDENCRYPT : UA_MESSAGESECURITYMODE_SIGN;
    const std::string uri = sec.security_policy_uri.empty()
                                 ? "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256"
                                 : sec.security_policy_uri;
    UA_String_clear(&cc->securityPolicyUri);
    cc->securityPolicyUri = UA_STRING_ALLOC(uri.c_str());

    if (!sec.application_uri.empty()) {
        UA_String_clear(&cc->clientDescription.applicationUri);
        cc->clientDescription.applicationUri = UA_STRING_ALLOC(sec.application_uri.c_str());
    }
    return opcua_security_detail::apply_user_identity_token(cc, sec);
}

}  // namespace aero::drivers

#endif  // AERO_OPCUA_ENABLED
