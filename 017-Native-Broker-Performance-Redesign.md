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
