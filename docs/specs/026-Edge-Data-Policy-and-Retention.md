# 026 — Edge Data Policy and Retention

> Draft v0.2. What data AeroEdge keeps, where, for how long, and which of it is disposable.
> Resolves 007 §10 "Retention & compaction policy" and 012 §7 "Backfill/replay". Suite-wide
> policy (what AeroOEE/AeroMes keep) is in
> [AeroSuite 05](https://github.com/Iot-Viet-Solution/AeroSuite/blob/main/docs/05-data-storage-policy.md).
>
> v0.2 incorporates the red-team and use-case review:
> - raw capture is change-only and off by default;
> - new health-history class;
> - a real ingest-throttle path;
> - complete outbox sizing;
> - control-plane epoch on restore.

## 1. Principle

AeroEdge is a **pipe with a buffer, plus a control plane**. It isn't a historian.

- **Runtime data** (raw captures, delivered events) is short-lived and disposable.
- **Undelivered data** (producer outboxes, 025 §2) must not be lost, because it's production
  counts that haven't reached a system of record yet.
- **Control-plane data** (registry, credentials, bindings, flows) is configuration the plant runs
  on, and it must be backed up.
- Once an event is delivered to every critical subscription, **the upstream product owns its
  history**.
- **AeroEdge produces no rollups of raw data.** The only edge-side aggregation is what the
  canonical events carry: count windows, cycle stats, opt-in downsampled `ProcessSample`.
  Rollups and trends belong to consumers (AeroSuite 05).

## 2. Data classes

| Class | Where | Default retention | Bound | Disposable? |
|---|---|---|---|---|
| **Raw capture** (point changes) | node, Quark 012 store | **off by default**; when enabled per point: 3 days | **change-only** (deadbanded), per-point and per-node size caps | yes |
| **Producer outboxes** (undelivered events) | node, Quark 012 store (025 §2) | until every `critical` subscription has acked | sized for **N days** of outage (§4) | **no** |
| **Delivered event log** | node | 3 days | size cap | yes (replay/debug, §3 rule 4) |
| **Health history**: sessions, connectivity changes, heartbeats (on change + 5-min summaries), reboots, device logs | node, forwarded to control plane | 90 days (device logs: 30 days) | size cap | yes, but useful. Back it up if cheap |
| `MachineActor` / `EdgeActor` state | node, Quark 012 (S1) | current | small | no (024 §5.2) |
| Registry, credentials, bindings, groups | control plane, EventSourced (022 DR5) | life of deployment. Log compacted to snapshots after 90 days | small | **no** |
| Applications / flow versions | control plane (009) | current + last 10 versions | small | no |
| Firmware artifacts | control plane | current + 3 previous per model | per model cap | rebuildable from source |
| Audit (registry, credential, binding, output-command, revoke changes) | control plane | 1 year, customer-configurable | | no |

All defaults are **per-deployment configurable**.

## 3. Rules

1. **Raw is never shipped in bulk.** Upstream gets canonical events and opt-in downsampled
   `ProcessSample` (025 UH4). Raw capture is for wiring and diagnostics. It's exported on demand
   (bounded time range, CSV/Parquet) through Studio/API, never streamed.
2. **Raw capture is change-only and opt-in per point** (Studio "capture" toggle, auto-expiring
   after the retention period). It's built on the Quark 012 store. AeroEdge doesn't write a new
   storage engine (thin-over-Quark).
3. **Producer outboxes never evict silently.** This is the backpressure chain:

   | Outbox fill (node total) | Action |
   |---|---|
   | ≥ 80% | `OutboxPressure` alarm + metric. Best-effort subscriptions are already quota-bound (025 §2.1) |
   | ≥ 95% | **Ingest throttle**: the gateway sends `IngestThrottle(on)` to the node's `EdgeActor`s. They stop retiring stream batches (006 §3), so credit stops and drivers stop reading |
   | < 85% | `IngestThrottle(off)` |

   - Exempt from throttling: broker keep-alive (PINGRESP), session management and heartbeats.
     Otherwise every MQTT device would time out and reconnect in a storm.
   - While throttled, offline detection is suspended for devices silenced by the throttle
     (022 §5).
   - Counter sources (protocol v4, PLC) **lose nothing**: their counters keep counting, and the
     delta is recovered on the next read (024 §4).
   - Driver-host interrupt counts may be lost at this point. That loss is reported as
     `CountGap{reason: throttle}`. It's never hidden.
4. **Replay:** a subscription may be rewound within the delivered-event-log window (e.g. after a
   consumer restores from backup). Beyond that, the system of record is upstream.
5. **Sensitive data:** credentials (023) and recipes use Quark 020 at-rest encryption (007 §8).
   Points may be flagged sensitive, which excludes them from raw capture and export.
6. **Personal data:** AeroEdge holds none by design. Operator identity enters upstream. Audit
   records hold Studio user IDs and follow the customer's audit retention.

## 4. Sizing

Per-node disk budget is the sum of every class, not only the outbox:

```text
outbox      = M × E × B × 24 × N_days × 1.5        (safety factor incl. store/WAL overhead)
delivered   = M × E × B × 24 × 3
health      = D × 57.6 KB × 90                      (≈ 288 summaries/day × 200 B)
raw         = Σ(enabled points) × change_rate × 32 B × 86 400 × 3
```

M = machines, E = events per machine per hour, B = average event bytes, D = devices.

Worked example: 200 machines, 200 devices, 15 s windows, `count` + `reject` bound, 2 process
roles at 1 sample/min. E ≈ 240 + 240 + 20 + 120 = **620 events/h**, B ≈ 300 B, N = 7:

| Class | Size |
|---|---|
| Outbox (7 days) | ≈ 9.4 GB |
| Delivered log (3 days) | ≈ 2.7 GB |
| Health history (90 days) | ≈ 1.0 GB |
| Raw capture (50 points at 1 change/s, 3 days) | ≈ 0.4 GB |
| **Total** | **≈ 14 GB** |

Default target: **N = 7 days**. Ship a sizing calculator in Studio/CLI.

**Drain time:** after a 7-day outage the backlog is ≈ 20 M events. At 500 events/s that's about
11 h of drain, which is why the live lane exists (025 §2.2). Live data is current within
seconds while the backfill catches up.

## 5. Backup and restore

| Data | Backup |
|---|---|
| Control plane (registry, credentials, bindings, flows, audit) | scheduled snapshot of the Quark 012 store + documented restore. **Required** |
| Health history | optional |
| Outboxes | not backed up (transient). Protected by Sync persistence and N-day sizing |
| Raw capture, delivered log | not backed up |

**Restore bumps the control epoch.** Restoring the control plane from an older snapshot would
make its projection versions *older* than what the nodes hold. Nodes would then either reject
all new config, or, if forced, resurrect revoked credentials. So a restore starts a new
**`control_epoch`** (027 §3) and requires an explicit **reconcile** step:

1. Nodes report their current projection version and **local changes**, including emergency
   revokes (023 §5) and devices claimed since the backup.
2. The operator reviews the differences in Studio.
3. The **revocation list is always merged**. A credential revoked anywhere stays revoked.

## 6. Invariants (normative)

- **DP1**: an undelivered canonical event is never deleted to make room. Pressure turns into an
  alarm, then an ingest throttle. Any resulting count loss is reported as `CountGap`.
- **DP2**: raw capture is opt-in, change-only, and bounded by time **and** size.
- **DP3**: control-plane data is backup-eligible and restorable. A restore always starts a new
  control epoch with a reconcile that preserves revocations.
- **DP4**: every retention value is configurable per deployment, with the §2 defaults.
- **DP5**: AeroEdge produces no raw-data rollups. Aggregation beyond the canonical events is
  upstream.

## 7. Implementation status

Not started. Depends on 025 (producer outboxes, cursors), 027 (control epoch), 022 (health
history sources).

## 8. Open questions

- Store backend per class: `FileStore` for outboxes on small nodes, `RocksStore` for large ones
  (007 §10)?
- Should health history be forwarded to the control plane continuously, or pulled on demand?
