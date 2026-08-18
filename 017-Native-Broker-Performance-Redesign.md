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

### 2.4 Validation results — all three experiments, real measured numbers

All three built and ran real code against real loopback sockets; none touched
`native_broker.hpp`. Two ran clean via the validation workflow; the third (C) needed a
manual finish (its agent ran out of turns mid-stress-test-loop without reporting — the
built test itself was left in a working state and was picked up and run directly).

**Experiment A — I/O-layer throughput, CONFIRMED, high confidence.** A minimal
`IoContext`-based prototype (per-connection owned read buffer, MQTT packets parsed
incrementally out of it — the buffered-read fix — vs. the current per-syscall
`read_packet()`/`read_n()`), same 1-publisher→1-subscriber/no-fan-out/QoS-0 shape as
the documented baseline (~63,000 msg/s, ~44ms p50, backlog-bound), 3 runs:

| Run | Throughput | p50 | p99 | max |
|---|---|---|---|---|
| baseline (current broker) | ~63,000 msg/s | ~44ms | — | — |
| prototype run 1 | 120,469 msg/s | 6.54ms | 14.36ms | 16.22ms |
| prototype run 2 | 105,395 msg/s | 6.41ms | 13.08ms | 15.35ms |
| prototype run 3 | 107,327 msg/s | 7.02ms | 15.99ms | 17.83ms |

**~1.7-1.9x throughput, ~6-7x lower p50 latency**, 100% delivery, zero errors, all
three runs. The latency win is the more decisive number — it points at the
multi-syscall-per-packet read pattern (plus the 200ms poll-as-heartbeat idiom) as the
real driver of the baseline's 44ms p50, not some other untouched bottleneck. Caveat:
deliberately minimal first cut (CONNECT/CONNACK + single-recipient forwarding only —
no SUBSCRIBE/topic-matching/QoS1-2/TLS/keep-alive/sharding), so it isolates the
I/O-layer win specifically, not the full migration's net effect once every other
subsystem is layered back on top of the same reactor thread.

**Experiment B — head-of-line-blocking fix, CONFIRMED, high confidence, and the
strongest result of the three.** 10 subscribers + 1 publisher on one `IoContext` loop,
1 subscriber deliberately non-draining after its first 5 messages. Same delivery/drop
outcome to the slow recipient in both modes (501/6000 dropped — the fix changes
nothing about how the slow client itself is treated), but the effect on the other 9
*healthy* subscribers:

| Fan-out strategy | Healthy-subscriber p50 | p99 | max | Publish throughput |
|---|---|---|---|---|
| Naive inline loop | 1630ms | 1996ms | 2011ms | 102 msg/s |
| Bounded/chained `post()` | **14.0ms** | **16.7ms** | **17.1ms** | **9,626 msg/s** |

**~116-119x latency reduction**, reproduced with a second, independent parameter set
(tighter retry cap, fewer messages: 519ms → 23.6ms p50, ~22x). The naive mode's
publish-side throughput also collapsed 94x (102 vs 9,626 msg/s) — because the inline
stall on the slow subscriber's socket also blocks the reactor from reading the
publisher's *next* PUBLISH, so the stall propagates backward through TCP backpressure
too, not just forward to the other subscribers. This is real, reproduced evidence for
§1.0's claim that a slow handler stalls "every other connection on that loop" — not an
inference from documentation, an observed effect. §2.1.1's proposed fix demonstrably
works, at this N=10/single-shard scale.

**Experiment C — does a concurrent publish-vs-teardown race exist today? NO, not
detectably, high confidence.** New permanent test, `tests/broker/concurrency_stress.cpp`
(merged this round — see commit history): against the **current, unmodified, already-
shipped** `native_broker.hpp`, 3 publisher threads tight-loop QoS-1 PUBLISH with zero
sleeps while a separate thread continuously connects-subscribes-abruptly-disconnects
(no clean DISCONNECT, forcing `teardown_session`'s non-clean path) for a sustained 12s
window, while 3 steady never-torn-down subscribers must keep receiving throughout.

Run **16 times** (1 initial + 15 repeated, each a fresh process invocation): **16/16
passed cleanly.** Across the 16 runs: ~285-298 teardown rounds per run (~4,600 total),
~2,300-2,400 QoS-1 publishes delivered per steady subscriber per run (~37,000 total
publish/deliver round trips), zero publish failures, zero connect/subscribe failures,
zero hangs, zero crashes, every run's post-stress round trip succeeded. This is
meaningful evidence — not proof, but real repeated stress at real volume — that the
prior reverted fan-out-pool attempt's stall was **not** a latent race already present
in the current serial/synchronous design being newly exposed by concurrency; the
current design handles this exact stress shape without incident. This makes the
"Windows thread-creation-cost variability" explanation from that prior incident more
credible than "we already had a hidden logic bug," though it doesn't fully rule out a
race specific to the *reverted pool design itself* (which no longer exists to
re-test). This test is now permanent coverage regardless of what Phase 3 builds.

**Net read on Phase 2 as a whole:** the core premise holds up under real, adversarial
testing, not just the documentation/precedent case in §1.0. Recommend proceeding to
Phase 3 scoping (a concrete implementation plan, starting with the I/O layer + the
single-recipient and QoS-0 fan-out paths, per §2.1's table) rather than further
validation — the open items flagged in §2.3 (TLS non-blocking compatibility,
cross-shard `post()` correctness) remain real but lower-priority, addressable as
Phase 3 sub-tasks with their own focused validation rather than blocking gates here.

*(Process note: the Experiment A/B prototype source files were inadvertently deleted
during worktree cleanup before being copied out of their isolated worktrees — an
avoidable operational mistake. The measured numbers and conclusions above are intact
(captured in the workflow journal before cleanup); the prototype code itself would
need to be rewritten if wanted as a standing reference artifact. Experiment C's test
file was copied out first and is preserved/merged.)*

## Phase 3 — Implementation

### 3.0 Scoping: what ships now vs. deferred (post-critique)

An initial Phase 3 draft proposed implementing BOTH Phase 2 wins as smaller,
non-reactor, thread-per-connection-preserving changes: buffered reads (§2.4
Experiment A's mechanism) and a per-session outbound queue for fan-out (an
approximation of §2.4 Experiment B's mechanism, without adopting `IoContext`).

A Plan-agent critique of that draft found the buffered-read piece sound and
low-risk, but found **real, concrete correctness bugs** in the fan-out piece as
originally scoped:
1. Queuing pre-serialized packet bytes breaks the Message-Expiry-Interval
   invariant (`publish_to()`, native_broker.hpp — the single highest-risk item
   in the whole §1.2 inventory) — the expiry/remaining-seconds check would only
   ever run once, at enqueue time, not at actual flush time (which could be much
   later for a backed-up recipient).
2. It needs a new dedicated mutex, an explicit ordering-preservation rule
   (queued items must never be leapfrogged by a later inline send to the same
   recipient), and every write to `Session::channel` — including the new
   non-blocking attempt — must still route through the existing `io_mu` or it
   risks wire corruption.
3. Even fixed, its poll-cycle-driven flush (~200ms granularity, since it piggy-
   backs on `session_loop`'s existing `wait_readable(200)` cadence rather than
   an event-driven wakeup) would NOT match Experiment B's measured ~14ms p50
   recipient latency — it would only remove the publisher-side stall, not
   reproduce the recipient-side latency win. Framing it as "captures Experiment
   B's result" would overstate what it actually delivers.

**Decision:** ship only the buffered-read change this round (§3.1 below) — the
larger, more broadly-applicable win (helps every connection, not just the
high-fan-out-with-a-slow-consumer case), already independently critiqued as
low-risk and well-scoped, touching neither locking, `teardown_session`, TLS, nor
keep-alive timing. The fan-out/outbound-queue change is deferred to a future
phase with a corrected design: queue `{topic, payload, qos, retain, extras}`
(`QueuedMessage`-shaped, matching the precedent already established for
`StoredSession`'s offline queue — not pre-serialized bytes), re-run the full
choke-point sequence at actual flush time, add a dedicated `outbox_mu`, enforce
the ordering rule, and scope the latency claim honestly against what it can and
can't match from Experiment B. `route_publish()`'s offline-queue topic-index
parity (the §1.1 asymmetry) and the full `IoContext` reactor migration both
remain separately deferred, per §2.4's own recommendation.

### 3.1 Buffered incremental packet reads

Status: **shipped.**

- **Step A** — `try_parse_packet()` added to `mqtt_codec.hpp`, additive alongside
  the unchanged `read_packet()`/`read_n()`. Unit-tested in isolation
  (`tests/transport/mqtt_codec.cpp`): empty/partial/malformed/burst/split buffers,
  zero-length body, multi-byte remaining-length — all pass.
- **Step B** — wired into `NativeBroker::session_loop()`: a per-`Session` owned
  `read_buf`/`read_pos`, one `recv_some()` burst per outer poll cycle, an inner
  loop draining every complete packet currently buffered (with `kicked`
  re-checked per packet, not just per outer iteration, per the critique's
  finding). `read_packet()`/`read_n()` stay untouched for every other caller.

**Verification, all green:**
- Full existing broker suite (`native_broker`, `mqtt5`, `broker_cluster`, `acl`,
  `bridge`, `concurrency_stress`) — including `test_disconnect_reason_session_
  taken_over` (mqtt5.cpp), the existing regression coverage for exactly the
  `kicked`-promptness concern the critique raised.
- New `tests/broker/buffered_read_framing.cpp` (broker-integration level, closes
  Phase 1 gap #4 alongside the unit tests above): two packets delivered in one
  burst (dispatched correctly, in order), one packet split across two separate
  socket writes (dispatched correctly once complete), and session takeover
  staying prompt (<2s) with a 50-packet backlog buffered ahead of it. 5/5 repeat
  runs clean, no flakiness.
- `concurrency_stress` run 18x independently (same methodology as Phase 2's
  Experiment C): 18/18 clean, zero hangs, zero failures.

**Real measured throughput/latency, `broker_bench`, 1 publisher → 1 subscriber,
no fan-out** (vs. the documented pre-Phase-3 baseline):

| Scenario | Throughput | p50 latency |
|---|---|---|
| QoS 0 baseline (pre-Phase-3) | ~63,000 msg/s | ~44ms |
| QoS 0, buffered reads (3 runs) | 93,699 / 99,248 / 95,069 msg/s | 29.7 / 28.5 / 29.0 ms |
| QoS 1 baseline (pre-Phase-3) | ~22,700 msg/s | ~0.031ms |
| QoS 1, buffered reads | 22,446 msg/s | 0.032ms |

**QoS 0: ~1.5-1.6x throughput, ~1.5x lower p50 latency** — real and consistent
across repeat runs, but smaller than Experiment A's isolated-prototype numbers
(~1.7-1.9x / ~6-7x). Honest read: the prototype deliberately measured ONLY the
read-path change (CONNECT/CONNACK + single-recipient forwarding, no
SUBSCRIBE/topic-matching/ACL/retained-message overhead); the real, integrated
broker still pays for everything else on the per-message path unchanged by this
slice (topic index lookup, `subs_mu` locking, the ACL gate even when unset,
`retained_mu_`), which dilutes the isolated win. Still a genuine, worthwhile
improvement, just not the full isolated-prototype magnitude — expected and
consistent with what's actually in scope for this slice (§3.0).

**QoS 1: no measurable change (expected, not a regression).** QoS 1 is
round-trip-bound — the publisher waits for a PUBACK before sending the next
message, so at most one packet is ever in flight, and the buffered-read win
(amortizing read overhead across many packets available at once) has nothing to
amortize over. This matches the mechanism exactly: buffered reads help
unthrottled/bursty traffic (QoS 0, or QoS 1/2 fan-out to many subscribers under
load), not single-in-flight round-trip-bound traffic.

### 3.2 Real comparison against Mosquitto (same machine, same tool, same run)

Prompted by an earlier question ("MQTTnet claims 700K msg/s — how does this
compare?") — that figure is virtually always *aggregate throughput across many
concurrent connections/cores*, not a single-connection number, so comparing it
directly to §3.1's single-pair figures is apples-to-oranges. Instead of
reasoning from published/marketing numbers, `bench/broker/broker_bench.cpp`
gained a `--external-port N` flag (skips starting an in-process `NativeBroker`,
dials an already-running MQTT 3.1.1 broker on `127.0.0.1:N` instead) so the
*exact same* client-side protocol code and measurement methodology could be
pointed at Mosquitto 2.0.15 (installed via `conda-forge`, `allow_anonymous
true`, no persistence, default listener config) on this same Windows machine,
back-to-back with AeroEdge, in the same session.

**Caveats up front, so the numbers aren't over-read:**
- AeroEdge's broker runs *in-process* with the benchmark tool in this harness
  (still communicating over a real loopback TCP socket, same as every other
  number in this doc); Mosquitto runs as a genuinely separate OS process. This
  is how AeroEdge has been measured all session, so it stays internally
  consistent with §2.4/§3.1, but it is a structural difference from Mosquitto's
  setup, not a fully neutral harness.
- This is one broker's Windows build (`conda-forge`'s win-64 package) on one
  machine — not a claim about Mosquitto's Linux/epoll performance in general,
  which is a different, well-optimized code path this test never touched.
- No tuning was attempted on either side beyond each project's stated defaults.

**Single connection pair, QoS 0, no fan-out** (3 runs each, back to back):

| Broker | Throughput (msg/s) | p50 latency (ms) |
|---|---|---|
| AeroEdge (buffered reads) | 57,270 / 83,634 / 90,764 | 48.8 / 32.6 / 30.8 |
| Mosquitto 2.0.15 | 40,721 / 41,829 / 25,896 | 69.8 / 62.5 / 110.9 |

AeroEdge was faster and lower-latency than this Mosquitto build in every run,
for this traffic shape, on this machine.

**2,000 idle (non-matching) subscriber connections present, same 1:1 QoS 0
traffic on top:**

| Broker | Connect setup rate | Real-traffic throughput | Real-traffic p50 latency |
|---|---|---|---|
| AeroEdge | ~24/s | 93,418 msg/s | 29.9ms |
| Mosquitto 2.0.15 | ~24/s | 448 msg/s | 6,347.6ms |

Two findings here:

1. **The connect-setup rate (~24-26/s) was identical for both brokers.** This
   corrects an assumption from earlier in this session — the ~25-33/s connect
   rate seen driving 5,000 idle sessions against AeroEdge was suspected to
   reflect broker-side (thread-per-connection) setup cost. Since Mosquitto's
   connection handling is event-loop-based, not thread-per-connection, and it
   shows the *same* rate, the ceiling is actually in `broker_bench`'s own
   `SubClient` (one reader thread spawned per idle session, client-side) — not
   a broker property at all. Worth fixing in the harness if idle-session
   connect-rate is measured again, but doesn't affect any throughput/latency
   number in this doc (which are all measured only after setup completes).
2. **Once traffic starts, AeroEdge holds its 3.1-throughput steady (93K msg/s,
   ~30ms p50 — matching the 0-idle-session numbers) while this Mosquitto build
   collapsed to 448 msg/s and 6.3-second p50 latency** with 2,000 idle,
   non-matching subscriptions present. AeroEdge's result is expected — it's
   exactly what the topic-index fix (this session's first change) was built
   to guarantee: `route_publish()` no longer scans idle sessions at all.
   Mosquitto's collapse was *not* investigated further (out of scope for this
   round) — a likely cause is this Windows build's network-loop I/O
   multiplexing degrading under thousands of concurrent sockets (e.g. a
   `select()`-based loop rather than an IOCP/epoll-equivalent), but that's a
   hypothesis, not confirmed. Flagged honestly rather than asserted as a
   general Mosquitto weakness.

**Bottom line (2,000-session round):** for the traffic shapes actually
measured here, AeroEdge is competitive with — and in the idle-connection-
scaling case, currently ahead of — this particular Mosquitto build on this
machine. This is not a claim about Mosquitto's real-world (Linux) ceiling, nor
about AeroEdge's own ceiling against reactor-based brokers at very high
aggregate connection counts (tens of thousands+), which remains the open
question the deferred `IoContext` migration (§3.0) would address.

### 3.3 Full-scale re-run: 5,000 idle sessions, plus a CPU-usage check

Re-ran the same comparison at the full 5,000-idle-session scale already used
for AeroEdge earlier in this session (matching that original benchmark's
scope), and added real CPU-usage sampling (`Get-Process` polled every 3s
throughout) to check an observation made mid-run: neither side looked
CPU-bound on Task Manager.

**AeroEdge, 5,000 idle sessions:** connect setup held steady at ~27-33/s
(same range as the 2,000-session run and the original 5,000-session run
earlier this session — no decay as count grows), then real 1:1 QoS 0 traffic
on top: **86,997 msg/s, 32.4ms p50** — matching the 0- and 2,000-idle-session
numbers almost exactly. The topic index holds at this scale too.

**Mosquitto 2.0.15, 5,000 idle sessions: broke at session 2,047.** Setup
proceeded at the same ~26-30/s rate as AeroEdge up to session 2,000, then
**session 2,047 failed to connect/subscribe after an 80.7s timeout** — the
Mosquitto process itself stayed alive throughout (confirmed via `Get-Process`
immediately after), it simply stopped completing new connection handshakes
past that point. The number is suspicious in a specific way: 2,047 is exactly
`2,048 - 1`, consistent with a compile-time `FD_SETSIZE`-style cap on this
Windows build's socket multiplexing (one slot reserved for the listening
socket) rather than a random hang. **Not confirmed** — didn't have this
build's source/build config on hand to verify — but it lines up with §3.2's
"select()-based loop" hypothesis for the 2,000-session collapse: the same
mechanism could plausibly explain both the *slowdown* at 2,000 and the *hard
wall* at 2,047. Flagged as a strong lead, not a proven root cause.

**CPU usage during the run (answers "our test didn't use much CPU" — it's
real, not just visual):**

| Process | Avg CPU over the ~80s window |
|---|---|
| `mosquitto.exe` | ~1.6% |
| `broker_bench.exe` (client, 2,000+ live threads) | ~12-13%, trending down |

Neither process was anywhere near CPU-saturated when Mosquitto stopped
accepting new connections. This matters for the diagnosis: it rules out "the
CPU is the bottleneck" for both the connect-rate ceiling (§3.2's finding —
already known to be client-thread-spawn-bound, not compute-bound) and
Mosquitto's connection-count wall — reinforcing that these are I/O-multiplexing
/ socket-accounting limits, not raw processing-power limits. This is
consistent with the whole session's throughline: AeroEdge's own bottlenecks
(pre-topic-index linear scan, pre-buffered-reads per-syscall reads) were also
never about CPU cycles — they were about the *shape* of I/O (syscall count,
scan breadth), which is exactly why a topic index and buffered reads produced
real wins without needing more compute.

**Updated bottom line:** at full 5,000-connection scale, AeroEdge not only
kept its throughput/latency steady (as it did at 2,000) but was the only one
of the two that could actually reach 5,000 live connections on this machine —
this specific Mosquitto build hit a hard connection-count wall around 2,047.
Same caveats as §3.2 apply: one Windows build, one machine, not a general
claim about Mosquitto (particularly not about its Linux/epoll deployments,
where this specific limit likely wouldn't exist).

### 3.4 Does AeroEdge actually use more than one core? (multi-pair scaling test)

Raised by a follow-up question: §3.2/§3.3's single-connection-pair numbers and
low CPU% (~1-13%) could look like "the whole broker only ever uses one core" —
worth checking directly rather than assuming, since that's a materially
different claim from "this workload happens to be I/O-bound, not CPU-bound."

`broker_bench` gained `--independent-pairs 1`: instead of every publisher/
subscriber sharing one broadcast topic, publisher *i* and subscriber *i* get a
unique topic (`bench/topic/i`), so N pairs behave as N genuinely independent,
non-overlapping publish→deliver chains — the traffic shape aggregate
multi-core throughput claims (MQTTnet's 700K, EMQX's 2M) actually measure,
unlike the single-pair ceiling in §3.1-§3.3.

**Aggregate throughput scaling** (this machine: AMD Ryzen 5 4600H, 6 physical
/ 12 logical cores; 50,000 msg/publisher, QoS 0):

| Independent pairs | Aggregate throughput | p50 latency |
|---|---|---|
| 1 | 52,756 msg/s | 54.4ms |
| 4 | 96,608 msg/s | 88.8ms |
| 8 | 131,107 msg/s | 3.8ms (p95 176ms — see note) |
| 12 | 126,862 msg/s | 106.5ms |

Real scaling with concurrency, not flat — confirms the broker is not confined
to one core. Peaks around 8 pairs (~6 physical cores' worth of genuinely
concurrent session threads) and roughly plateaus/dips slightly by 12, which is
consistent with this machine having 6 physical cores under 12 logical ones.

**Confirmed directly via CPU sampling**, not just inferred from throughput: a
12-pair, 300,000-msg/publisher run (3.6M total messages) was sampled every
300ms for its whole duration. CPU usage climbed steadily to **~315% (over 3
full cores' worth of simultaneous work)** as the run progressed — a world away
from §3.3's ~12-13% (single-core-equivalent) when the workload was mostly
idle/blocked threads. **This settles the question: AeroEdge is not
single-core-bound. §3.1-§3.3's low CPU% reflects those specific workloads
being I/O-bound (blocked on syscalls, not spinning), not a ceiling on how many
cores the broker can use** when given genuinely concurrent, independent work.

**A throughput drop seen on the first 300K-msg/publisher run (56,716 msg/s,
p95 1.76s) did NOT reproduce** — flagged initially as a possible new finding,
then checked, per this session's own "repeat before concluding" discipline
(the same reasoning applied to `concurrency_stress` and the §3.1/§3.2 repeat
runs). Two immediate repeats of the identical 12-pair/300K-msg/publisher run
came back at **133,153 msg/s and 131,442 msg/s** — consistent with the
smaller-volume 8/12-pair numbers above, not the one-off drop. The likely cause
of that single bad sample: it was launched from inside a PowerShell CPU-
sampling loop polling `Get-Process` every 300ms for the run's entire duration
— that sampler itself was competing for the same cores it was trying to
measure. Recorded here as a methodology note, not a broker finding: don't
trust a single sample, especially one taken by an instrument that shares
resources with the thing being measured.

One real (reproducible) oddity worth noting: **p50 latency swung between 1.4ms
and 155.7ms across the two clean repeat runs, while p95/p99/max stayed tight**
(286-300ms / 359-389ms / 558-582ms). With 12 independent, unsynchronized
publisher threads racing at QoS 0, the aggregated latency distribution is
bimodal-ish (some pairs finish their burst quickly, others queue behind
scheduler contention) — small run-to-run scheduling differences can flip
which cluster the 50th-percentile index lands in. p95/p99/max are the
trustworthy numbers for this traffic shape; p50 is not.

**Bottom line:** AeroEdge does scale aggregate throughput with genuine
concurrency and does use multiple cores when the workload calls for it — the
single-connection numbers in §3.1-§3.3 measure a different thing (one
connection pair's ceiling) on purpose, not a hidden single-core limitation.
Peak observed aggregate throughput this round (~131K msg/s at 8 pairs, small
volume) is still far below marketing figures like 700K/2M — expected, since
those are typically measured on server-grade multi-core/multi-node hardware,
not a 6-core laptop CPU, and (per §3.2) likely represent a different
connection-count/volume regime than tested here. The volume-dependent
degradation above is a more interesting lead for closing that gap than raw
core count is.

## Phase 4 — Next-round candidates: investigated, red-team verified, none shipped

Three candidates raised in review (pooling, cache-line padding, QoS-0-specific
outbound write batching) were investigated via a workflow: one agent per
candidate builds a small, isolated, real prototype and measures it (repeated,
never a single sample), then **two independent adversarial agents per
candidate** try to refute the finding — the same "prove it, then try to break
it" discipline used for Phase 3's implementation plan. No production code
changed as a result of this section; it's a validated punch list for a future
round. One real bug in `broker_bench` itself *was* found and fixed as a direct
result (§4.3).

### 4.1 Object/buffer pooling for the PUBLISH hot path — real, worth pursuing

**Investigation:** every PUBLISH today allocates ~6 heap objects on the common
path (`handle_publish`'s topic/resolved_topic/payload copies, `route_publish`'s
candidate-session and per-session-matches vectors, `publish_to`'s outbound
buffer — exact line numbers in the investigation transcript). A standalone
microbenchmark (no AeroEdge headers, pure `clang++ -O2`) replicated the same
allocation shapes/sizes: **fresh allocation every iteration averaged
~890ns/op; a pooled/reused-buffer variant averaged ~38ns/op — a 15-23x
speedup on the allocation-only slice**, measured 3 independent process runs ×
3 repeats each (9 samples/variant), with real run-to-run variance
(2.3-2.7x) openly disclosed rather than cherry-picked.

**Independently re-verified twice more:** once by re-running the agent's exact
compiled binary myself in a separate process (~882-895ns fresh / ~37-40ns
pooled, 23.7x — matching the original), and once by two independent red-team
agents (both marked the finding **not refuted**) who additionally cross-checked
the allocation-only cost against this doc's own §3.1 broker_bench numbers:
~0.9-2.0us of allocation cost is ~9-19% of QoS 0's real per-message budget
(~10.1-10.7us at current throughput) and only ~2-4.5% of QoS 1's (PUBACK-round-
trip-bound, ~44.5us/message) — independently corroborating the investigator's
own hedged estimate almost exactly.

**Honest scope:** the 15-23x ratio describes only the allocation slice, not
the whole hot path (locks, topic matching, and the actual socket write/PUBACK
round trip aren't modeled). Estimated real-world impact if implemented:
**roughly 10-20% additional QoS 0 whole-broker throughput, likely negligible
for QoS 1** — smaller than Phase 3's shipped buffered-reads win, not a
replacement for it. Not yet attempted end-to-end. **Recommended as a real
Phase 5 candidate** — next step would be an actual pooled implementation in
`native_broker.hpp` (thread_local reusable buffers, with real lifetime
verification against the thread-per-connection model) measured with
`broker_bench`, not another isolated prototype.

### 4.2 Cache-line padding for `Session::kicked` — investigated, not worth pursuing

**Investigation:** hypothesized that `Session::kicked` (an
`std::atomic<bool>` written once by a takeover thread, read every drain-loop
iteration by the owning thread) might suffer false sharing with adjacent
fields the owning thread mutates constantly (`read_buf`/`subs`/`client_id`).
A standalone microbenchmark confirmed false sharing is a *real, measurable*
effect on this hardware **under a pathological, continuously-hammering writer**
(non-overlapping ~20-40% throughput hit) — but the effect vanished into noise
at more realistic write rates, and production's actual write rate (`kicked`
is set **at most once, ever, per superseded session** — not periodic, not
per-message) sits many orders of magnitude below where the benchmark could
even detect anything.

**Red team correction (both agents independently found this via
`offsetof` on the real struct):** the investigation's own claim about *which*
fields are adjacent to `kicked` was factually wrong. On this ABI, `kicked`
(offset 312) is a full cache line away from `read_buf`/`subs`/`client_id`
(offsets 192-279) — its actual neighbors are
`keep_alive_s`/`clean_session`/`protocol_version`/`session_expiry_interval`/
`max_packet_size`/`last_activity`/`has_will`, and even the closest "hot"
candidate (`last_activity`) is written once per *packet*, not once per
drain-loop *iteration* as the benchmark modeled. This doesn't flip the
conclusion — if anything it reinforces it — but the original mechanism
description should not be trusted as-is.

**Verdict: not worth pursuing.** Both red-team agents concurred
(`refuted: false` on the "don't pursue" recommendation) — the write frequency
argument alone is sufficient and doesn't depend on the (partly incorrect)
layout claim.

### 4.3 QoS-0-specific outbound write batching — correctness design is sound, performance claim did NOT survive red-team review

This is the corrected version of the fan-out/outbound-queue idea deferred
back in §3.0. The investigating agent built a real prototype in an isolated
worktree (not a microbenchmark — an actual scoped edit to `native_broker.hpp`,
QoS 0 only): a dedicated `outbox_mu` separate from `io_mu`/`subs_mu`, a
strict single-flusher FIFO (no leapfrogging), and — critically — it queues
`{topic, payload, extras}` (the logical message) and **re-runs the real
Message-Expiry/Maximum-Packet-Size choke points at actual flush time**, not
at enqueue time. **Code-level correctness holds**: both red-team agents
independently read the real diff and confirmed all three of §3.0's original
critique bugs are genuinely avoided by construction.

**But the performance case does not survive review — both red-team agents
marked it `refuted: true`.** Two independent, serious problems surfaced:

1. **Unconfirmed whether the new code path was even exercised.**
   `broker_bench` defaults to `--qos 1`, and one `--qos` flag governs both
   publish and subscribe QoS (the new batching path only activates when
   *delivered* QoS is 0). None of the investigation's reported run labels
   record `--qos 0` being passed, and no startup log confirming it was found.
   If it wasn't, both "baseline" and "prototype" binaries ran the identical,
   untouched code for every message, and the entire measured differential —
   including a reported ~140x heavy-load median-latency improvement — would
   have nothing to do with the change under test.
2. **A real, confirmed bug in `broker_bench.cpp` itself**: percentiles were
   computed only over messages that arrived before `--timeout-s` expired, so
   an incomplete run silently excludes its own slowest deliveries — making
   *worse-performing, incomplete* runs look *better* on p95/p99 than complete
   ones. The red team found the reported data matching this pattern exactly
   (each variant's one incomplete run was its own best-looking p95). **This
   bug is real and not specific to this one investigation** — it could have
   silently biased any past or future `broker_bench` run that didn't fully
   drain within its timeout window. **Fixed directly in `bench/broker/broker_bench.cpp`**:
   the tool now prints an explicit stderr warning whenever delivery is
   incomplete, naming the percentiles as survivorship-biased and
   untrustworthy, instead of silently reporting them. Verified with a real
   run forced into a 99.8%-complete window — the warning fires correctly.
3. Beyond those two, the investigation's own heavy-load numbers were n=1
   (not repeated, "each run costs 60-150s") despite the same investigation's
   *light*-load data already showing 11-25x run-to-run swings at n=3 — nowhere
   near enough evidence for the specific multipliers reported.

**Verdict: not ready to promote.** The design itself is a legitimate,
correctness-sound answer to §3.0's original critique and is worth keeping as
a starting point — but the measured performance claim needs a clean re-test
(confirmed `--qos 0`, the now-fixed `broker_bench`, and enough repeats at
every load level) before it can be trusted either way. The investigation also
surfaced one real, not-yet-fixed design gap worth carrying forward regardless
of the numbers: under sustained backlog, a single flusher thread can be
pinned draining one recipient for an unbounded duration — a different
monopolization problem than the one §3.0 originally named, and something a
real implementation would need a bounded-batch-size cap to avoid.

### 4.4 Summary

| Candidate | Design/correctness | Performance claim | Verdict |
|---|---|---|---|
| Pooling | N/A (isolated microbench, not integrated) | Real, reproduced independently, cross-checked against real broker numbers | **Worth pursuing** as a Phase 5 candidate |
| Cache-line padding (`kicked`) | N/A | Real effect exists but at an unrealistic write rate; production write rate is negligible | **Not worth pursuing** |
| QoS 0 write batching | Sound — avoids all 3 originally-critiqued bugs | Not validated — unconfirmed test config + a real `broker_bench` percentile bug + insufficient repeats | **Design keeper, re-test before trusting any number** |

## Phase 5 — Buffer pooling: shipped, real but smaller than estimated

Turned §4.1's "worth pursuing" pooling finding into a real, scoped
implementation, following the same critique-before-code discipline used for
Phase 3. A Plan-agent critique of the draft (full write-up in the approved
plan) found and the shipped version incorporates three fixes before any code
was written:

1. **A real bug in the naive description**: `topic_index_candidates()`'s
   "not found" branch never reset its output vector — harmless when it was a
   fresh local every call, but would silently leak a previous call's stale
   sessions into the current one once it became a persistent `thread_local`.
   Fixed: `out.clear()` now runs unconditionally, before the lookup.
2. **A reentrancy gap the original draft's analysis missed**:
   `route_publish()` has a second caller, `deliver_remote_publish()` (the
   cluster relay path, dispatched from `BrokerRelayActor` on a Quark actor-
   engine worker thread, not a session's reader thread). Verified safe by
   reading QuarkCpp's `activation.hpp`: `BrokerRelayActor` is a
   `quark::Sequential` actor, confirmed single-in-flight (never processes a
   second message before the first fully completes) — combined with
   `thread_local` being inherently per-OS-thread, there's no cross-call
   interference regardless of which thread dispatches into this code.
3. **A scope cut**: pooling `route_publish()`'s per-candidate `matches`
   vector (`vector<Subscription>`) was dropped — it only saves the outer
   vector's own allocation, since each `Subscription` still heap-allocates
   its own `filter` string on every refill (SSO doesn't cover realistic MQTT
   topic lengths), a much smaller win than the other two candidates for the
   same added auditing burden.

**Shipped**: `topic_index_candidates()`'s output vector and `publish_to()`'s
outbound `vh` buffer are now `static thread_local`, reused across calls
instead of freshly heap-allocated every time (`publish_to()` covers all 3 of
its call sites — live fan-out, offline-queue flush, retained replay — not
just the one originally scoped). `handle_publish`'s `topic`/`payload` and the
QoS 2 / offline-queue persisted copies remain unpooled, per §4.1's original
scoping (public extension-hook exposure and genuine cross-call persistence,
respectively).

**Verification**: full broker test suite green (`native_broker`, `mqtt5`,
`broker_cluster`, `acl`, `bridge`, `buffered_read_framing`, `mqtt_codec`,
`native_broker_security`); `concurrency_stress` run 18x independently, 18/18
clean.

**Real measured result — smaller than the §4.1 estimate, and a genuine
lesson in machine-noise discipline.** The first measurement pass (interleaved
baseline-vs-pooled `broker_bench` runs) was unusable: the dev machine had
heavy concurrent background load (a JetBrains Rider indexing pass, Docker
Desktop) at the time, and pooled vs. baseline throughput swung both
directions between rounds with no consistent signal. Per this project's own
established discipline (§3.4, §4.3), a noisy, direction-flipping comparison
is not evidence either way — so rather than report it, the background load
was quiesced and the comparison re-run clean:

| Scenario | Baseline (10 interleaved runs) | Pooled (10 interleaved runs) | Delta |
|---|---|---|---|
| QoS 0, 1:1, 300K msgs | mean ≈63,046 msg/s | mean ≈64,957 msg/s (1 early outlier run excluded from the tighter read: ≈66,478 msg/s) | **+3% to +5%** |
| QoS 1, 1:1, 50K msgs (3 rounds) | mean ≈8,922 msg/s | mean ≈8,727 msg/s | ~flat (within noise, as expected — round-trip-bound) |
| Shared-topic fan-out, 8 subs × 2 pubs, 16 rounds (12 after excluding noise-spike rounds) | mean ≈11,339 msg/s (clean subset) | mean ≈11,609 msg/s (clean subset) | **~+2.4%, directionally positive but not statistically confident** |

QoS 0's single-connection win is real and directionally consistent (pooled
ahead in 9 of 10 clean rounds) but **landed at the low end of §4.1's own
hedged estimate (~10-20%), not the middle of it** — actual measured gain is
roughly 3-5%. This is not a failure of the investigation: §4.1 explicitly
flagged this as an *estimate*, not a measurement, and correctly anticipated
"the real hot path is dominated by mutex locks, topic matching, and the
actual socket write" diluting the allocation-only ratio — the real number
just landed lower within that already-acknowledged range than the midpoint
suggested.

**Fan-out follow-up (16 total interleaved rounds, run after the QoS 0 result
above):** the hypothesis going in was that `publish_to()`'s `vh` pooling
should show a *bigger* win here than the 1:1 case, since `vh` is rebuilt once
per recipient — 8 subscribers should amplify the effect roughly 8x versus
1:1. That hypothesis did not hold up. 4 of the 16 rounds were clearly hit by
an external system-wide slowdown (both baseline and pooled — or, in two
rounds, only one side — dropped to roughly half their surrounding
throughput; not attributable to either binary, most likely a background OS/
disk event, since the *which* side got hit alternated between baseline and
pooled across the affected rounds rather than consistently favoring one).
Excluding those 4, the remaining 12 clean rounds show pooled ahead in 9 of
12, mean delta ≈+270 msg/s (+2.4%) — same direction as the 1:1 result, but
**roughly the same magnitude, not amplified**, and a rough paired-difference
check (mean delta ≈270, standard deviation of the per-round deltas ≈794,
giving a t-statistic ≈1.18 against 11 degrees of freedom) does **not** clear
a conventional significance threshold — unlike the 1:1 QoS 0 result, this one
should be read as "consistent with a small real effect, not proof of one."
Best-guess explanation: at only 8 subscribers, the per-recipient cost `vh`
pooling removes is still a small fraction of that recipient's total handling
cost (socket write, `wait_writable` retry loop, subs_mu lock, topic-match
check) — the allocation isn't the dominant cost even when repeated 8x, so it
doesn't compound into a proportionally bigger win. A wider fan-out (dozens to
hundreds of matching subscribers) might show more separation, but wasn't
tested this round.

**Bottom line**: real, positive, shipped — but a good reminder that an
isolated microbenchmark's ratio is a ceiling, not a prediction, for the
integrated system, and that machine noise must be actively controlled for
(not just disclosed) before a small effect can be trusted at all.
