// AeroEdge M9.4 — OpcUaSecurityConfig + apply_security_config(): shared Sign/SignAndEncrypt + cert-based
// client-auth wiring for OpcUaDriver (opcua_driver.hpp) and OpcUaSubscriptionDriver
// (opcua_subscription_driver.hpp), 018 §8's last v1 backlog item. See opcua_driver.hpp's file banner
// ("SECURITY POLICIES") for the full scope writeup (one trusted peer cert, Sign/SignAndEncrypt only, DER
// files on disk) — this header is intentionally just the config struct + the open62541 plumbing, kept out
// of both driver files so it isn't duplicated between them (mirrors how both drivers already each ported
// their own copy of variant_to_double() rather than needing this treatment — the security wiring is
// larger and genuinely identical between the two, so it earns the shared header they don't).
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

// Default-constructed = disabled (certificate_file empty) — every existing OpcUaDriver/
// OpcUaSubscriptionDriver call site and deploy config keeps working unchanged, connecting at
// MessageSecurityMode::None exactly as before M9.4.
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

}  // namespace opcua_security_detail

// Applies `sec` to a freshly-UA_Client_new()'d client's config, in place of the caller's own
// UA_ClientConfig_setDefault() call. `sec.certificate_file.empty()` (the default) == disabled: plain
// UA_ClientConfig_setDefault, MessageSecurityMode::None — identical to every OpcUaDriver/
// OpcUaSubscriptionDriver behavior before M9.4. Returns false on any load/config failure (missing/
// unreadable file, open62541 rejects the cert or key) — the caller treats that identically to any other
// open()-time failure (DriverStatus::Error, client left un-deleted for the caller's own cleanup path).
[[nodiscard]] inline bool apply_security_config(UA_ClientConfig* cc, const OpcUaSecurityConfig& sec) {
    if (sec.certificate_file.empty()) {
        return UA_ClientConfig_setDefault(cc) == UA_STATUSCODE_GOOD;
    }

    // MUST run before any other mbedTLS call this process makes. This project's vendored mbedTLS is
    // built with MBEDTLS_THREADING_ALT (cmake/patch_mbedtls.cmake, patch 3 — a real fix for a PSA
    // key-slot-table race under the native broker's own concurrent TLS handshakes), which requires
    // mbedtls_threading_set_alt() to run before any other mbedTLS call, full stop — NOT specific to TLS.
    // Without it, open62541's mbedTLS-backed SecurityPolicy setup (UA_ENABLE_ENCRYPTION=MBEDTLS) fails
    // outright: mbedtls_ctr_drbg_seed() returns MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED for every
    // policy, deterministically, on every call — confirmed empirically standing up a real security-
    // enabled UA_Server/UA_Client pair in this driver's own test (opcua_driver_security.cpp). This is the
    // real, load-bearing answer to 018 §8's open question about mbedTLS-sharing thread-safety: yes, a
    // hazard exists, and the fix is routing every mbedTLS-touching subsystem in this process through the
    // SAME registration call (std::call_once-guarded, so calling it again from aero/pal/tls.hpp's own
    // TlsServerContext::create() is a no-op either order).
    aero::pal::tls::detail::ensure_threading_registered();

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
    return true;
}

}  // namespace aero::drivers

#endif  // AERO_OPCUA_ENABLED
