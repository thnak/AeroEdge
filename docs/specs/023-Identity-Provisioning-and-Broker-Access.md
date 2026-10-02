# 023 — Identity, Provisioning and Broker Access

> Draft v0.2. Who may connect to AeroEdge, how they get credentials, and what they may publish
> or subscribe to. 017 shipped the **mechanisms** (TLS, optional mTLS, a pluggable
> `Authenticator` and `TopicAclAuthorizer`). This spec supplies the **policy and its source**:
> the device registry (022). It also lists the 017 changes that policy needs (§6).
>
> v0.2 incorporates the red-team review: per-unit claim proof, client-id binding, node-local
> revocation, scalable ACL, per-node service credentials. It also records the fleet decision:
> existing ESP32 devices are **re-flashed** to protocol v4 (028). There is no legacy
> MAC-identity mode.

## 1. Principals

| Principal | Examples | Lives in |
|---|---|---|
| **Device identity** | PLC gateway, ESP32 counter, sensor hub | registry, 1 per device (022) |
| **Service identity** | AeroOEE, AeroMes, a third-party integration | registry, separate namespace |
| **Node identity** | an edge node, toward the control plane and toward upstream (025) | registry, 1 per node |
| **User** | Studio operator, admin | `aero-api` auth (Quark 020). Out of scope here |

These namespaces are disjoint. A leaked service credential can't impersonate a device or a node,
and the reverse holds too.

## 2. Credentials

| Kind | Use |
|---|---|
| Username + secret | MQTT devices without certificate support |
| Token (bearer) | HTTP/WebSocket clients, services |
| Client certificate (mTLS, 017 M5) | devices and nodes that support it. Preferred |

Rules:

- Issued by AeroEdge. **Shown once** at issue time and stored only as a hash (secrets) or a
  fingerprint (certs). Hashing and at-rest protection use Quark 020 / the PAL TLS stack.
  AeroEdge doesn't roll its own crypto (007 §8).
- **Rotation:** a new credential can be issued while the old one stays valid for a grace
  period. **Revocation** is described in §5.
- Optional limits per credential: expiry, allowed source IPs, failed-attempt lockout
  (default 5 failures → 15 min, matching AeroOEE today).
- **CONNECT cost:** hash verification runs on a bounded worker pool with a per-IP and global
  rate limit. A plant-wide reconnect storm (1,000 devices after a node restart) is spread out
  instead of stalling the broker. Devices reconnect with jittered backoff (028 §3).

### 2.1 Importing AeroOEE credentials

Existing per-device MQTT credentials in AeroOEE (PBKDF2 hashes) are **imported as-is**, so a
re-flashed device (028) can keep its current secret:

- Imported only when the credential belongs to **exactly one** device (username ↔ MAC/device).
  Shared credentials are not imported. Those devices get a new credential at re-flash.
- The hash scheme (PBKDF2 parameters) is kept per credential and verified via the PAL crypto.
  On the device's next rotation, it's re-hashed with the current default scheme.
- Lockout, expiry and IP-allowlist settings carry over.

## 3. Provisioning flows

| Flow | Steps | When |
|---|---|---|
| **Manual** | operator registers device → issues credential → enters it on the device (or via the device's local setup page) | small sites, one-offs |
| **Bulk** | CSV import of devices (022 §6) → credentials generated → delivered as an **encrypted export** or as **one-time pickup tokens** (§3.2) | commissioning a line |
| **Claim** | per-unit proof → claim request → operator approval → credential delivered on the claimant's own session (§3.1) | fleets of identical devices, re-flashed fleet |

### 3.1 Claim with per-unit proof

A model-wide factory secret is **not** acceptable. Anyone who dumps one unit could read or
pre-claim every other unit. The claim flow is:

1. **Per-unit claim secret.** Each unit has its own claim key, burned at flashing time
   (re-flash station or factory). It's printed as a QR code on the unit label and stored in
   the device's secure storage. The flashing station registers `{device_id, claim key hash}` in
   bulk, so the device is `Registered (claimable)`.
2. The device connects to the **claim listener/topic** as client-id = its `device_id`,
   authenticating with the claim key. A claim principal can only publish its own claim request
   and subscribe to **its own** response topic (`aero/claim/{device_id}/response`, ACL'd to that
   client-id). It can't see any other device's response.
3. The operator approves in Studio, either by scanning the label or by bulk-approving a
   flashing-station batch. The approval can't be satisfied by data the device itself supplied.
4. The credential is delivered **only on that session**, over TLS (MQTT 5 Response Topic /
   Correlation Data, already shipped in 017 M7.2). The claim key is then invalidated.
5. Pending claims **expire** (default 24 h) and are **rate-limited** per source IP and globally.
   Unknown `device_id`s are rejected without creating records, so the event-sourced log can't
   be flooded.

### 3.2 Bulk delivery

Plaintext CSVs of secrets don't leave the control plane. Bulk credentials are delivered either:
- as an export **encrypted to an installer's public key**, or
- as **one-time pickup tokens**: the installer or flashing tool exchanges each token for that
  device's credential once, and the token is then dead.

## 4. Broker access from identity

### 4.1 Client-id binding

- For device principals, **MQTT client-id MUST equal `device_id`**. A CONNECT whose client-id
  doesn't match the authenticated principal is refused.
  Without this rule, device A could connect as device B's client-id and repeatedly take over
  B's session (017 session takeover is keyed by client-id). That would be a cross-device denial
  of service, even though topic ACLs stop A publishing as B.
- `device_id` charset is restricted (022 §6), so it's safe inside topic names.

### 4.2 Topic namespace and templated ACL

```text
device namespace:   aero/{site}/{device_id}/#
   device may PUBLISH   aero/{site}/{device_id}/up/#      (telemetry, heartbeat, replies, requests)
   device may SUBSCRIBE aero/{site}/{device_id}/down/#    (commands, config, OTA, responses)
service namespace:  per grant (e.g. AeroOEE: subscribe on canonical events, 025 §4)
```

The ACL is a **constant set of templated rules** with `{principal}` / `{site}` substitution
(§6). It's not a rule list that grows with the device count, because the shipped authorizer
scans its rules linearly on every PUBLISH. 5,000 devices × 2 literal rules each would put
10,000 rule checks on the hot path.

- The `Authenticator` resolves a CONNECT to a principal through the node's registry projection
  (022 §4). For mTLS it resolves the **peer-certificate fingerprint**, not a username (§6).
- Service grants (topic filters, max QoS, retain allowed) are per-service rules, few in number.

## 5. Revocation

Revocation must work even when the control plane can't reach a node (027 §3).

| Path | Effect |
|---|---|
| **Normal**: control plane revokes | new projection version pushed. Each node **acks** it. Studio shows per-node status ("revoked on A, pending on B") |
| **Emergency**: node-local revoke via that node's `aero-api` | takes effect on that node immediately, is recorded locally, and is reconciled into the control plane when it's reachable |
| **Staleness bound** | credential projections carry a **max age** (default 7 days, configurable, can be off for air-gapped sites). A node whose projection is older than that refuses new CONNECTs for non-cached principals and raises an alarm |

On revocation the node **disconnects live sessions of that principal** (kick-by-principal) and
**re-checks existing subscriptions** against the new rules (§6).

## 6. Required 017 broker changes

These are amendments to 017's shipped seams, not new seams:

| Change | Why |
|---|---|
| `Authenticator` receives the **peer certificate** (fingerprint / subject) in addition to username/password | mTLS → principal mapping (§4.2) |
| `TopicAclAuthorizer` supports **`{principal}` / `{site}` substitution** in rule filters | constant rule set, O(rules) independent of device count (§4.2) |
| **Client-id ↔ principal check** at CONNECT | §4.1 |
| **Kick-by-principal** API | revocation (§5) |
| **Subscription re-check** on rule change | existing subscriptions must not outlive a revoked grant (§5) |
| **Hot swap** of authenticator/authorizer data from a new projection | no broker restart on config change |
| **Bounded hash-verification pool + connect rate limit** | reconnect storms (§2) |

## 7. Invariants (normative)

- **IA1**: every connection resolves to exactly one principal from the registry projection.
  Anonymous access is off unless a deployment explicitly enables it for a listener. None of
  AeroOEE's anonymous IoT/MQTT/OTA endpoints are carried over.
- **IA2**: a device can publish and subscribe only inside its own namespace. Cross-device topics
  are never granted to a device identity.
- **IA3**: secrets are never stored, logged or exported in plaintext. They're shown once, or
  delivered encrypted or by one-time pickup.
- **IA4**: revocation takes effect immediately on every **reachable** node. On an unreachable
  node it takes effect by node-local revoke or by projection max-age. Live sessions are
  terminated and subscriptions re-checked.
- **IA5**: device, service and node identities are disjoint namespaces.
- **IA6**: for device principals, MQTT client-id = `device_id`.
- **IA7**: claim proofs are per unit. A credential is delivered only on the claimant's own
  authenticated session.

## 8. Implementation status

Mechanisms shipped in 017 (TLS, mTLS, `Authenticator`, `TopicAclAuthorizer`, MQTT 5 response
topic). Not started: everything in §6, the registry-backed authenticator, templated ACL,
issue/rotate/revoke API, claim flow, AeroOEE credential import.

## 9. Open questions

- Certificate issuance: an AeroEdge-internal CA, or customer PKI only? Per-unit claim keys work
  without a CA.
- Enhanced/SASL auth in MQTT 5 (deferred in 017): still needed now that the claim flow uses a
  per-unit key over TLS?
- Default projection max-age for credentials: 7 days, or tied to the outage-buffer target (026)?
