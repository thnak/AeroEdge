# Native MQTT Broker — Where It Runs and What Happens to a Message

> Explainer for the embedded broker of spec [017](specs/017-Native-Broker-and-Southbound-Termination.md)
> (with the I/O redesign of [021](specs/021-Native-Broker-Performance-Redesign.md)). Describes the code
> **as it is today** — gaps are called out explicitly. Source of truth:
> `include/aero/broker/native_broker.hpp`, `include/aero/broker/broker_cluster.hpp`,
> `include/aero/runtime/runtime.hpp` (`configure_broker()`), `src/runtime/aero_runtime_main.cpp`.

## 1. Where the broker lives

The broker is **not** a separate server — it is `NativeBroker`, a subsystem inside the `aero-runtime`
daemon. It is off by default and started only when a `--broker-*` flag is passed:

```text
aero-runtime --broker-port 1883 [--broker-bind 0.0.0.0]
             [--broker-tls-cert X --broker-tls-key Y [--broker-tls-ca Z]] [--broker-tls-port 8883]
```

It is **daemon-scoped**: it lives for the daemon's lifetime, independent of which `Application`/flows are
deployed or hot-reloaded. `GET /broker/status` reports whether it is configured.

```mermaid
flowchart LR
    subgraph Devices["Devices / MQTT clients"]
        D1["Device (plaintext)"]
        D2["Device (TLS / mTLS)"]
    end

    subgraph Daemon["aero-runtime process"]
        CLI["main(): --broker-* flags"] --> CFG["Runtime::configure_broker(cfg)"]
        CFG --> NB

        subgraph NB["NativeBroker"]
            ACC["accept thread"]
            REA["IoContext reactor<br/>(plaintext sessions, 021 Phase 7)"]
            LEG["thread per session<br/>(TLS sessions)"]
            HO["bounded hand-off worker pool"]
            CORE["handle_publish → deliver_publish → route_publish"]
            RET[("retained_ store")]
            OFF[("offline persistent-session queues")]
            HOOK{{"on_publish_ hook"}}
            FWD{{"peer_forwarder_ hook"}}
        end

        CLUSTER["BrokerCluster (optional, 017 M6)"]
        API["REST API: GET /broker/status"]
    end

    D1 -- "TCP :1883" --> ACC
    D2 -- "TLS :8883" --> ACC
    ACC --> REA
    ACC --> LEG
    REA --> CORE
    LEG --> CORE
    CORE --> RET
    CORE --> OFF
    CORE --> HOOK
    CORE --> FWD
    REA -. "legacy recipient" .-> HO
    FWD -.-> CLUSTER
    API --- NB
```

## 2. What happens when a PUBLISH arrives

### 2.1 Wire-level handshake by QoS

`handle_publish()` parses the packet, resolves MQTT 5 properties / Topic Alias, runs the ACL gate, then
acts according to QoS. Key rule: for QoS 0/1 the message is **delivered before it is acknowledged**, so a
publisher holding a PUBACK can assume subscribers will see it. A denied PUBLISH is **silently dropped** —
no PUBACK/PUBREC is sent; that withheld ack *is* the signal.

```mermaid
sequenceDiagram
    autonumber
    participant P as Publisher
    participant B as NativeBroker
    participant S as Subscribers

    P->>B: PUBLISH(topic, payload, qos)
    B->>B: parse + v5 properties (expiry, response topic, user props)
    B->>B: resolve Topic Alias (before ACL, so an alias can't bypass it)
    B->>B: ACL authorizer.allow(principal, topic, Publish)

    alt denied
        Note over B: silent drop — no ack, deliver_publish() never runs
    else QoS 0
        B->>S: deliver_publish()
    else QoS 1
        B->>S: deliver_publish()
        B-->>P: PUBACK
    else QoS 2
        B->>B: stash in qos2_inflight[packet_id]
        B-->>P: PUBREC
        P->>B: PUBREL
        B->>S: deliver_publish() (exactly once)
        B-->>P: PUBCOMP
    end
```

A client's **Last Will** takes the same `deliver_publish()` path (with no wire packet or ack) when its
session is torn down abnormally.

### 2.2 `deliver_publish()` — the single shared action

Every locally-originated PUBLISH (QoS 0/1 immediately, QoS 2 at PUBREL, Will at teardown) goes through
one function, so retention, ingestion and routing can never drift apart:

```mermaid
flowchart TD
    A["deliver_publish(topic, payload, qos, retain, extras)"] --> R{"retain flag?"}
    R -- "yes, empty payload" --> R1["erase retained_[topic]"]
    R -- "yes, payload" --> R2["retained_[topic] = message"]
    R -- no --> H
    R1 --> H
    R2 --> H
    H{"on_publish_ set?"} -- yes --> H1["on_publish_(topic, payload, qos, props)<br/>ingestion seam for bridges / flows"]
    H -- "no (daemon today)" --> RP
    H1 --> RP["route_publish() — local fan-out (§2.3)"]
    RP --> F{"peer_forwarder_ set?<br/>(cluster enabled)"}
    F -- yes --> F1["forward to every peer (§3)<br/>AFTER local delivery"]
    F -- no --> E(["done"])
    F1 --> E
```

### 2.3 `route_publish()` — local fan-out

1. **Candidate narrowing**: `topic_index_candidates(topic)` returns only sessions that *could* match,
   instead of scanning every session.
2. **Exact match**: each candidate's subscriptions are checked with `topic_matches(filter, topic)`
   (`+` / `#` wildcards). Delivered QoS = `min(publish qos, granted qos)`.
3. **Shared subscriptions** (`$share/group/filter`, MQTT 5): matches are bucketed per (group, filter) and
   exactly **one** member per bucket is chosen, round-robin.
4. **Offline persistent sessions** (`clean_session=0`) with a matching subscription get the message
   queued — **QoS ≥ 1 only** — and flushed on reconnect. Message Expiry is re-checked at send time.
5. **Dispatch** depends on who is calling and who is receiving — the reactor thread must never block:

```mermaid
flowchart TD
    Q["route_publish()"] --> T{"caller is the reactor thread?"}
    T -- yes --> B["build delivery list, then process in batches of<br/>kMaxFanoutInlinePerCall = 256<br/>(rest chained via reactor post(), 021 Phase 7b)"]
    B --> K1{"recipient kind"}
    K1 -- "reactor session (plaintext)" --> E1["enqueue_reactor_publish()<br/>non-blocking outbound queue"]
    K1 -- "legacy session (TLS)" --> E2["enqueue_legacy_handoff()<br/>bounded worker pool + send deadline"]
    T -- "no (TLS session thread / cluster relay thread)" --> K2{"recipient kind"}
    K2 -- "reactor session" --> E1
    K2 -- "legacy session" --> E3["publish_to() inline<br/>(blocks only the caller's own thread)"]
```

## 3. Distribution (multi-node) — `BrokerCluster`

Opt-in via `configure_broker(cfg, BrokerClusterConfig{self, cluster_port, peers})`. With an empty peer
list nothing is constructed and the broker is purely single-node.

| Aspect | Behavior today |
|---|---|
| Membership | **Static** peer roster from config — no gossip, failure detection, or dynamic join/leave |
| Link | One multiplexed `TcpTransport` connection per peer on a separate `cluster_port`; optional mTLS (017 M5.1) |
| Fan-out model | **Broadcast**: every locally-received PUBLISH goes to every peer; each peer does its own local wildcard match |
| Loop prevention | Relayed messages enter via `deliver_remote_publish()` — local delivery only, never re-forwarded |
| Retained messages | **Not** synchronized across nodes |
| Session continuity | **Not** carried across nodes |
| Message Expiry | Forwarded as a *relative* remaining duration, re-anchored on the receiving node's clock |
| Daemon wiring | **Gap**: `aero-runtime` has no cluster CLI flags — only reachable via the C++ API / tests |

```mermaid
sequenceDiagram
    autonumber
    participant DA as Device on node A
    participant A as NativeBroker A
    participant CA as BrokerCluster A
    participant CB as BrokerCluster B (BrokerRelayActor)
    participant B as NativeBroker B
    participant DB as Subscriber on node B

    DA->>A: PUBLISH sensors/line1/temp
    A->>A: deliver_publish(): retain, on_publish_, route_publish (A's subscribers)
    A->>CA: peer_forwarder_(topic, payload, qos, expiry remaining, props)
    loop every peer
        CA->>CB: MessageFrame over TCP (cluster_port)
    end
    CB->>B: deliver_remote_publish()
    B->>B: on_publish_ + route_publish only<br/>(no retain write, no re-forward)
    B->>DB: PUBLISH sensors/line1/temp
```

## 4. Where the message goes *after* the broker — current gap

Spec 017 (N4) intends broker traffic to reach flows through the same Driver/Source contract as every
other ingestion path (006). **That link is not wired in the daemon today**: `Runtime::configure_broker()`
never calls `broker.on_publish(...)` — only tests do. So in a running `aero-runtime`, the broker relays
between MQTT clients and nothing else. The bridge / rule engine (`bridge.hpp`, incl. the RabbitMQ sink)
plugs into the same hook but is also C++-API-only.

```mermaid
flowchart LR
    NB["NativeBroker<br/>deliver_publish()"] --> H{{"on_publish_ hook"}}
    NB --> SUBS["MQTT subscribers<br/>(works today)"]

    H -. "NOT WIRED in daemon" .-> SRC["Source node<br/>(Frame per message, 006)"]
    SRC -.-> FLOW["Flow DAG (004)<br/>transform / rule nodes"]
    FLOW -.-> OUT["Output nodes<br/>e.g. MesReportNode (012)"]

    H -. "C++ API only" .-> BR["BrokerRuleEngine (bridge.hpp)<br/>rules → IBridgeSink"]
    BR -.-> EXT["MQTT-to-MQTT / HTTP webhook / RabbitMQ"]

    classDef gap stroke-dasharray: 5 5;
    class SRC,FLOW,OUT,BR,EXT gap;
```

To close it: register `on_publish` in `configure_broker()` and add a broker-backed Source node that turns
each `(topic, payload, qos, props)` into a `Frame` for a deployed flow — keeping the callback
non-blocking, since it runs on the reactor / session thread (I1, 004 §4).
