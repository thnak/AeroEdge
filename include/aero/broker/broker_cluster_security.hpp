// AeroEdge Broker M5.1 (017 §10) — `BrokerClusterSecurityConfig` + `load_cluster_identity()`: opt-in
// mTLS for `BrokerCluster`'s inter-node link (broker_cluster.hpp), over QuarkCpp's ADR-040 transport-
// security seam (`quark::SecureTransport` + `quark::adapters::MbedtlsHandshakeEngineFactory`/
// `MbedtlsAeadGcm`). Default-constructed (`certificate_chain_file` empty) == disabled: `BrokerCluster`
// then wires `DistributedRouter` straight to its plain `TcpTransport`, exactly as it did before M5.1 —
// every existing `BrokerClusterConfig` and deploy config keeps working unchanged.
//
// v1 SCOPE (deliberately narrow, matching every other slice in this codebase):
//   - ONE trusted CA/roots blob (`trusted_roots_file`), not per-peer pinning — every cluster peer's
//     leaf cert must chain to it.
//   - Certificate identity is bound to `CN=quark:<cluster_id>:<node_id>` (decimal) — QuarkCpp's own
//     `mbedtls_handshake.hpp` convention (Subject Common Name, not a SAN URI — a DIFFERENT convention
//     from this project's own OPC-UA M9.4 certs, which use a URI SAN; each subsystem follows whichever
//     upstream library it's securing dictates, not a project-wide house style).
//   - Cert/key material loaded from DER FILES on disk (leaf cert chain DER, PKCS8 private key DER, CA
//     chain DER) — mirrors `aero/pal/tls.hpp`'s own `cert_file`/`key_file` convention and M9.4's
//     `OpcUaSecurityConfig`, not inline bytes/base64 in a deploy config.
//   - NO certificate rotation/revocation sweep wiring in v1: `BrokerCluster` uses `StaticBrokerMembership`
//     (M6, a fixed, never-ticking roster), so there is no periodic-tick mechanism yet to hang
//     `SecureTransport::sweep_rotation()`/`sweep_revocations()` off of — the SAME v1 boundary 017 M5's
//     own southbound TLS drew (no cert rotation there either). A rotated/revoked cert only takes effect
//     on the NEXT fresh handshake (e.g. after a peer reconnects), not proactively.
//   - NO `set_reset_hook()`/`on_peer_disconnected()` wiring in v1: those need `TcpTransport::
//     reset_peer_connection()`/`set_peer_down_hook()`, hooks AeroEdge's own `aero::transport::
//     TcpTransport` (a from-scratch reimplementation, not QuarkCpp's own `net::TcpTransport`) does not
//     yet expose. `SecureTransport`'s own docs say this degrades gracefully — a stale session after a
//     connection drop just drops frames until the peer's natural reconnect triggers a fresh handshake —
//     not a hard failure, but a real v2 gap, not a completeness claim.
#pragma once

#include <cstdint>
#include <string>

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED

#include <fstream>
#include <memory>
#include <vector>

#include <mbedtls/sha256.h>

#include "quark/core/ids.hpp"          // quark::ClusterId, quark::NodeId
#include "quark/core/tls_identity.hpp"  // quark::TlsIdentity/TrustedRoots/IdentityMaterial/TrustStore

#endif  // AERO_TLS_ENABLED

namespace aero::broker {

struct BrokerClusterSecurityConfig {
    quark::ClusterId cluster_id{};  // must match every peer's own cluster_id — a mismatch fails the handshake
    std::string certificate_chain_file;  // this node's own leaf cert, DER — empty = disabled (v1 default)
    std::string private_key_file;        // matching private key, PKCS8 DER
    std::string trusted_roots_file;      // the cluster's CA/trust roots, DER
};

#if defined(AERO_TLS_ENABLED) && AERO_TLS_ENABLED

namespace cluster_security_detail {

[[nodiscard]] inline bool read_der_file(const std::string& path, std::vector<std::byte>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size <= 0) return false;
    out.resize(static_cast<std::size_t>(size));
    f.seekg(0);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), size));
}

}  // namespace cluster_security_detail

// Loads `sec`'s DER files into a fresh `IdentityMaterial`/`TrustStore` pair, ready for
// `SecureTransport::enable_handshake()`. Returns false on any read failure (caller treats that
// identically to any other config-time error — see `BrokerCluster::start()`). `identity_out`/
// `trust_out` are (re)constructed in place; any previous contents are discarded (this is a one-shot
// v1 load, not a rotation — see file banner).
[[nodiscard]] inline bool load_cluster_identity(const BrokerClusterSecurityConfig& sec,
                                                std::unique_ptr<quark::IdentityMaterial>& identity_out,
                                                std::unique_ptr<quark::TrustStore>& trust_out) {
    std::vector<std::byte> cert_bytes, key_bytes, ca_bytes;
    if (!cluster_security_detail::read_der_file(sec.certificate_chain_file, cert_bytes)) return false;
    if (!cluster_security_detail::read_der_file(sec.private_key_file, key_bytes)) return false;
    if (!cluster_security_detail::read_der_file(sec.trusted_roots_file, ca_bytes)) return false;

    // v1: `cert_chain_file` is exactly one leaf cert (no intermediates), so hashing the whole file IS
    // hashing the leaf — see file banner's "ONE trusted CA/roots blob" scope note (a chain with
    // intermediates would need parsing out just the leaf entry here; not needed for a self-signed-per-
    // node-off-one-CA v1 deployment).
    quark::Fingerprint fingerprint{};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(cert_bytes.data()), cert_bytes.size(),
                   reinterpret_cast<unsigned char*>(fingerprint.data()), /*is224*/ 0);

    auto identity = std::make_shared<const quark::TlsIdentity>(
        quark::TlsIdentity{std::move(cert_bytes), std::move(key_bytes), fingerprint});
    auto trust = std::make_shared<const quark::TrustedRoots>(quark::TrustedRoots{std::move(ca_bytes)});

    identity_out = std::make_unique<quark::IdentityMaterial>(std::move(identity));
    trust_out = std::make_unique<quark::TrustStore>(std::move(trust));
    return true;
}

#endif  // AERO_TLS_ENABLED

}  // namespace aero::broker
