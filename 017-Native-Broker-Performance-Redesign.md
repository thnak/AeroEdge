# 017 Follow-on: Native Broker I/O & Fan-out Architecture Redesign (WIP)

Status: research/design phase — no implementation started. This document is the living
record of that phase, per the working agreement: full feature inventory and validated
technical claims come first; no new production code lands until both are done.

## Background

`bench/broker/broker_bench.cpp` (merged in `e5481d7`) measured two real, unaddressed
bottlenecks in `NativeBroker` beyond the already-fixed linear session scan
(`route_publish()`'s topic index, same commit):

1. **Unbuffered, byte-at-a-time packet reads** — `mqtt_codec.hpp`'s `read_packet()` does
   3+ separate `recv()` syscalls per packet (1 for the fixed header, 1+ for the
   remaining-length varint, 1+ for the body), plus `NativeBroker::session_loop()` adds a
   4th (`wait_readable` poll) before every read. Measured ceiling: ~23K msg/s (QoS 1,
   round-trip-bound) / ~63K msg/s (QoS 0, backlog-bound) for a single publisher→single
   subscriber pair, no fan-out.
2. **Synchronous, serial, in-thread fan-out** — `route_publish()` writes to each matching
   session one at a time, inline, on the publishing session's own thread. Measured: 1→10
   subscribers dropped throughput ~7x (62K → 8.9K msg/s at QoS 0).

A first attempt at (2) — a small fixed-size worker pool + `std::latch` barrier — was
built, passed the test suite on early runs, then produced an intermittent severe stall
under repeated stress testing. Root cause was not conclusively isolated (evidence points
at Windows thread-creation-cost variability rather than a logic bug, but this was not
proven) and the change was reverted rather than shipped unverified. See conversation
history / commit `e5481d7` for the full trace.

**Working agreement for this round:** no more piecemeal patches. Full feature inventory
first, then a validated design (claims checked against real, isolated tests/benchmarks
by sub-agents), then implementation — in that order.

## Phase 1 — Feature inventory

Produced by 6 parallel research agents (624K tokens, 104 tool calls, all reading actual
current source — see `weh65del1` workflow journal for full per-agent transcripts).
Condensed below; nothing here is speculative, every claim carries a `file:line`.

### 1.0 The single biggest finding: a reactor already exists and NativeBroker doesn't use it

QuarkCpp (`D:/GitSrc/QuarkCpp`, AeroEdge's actor-engine dependency) ships a **complete,
cross-platform, already-battle-tested reactor** — `IoContext` — that `NativeBroker`
doesn't touch at all:

- **Windows**: `pal/windows_x86_64/net.hpp:355-517`. WSAPoll-based (not IOCP — the
  banner explicitly calls IOCP "a documented future upgrade behind this same class
  shape"). Cross-thread wake via a loopback TCP pair (no eventfd on Windows).
- **Linux**: `pal/linux_x86_64/net.hpp:282-421`. epoll-based, eventfd wake. Identical
  API shape by design ("mirrors this file's contract exactly").
- **API** (same signatures both platforms): `add_fd(fd, events, ReadyHandler)`,
  `mod_fd(fd, events)`, `del_fd(fd)`, `post_after(delay_ns, fn)` (timers),
  `post(fn)` (thread-safe task marshaling), `stop()` (thread-safe), `run()` (blocking
  loop, one dedicated thread).
- **Threading contract** (both banners state this explicitly): single-threaded —
  `run()` owns the loop; `add_fd/mod_fd/del_fd/post_after` are **loop-thread-only, not
  internally synchronized**; `post()`/`stop()` are the only two thread-safe entry
  points. "All fd/socket state stays on one thread with no locking on the I/O path."
- **Already used**: `quark/net/tcp_transport.hpp:1038` (one `IoContext` per
  `TcpTransport`, "ALL connection state lives on it"), `quark/net/voice_channel.hpp`
  (idle-timeout sweep via `post_after()`, chained work via `post()` continuations
  "explicitly so a busy voice room never starves other traffic sharing the same
  IoContext"), and a dedicated loopback test (`tests/voice_channel_loopback_test.cpp`).
- **AeroEdge's usage: zero.** The only hit for "IoContext" anywhere in
  `D:/GitSrc2/AeroEdge/include` is a *comment* — `aero/pal/poll.hpp:3-8` — explaining
  that AeroEdge's transport adapters (tcp/mqtt/grpc, **and by the same shape,
  `native_broker.hpp`**) are "blocking-thread-per-connection designs, not
  reactor-driven," and that `poll.hpp`'s `wait_readable`/`wait_writable` shim exists
  specifically as a substitute for adopting `IoContext`. `tcp_transport.hpp` and
  `grpc_client_transport.hpp` do the exact same `wait_readable(200)`-then-blocking-read
  pattern `native_broker.hpp` does.

**Feasibility for NativeBroker** (full detail in workflow journal, condensed here):
structurally the same migration `TcpTransport`/`VoiceChannel` already made.
`accept_loop()` → `add_fd(listen_fd_, EPOLLIN, handler)`; `session_loop()`'s one
thread-per-connection loop → a per-fd `ReadyHandler` reading into an owned buffer and
dispatching complete packets; the 200ms poll-as-keepalive-heartbeat idiom → a
`post_after()` timer per session, exactly voice_channel.hpp's existing pattern. If all
sessions share **one** `IoContext`, `Session::io_mu_`/`subs_mu_` could potentially be
*removed* (everything already serializes on the one loop thread) — but cross-session
`route_publish()` fan-out would then need `post()`-marshaling if sessions are ever
sharded across multiple `IoContext`s for throughput.

**What would NOT be a drop-in swap** — the real work:
1. `mqtt_codec.hpp`'s `read_packet()`/`read_n()` currently assume blocking-with-retry
   ownership of the fd for a whole packet (see §1.6 below) — this is the single biggest
   rewrite, and it's shared with `MqttClientTransport`, so the blast radius isn't
   contained to the broker.
2. `aero/pal/tls.hpp`'s `TlsSession`/`TlsServerContext` — whether it tolerates
   non-blocking partial handshake/record I/O suitable for reactor dispatch was **not
   verified this pass** (open question).
3. Collapsing N independent OS threads (natural per-connection fairness under the OS
   scheduler) onto one/few loop threads means a single slow handler (e.g.
   `route_publish()` fanning out inline) can stall every other connection on that loop —
   `voice_channel.hpp` already had to solve this exact problem (its "bounded, chained
   `post()` continuation — never inline" rule) and `route_publish()` would need the same
   treatment.
4. Both `native_broker.hpp` and `tcp_transport.hpp` explicitly document their current
   design as a deliberate, scoped tradeoff — this migration is correctly scoped as its
   own milestone, not an incidental patch, and arguably makes sense to do for all three
   transport adapters together (tcp/mqtt/grpc + broker) rather than three times over.

### 1.1 Core MQTT pub/sub (CONNECT → DISCONNECT)

| Feature | Path kind | Redesign risk |
|---|---|---|
| CONNECT/CONNACK negotiation (`handle_connect`, native_broker.hpp:559-801) | cold, setup | Low — once/connection; must stay the single choke point session creation flows through |
| SUBSCRIBE/SUBACK + ACL gate (`handle_subscribe`, :803-882) | cold, setup | Medium — tail feeds `index_subscription()`, the hot-path data structure |
| Topic filter matching (`topic_matches`, topic_match.hpp:19-44) | **hot, per-publish** | Re-splits both strings on every call, no caching — a redesign should precompile filters (trie) |
| Topic index (`exact_topic_index_`/`wildcard_subscribers_`, :1195-1248) | **hot, per-publish** | THE central hot-path data structure (this round's already-shipped fix); no UNSUBSCRIBE exists so entries only ever get added, bulk-removed at teardown |
| PUBLISH ingestion (`handle_publish`, :884-996) | **hot, per-publish** | Must preserve: ACL check strictly after alias resolution (security), QoS2-defer-to-PUBREL, "deliver before ack" ordering for QoS 0/1 |
| QoS 0/1/2 delivery (PUBACK/PUBREC/PUBREL/PUBCOMP) | **hot, per-publish** | `qos2_inflight` is session-scoped, single-thread-owned, no lock today — moving PUBLISH processing off the owning thread needs new synchronization here |
| Retained messages (:1032-1046, :869-880) | **hot, per-publish** (retain flag) | Single global `retained_mu_` — could become a contention point under high retained-traffic concurrency; currently orthogonal to the topic index |
| Last Will & Testament | background/rare | Low direct risk — but `teardown_session` MUST stay the single choke point across every teardown trigger (error/timeout/takeover/DISCONNECT) |
| Keep-alive (200ms poll + 1.5×keep_alive check) | background/rare | Orthogonal to fan-out, but a reactor migration must replace the poll-as-heartbeat idiom with a `post_after()` timer |
| Persistent sessions / takeover / offline queue | cold, setup **except** offline-queue routing | **Asymmetry**: `route_publish()`'s offline branch (stored_sessions_, QoS≥1) is a linear scan under one global lock — it never got the topic-index treatment the live path did. If persistent/offline clients are common this is an equally-hot, currently-untreated concern. |
| PINGREQ/PINGRESP, DISCONNECT | background/rare | Trivial, near-zero risk |
| `publish_to()` (:1108-1155) — the actual per-recipient send | **hot, per-publish, MAXIMUM risk** | The real head-of-line-blocking hazard: called synchronously once per matching session, on the *publisher's own thread*. Must preserve choke-point ordering (expiry check → build bytes → size check → send) and per-session `io_mu_` serialization. |
| Cross-node relay entry (`deliver_remote_publish`) | **hot, per-publish** | A SECOND entry point into `route_publish()` besides `deliver_publish()` — any redesign of `route_publish()`'s signature/threading must handle both callers |

### 1.2 MQTT 5 properties

Every v5 feature gates on `Session::protocol_version`, set once at CONNECT, read-only
thereafter, unlocked (single-thread-owned).

- **Session Expiry Interval** — cold (CONNECT/teardown only). Low risk.
- **Maximum Packet Size** — **hot, high risk**: enforced per-recipient inside
  `publish_to()` (native_broker.hpp:1143-1150) using the *true* wire size (varint
  remaining-length included), computed *after* Properties are written. Any
  serialize-once-blast-to-N-sockets optimization must special-case this per recipient.
- **Topic Alias** — **hot, high risk, security-relevant**: resolved strictly *before*
  the ACL gate (:935-940) so a numeric alias can never smuggle a topic past ACL. This
  ordering is a security invariant a redesign must not accidentally reorder.
- **Message Expiry Interval** — **HIGHEST-RISK item in the whole inventory**. Absolute
  deadline computed *once* at ingestion, re-checked *per recipient at actual send time*
  (not once per publish) — a message can legitimately expire mid-fan-out and be
  delivered to earlier-scanned subscribers but not later ones. This directly conflicts
  with any "serialize once, write to N sockets" batching unless the choke-point check
  and the remaining-seconds re-encode are deliberately kept per-recipient.
- **Response Topic / Correlation Data / User Properties** — **hot**, and the only v5
  surface that already escapes the MQTT socket path into the bridge/rule-engine fan-out
  (`on_publish_` → `BrokerRuleEngine` → `IBridgeSink`). Response Topic wildcard
  validation is asymmetric (disconnect on a regular PUBLISH vs. silent drop on a Will,
  since no CONNACK exists yet during Will parsing) — easy for a rewrite to flatten
  incorrectly.
- **CONNACK/SUBACK/DISCONNECT reason codes** — cold/low-frequency, low risk.
- **Subscription Identifier** — parsed, discarded, inert today. Flagged as the natural
  future hook point if per-subscription delivery tagging is ever wanted.
- **Property codec (`read_properties`/`PropertyWriter`, mqtt_codec.hpp)** — the shared
  substrate every v5 site depends on; fails closed (rejects) on any unrecognized
  property ID rather than guessing, because guessing wrong desyncs the parse cursor for
  every field after it in the same packet. A redesign touching buffer ownership must
  keep this contract intact.
- **Cross-node relay v5 gap** — `deliver_remote_publish()`/`BrokerRelayMsg` carry
  topic+payload+qos only; Message Expiry/Response Topic/Correlation Data/User
  Properties are silently dropped across a cluster hop today. Documented v1 cut, not a
  bug — but directly relevant if a redesign touches the relay wire shape.

### 1.3 Security

- **Dual listener** (`accept_loop`/`accept_loop_tls`) — cold, one thread each, no
  reactor. TLS handshake happens fully *before* `Session` construction/registration —
  load-bearing (`session_loop` assumes framing can start immediately).
- **`Session::channel` = `std::variant<PlainChannel, TlsChannel>`** — the exact seam a
  buffered-read/async-I/O redesign touches most directly (`io_channel.hpp`). Must
  preserve the `(recv_some/send_some/fd())` contract.
- **CRITICAL TLS subtlety**: `read_n()` (mqtt_codec.hpp) only polls `wait_readable()` on
  a would-block — it never polls `wait_writable()`. Harmless for `PlainChannel`. **Not
  harmless for `TlsChannel`**: `mbedtls_ssl_read()` can legitimately return
  `WANT_WRITE` (collapsed into the same `would_block()` by `TlsSession::recv_some`,
  tls.hpp:205-206) — `read_n`'s retry loop would then wait on readability when
  writability is what's actually needed. A reactor redesign (which must register one
  specific interest set, unlike a blind poll-then-retry loop) **must fix this
  explicitly**, not port the bug forward.
- **mbedTLS owns all raw-socket I/O for a TLS connection via its own BIO callbacks** —
  a redesign must never read raw bytes off a TLS fd directly into an app buffer; it
  must keep going exclusively through `mbedtls_ssl_read`/`write`. Buffering *decrypted*
  bytes across multiple `mbedtls_ssl_read` calls before parsing an MQTT packet is fine
  (that's exactly what Plain/TlsChannel already abstract over uniformly).
- **mTLS is a binary accept/reject gate only** — verified client cert CN/SAN never
  becomes a `principal`; mTLS identity and the Authenticator/Authorizer identity system
  are two completely independent, unlinked seams today (confirmed: no code extracts a
  cert subject anywhere).
- **Authenticator** (username/password → principal) — cold, CONNECT-time only, the M5
  auth gate is explicitly "the first thing after parsing that can cause an early
  return" so a rejected CONNECT leaves zero trace. Not hot-path relevant.
- **Authorizer/`TopicAclAuthorizer`** — **hot** (once per SUBSCRIBE filter, once per
  PUBLISH — the two exact paths this redesign targets), but **CPU-only, noexcept, no
  locking** (rules never mutated after construction) — orthogonal to any I/O change, a
  redesign can call `allow()` from wherever `handle_publish`/`handle_subscribe` end up
  running with zero seam changes, *provided* two invariants survive: ACL check strictly
  before any topic-state mutation, and (v5) topic-alias resolution strictly before ACL.

### 1.4 Bridging & clustering

**The single biggest recurring theme across bridge sinks**: `on_publish_` fires
*synchronously, inline, before `route_publish()`*, on the publishing session's own
thread. All three `IBridgeSink` implementations (`MqttBridgeSink`,
`HttpWebhookBridgeSink`, `RabbitMqBridgeSink`) do a **blocking write under a mutex**
(TCP publish, HTTP POST, AMQP publish respectively) — today a slow/dead sink only
stalls *that one publisher's own thread*. **This is exactly the same risk category that
killed the reverted fan-out-pool attempt** — if any of this were pooled onto shared
workers, one slow sink would stall unrelated publishers too, a strictly worse failure
mode than today's isolated-per-thread blocking.

**`BrokerCluster`'s inbound relay leg is the one existing precedent for off-session-thread
delivery**: `deliver_remote_publish()` runs on a dedicated, persistent, single-worker
Quark actor thread (started once at `BrokerCluster::start()`, never per-publish) — *not*
a session thread. This is a structurally different, and structurally safer, pattern than
the reverted per-call thread-spawn/pool-dispatch attempt (a persistent worker vs.
ad-hoc pooled dispatch) — **a hypothesis worth validating in Phase 2, not a proven fact**.
Important caveat: it's `workers=1` — sequential relay delivery, not parallel fan-out, so
it only demonstrates off-thread liveness, not parallel-fan-out liveness.

`route_publish()`/`deliver_publish()`/`publish_to()` themselves have **no dedicated unit
test** — only exercised transitively through higher-level suites (confirmed by
codegraph's blast-radius scan).

### 1.5 Test coverage — what exists, and the gaps that matter most

Full per-file breakdown (7 files, invariants extracted per test) is in the workflow
journal. The **structural pattern across every single test file**: one broker instance
per test, ≤5 live connections, exactly one publisher, fully synchronous
request/response client ops, no burst traffic, no concurrency stress. This is coverage
of *correctness*, not of *the failure modes a redesign would introduce*.

**Concrete gaps a redesign MUST close before landing** (ranked by relevance to what
happened with the prior fan-out-pool attempt):

1. **Concurrent publish-while-teardown races — untested.** This is precisely the
   scenario suspected (not proven) to have caused the prior stall. Existing tests
   *avoid* this race with artificial sleeps (`test_persistent_session`'s 150ms sleep,
   `test_connect_auth_rejection`'s 150ms sleep) rather than stress it.
2. **Multi-threaded/concurrent publishers — untested.** Every test publishes from one
   thread, one call at a time. No test proves `route_publish()` stays correct/ordered
   under N concurrent publisher threads — exactly what the current serial in-thread
   design makes safe *by construction*, and what any parallel redesign must prove anew.
3. **Slow/non-draining subscriber under concurrent publish load — untested.** No
   backpressure scenario exists at all. This is precisely the case an async fan-out
   needs a defined, tested policy for (block? drop? bounded queue?).
4. **Multi-packet-per-`recv()` burst framing — untested.** No test ever writes 2+
   packets back-to-back before the reader drains them, and none splits a packet across
   a read boundary. Zero regression coverage for "did framing stay byte-exact under
   buffering" — directly relevant to the read-buffering fix.
5. **Fan-out beyond 2-3 subscribers is benchmarked, never correctness-tested.** The
   1→10-subscriber 7x-drop finding lives only in `bench/broker/broker_bench.cpp`
   (prints numbers, not a pass/fail gate). No test asserts "all N subscribers receive
   exactly one correctly-ordered copy" at N≥10.
6. **Delivery ordering is never asserted anywhere.** Implicit today (serial in-thread
   routing) but nothing pins it down — a redesign has no test to catch an ordering
   regression.
7. **Connection/session churn stress — untested.** Nothing opens/closes many sessions
   rapidly, which is the exact shape needed to probe the "Windows thread-creation-cost
   variability" suspicion from the prior attempt.
8. **TLS + buffered reads interaction — untested.** Nothing exercises an MQTT packet
   spanning multiple TLS records, or multiple MQTT packets inside one TLS record.
9. **Send-side backpressure/partial-write — essentially untested** outside one
   sequential dead-connection case in `rabbitmq_bridge_sink.cpp`.

**Before any Phase 2 design claim is treated as validated, it needs a real test closing
the corresponding gap above** — this is the concrete meaning of "kiểm chứng thực tế"
for this redesign.

## Phase 2 — Design candidates + validated claims

**Working constraint from the human owner of this initiative:** separate code paths
per data-flow shape are explicitly acceptable — the public API (`Config`,
`NativeBroker`'s surface) should stay clean/small; the delivery internals behind it may
be larger/more specialized than a single unified mechanism, as long as each
specialization has a clear, measured efficiency reason to exist. This directly shapes
the proposal below: rather than one generic delivery path forced to serve every QoS/
fan-out/transport shape, the design intentionally splits into a small number of
purpose-built paths.

### 2.1 Proposed architecture: `IoContext`-based reactor + per-flow specialized delivery

Anchored on §1.0 — QuarkCpp's `IoContext` is adopted rather than reinvented. This is a
proposal, not yet validated (see §2.3).

**I/O layer (replaces thread-per-connection).** One or more `IoContext` loop(s)
(§2.2 below — sharding is the one genuinely open sizing question). Each `Session` owns
a private read buffer; `recv_some()` is called on `EPOLLIN`/readiness, and MQTT packets
are parsed **incrementally** out of that buffer (this replaces `mqtt_codec.hpp`'s
current blocking-with-retry `read_packet()`/`read_n()` for the broker's own read path —
a new pull-based, resumable-across-readiness-events parser, *additive*: existing
`read_packet()`/`read_n()` stay untouched for `MqttClientTransport`/bridge callers, per
§1.6/1.0's "additive, don't touch shared code for one caller's needs" precedent already
used for the topic index). Keep-alive becomes a `post_after()`-scheduled recurring
timer per session (mirrors `voice_channel.hpp`'s idle-timeout-sweep exactly) instead of
the 200ms poll idiom. TLS: buffering happens on **decrypted** bytes from repeated
`mbedtls_ssl_read()` calls — never raw fd bytes — preserving the existing
`PlainChannel`/`TlsChannel` `(recv_some/send_some/fd())` contract unchanged.

**Delivery paths, deliberately separated by shape:**

| Path | When it's taken | Why separate |
|---|---|---|
| **Single-recipient fast path** | `topic_index_candidates().size()==1` (already the common case per real telemetry patterns — one device, one topic) | Zero marshaling, zero chunking, zero extra bookkeeping — direct inline write, same as today's already-shipped fast path in `route_publish()`. |
| **QoS 0 fan-out** | `qos==0`, N>1 candidates | No packet-id, no per-recipient ack tracking, no offline-queue interaction (nothing to durably queue). Cheapest possible per-recipient cost; can be chunked/continuation-scheduled (see §2.1.1) without needing to track completion beyond "sent." |
| **QoS 1/2 fan-out** | `qos>=1`, N>1 candidates | Needs a real completion signal per recipient (this is what the offline-queue path already requires today) and the Message-Expiry/Max-Packet-Size choke points **recomputed per recipient at actual send time** (§1.2 — non-negotiable, highest-risk invariant). Kept as its own path so QoS 0's fast path never carries this overhead. |
| **Bridge/cluster fan-out** (`on_publish_`, `IBridgeSink`, `peer_forwarder_`) | Every publish, unconditionally | Moved **off the reactor loop thread(s) entirely** onto a small, dedicated, persistent worker (mirrors `BrokerCluster`'s already-proven single-actor-thread pattern, §1.4) — because these are inherently slow, blocking, external-I/O calls (network dial/write to a remote MQTT broker, HTTP POST, AMQP publish) that must never share a thread with latency-sensitive local delivery. A slow/dead bridge sink degrades to "this bridge's own queue backs up," never "unrelated local subscribers stop receiving." |
| **Retained / offline-queue path** | `retain==true` / `qos>=1` against `stored_sessions_` | Stays a simpler, lock-based path (Phase 1 found this orthogonal, lower-frequency) — **except** it should finally get the same topic-index treatment the live path already got in this round's shipped fix (Phase 1's documented asymmetry) — cheap, low-risk, proven pattern, no reason to leave it a linear scan. |

**2.1.1 Fan-out under one reactor thread — the head-of-line-blocking fix.**
`voice_channel.hpp` already solved this exact problem for Quark's own reactor usage:
"bounded, chained `IoContext::post()` continuation — never inline." Applied here: a
fan-out to N candidates is broken into small batches; each batch is written, then the
*next* batch is scheduled via `post()` rather than looping inline — so a large fan-out
interleaves with other connections' I/O on the same loop instead of hogging it. This
is the mechanism §2.3's Experiment B validates directly.

### 2.2 The one real open design question: single shared `IoContext` vs. sharded

Not resolved by Phase 1 research (flagged as an explicit open question — "what
connection-count scale is NativeBroker actually targeted for?" is undocumented).
Proposed resolution: **don't choose between them — make shard count a runtime
parameter, default low (1-2).** A single-shard deployment is just the N=1 degenerate
case of the same sharded design, so this isn't "build both," it's "build the sharded
one and default it small." Cross-shard delivery (a fan-out recipient living on a
*different* shard than the publisher) becomes its own micro-path — a `post()` hop —
which is the other concrete instance of "separate code path, justified by shape" the
owner's guidance calls for: same-shard delivery stays a direct call, cross-shard
delivery pays one marshaling hop, and the two are never conflated into one mechanism
that's fast for neither case.

### 2.3 Validation plan — real, isolated, before any of §2.1 is trusted

Three experiments, each producing actual measured numbers or a pass/fail result, none
touching `native_broker.hpp` (so none of this risks the shipped topic-index fix):

- **Experiment A — quantify the I/O-layer win.** Build a minimal `IoContext`-based
  prototype (reactor + per-connection buffer + incremental packet parsing, reusing
  `mqtt_codec.hpp`'s `Packet` shape) and benchmark it with the *same* methodology as
  `bench/broker/broker_bench.cpp` (1:1 no-fanout, and small fanout). If the ~23-63K
  msg/s single-connection ceiling and the ~7x fan-out drop measured against the current
  broker don't measurably improve, the whole premise needs revisiting before Phase 3.
- **Experiment B — prove the head-of-line-blocking fix actually works.** Extend
  Experiment A's prototype: N sessions on one `IoContext`, one deliberately
  non-draining "slow" consumer, a fast publisher blasting to all N via §2.1.1's
  bounded/chained `post()` fan-out. Measure whether the *other* N-1 consumers keep
  receiving at low, bounded latency despite the slow one — this is the concrete
  claim behind "async fan-out fixes head-of-line blocking," and it needs a real
  demonstration, not an inference from Quark's docs.
- **Experiment C — does a race already exist today, independent of any redesign?**
  A new, permanent test (closes Phase 1 gap #1) against the **current shipped**
  `native_broker.hpp`: many rounds of concurrent publish-while-teardown with no
  artificial sleeps (unlike every existing test), stressing exactly the scenario
  suspected — never proven — to have caused the reverted fan-out-pool attempt's
  stall. This resolves real lingering uncertainty ("was that a logic bug we'd
  reintroduce, or environmental Windows thread-creation variance we're free of no
  matter what we build next") and is valuable test coverage either way, so it should
  be proposed for merge regardless of what Phase 3 decides.

Deferred (lower priority, not blocking a Phase 3 recommendation): TLS non-blocking
compatibility with mbedTLS (real work, but AeroEdge's TLS usage is a smaller fraction
of traffic, and can be an explicit Phase 3 sub-task with its own validation rather than
a Phase 2 gate); cross-shard `post()` marshaling correctness (already covered by
Quark's own `voice_channel_loopback_test.cpp` — re-validating from scratch here would
mostly re-prove Quark's own test suite, low marginal value).

(Results of A/B/C to be appended below once the validation workflow completes.)

## Phase 3 — Implementation

(not started)
