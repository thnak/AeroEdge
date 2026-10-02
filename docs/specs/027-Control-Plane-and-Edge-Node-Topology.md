# 027 — Control Plane and Edge Node Topology

> Draft v0.2. Splits an AeroEdge deployment into a **control plane** (what the plant is
> configured to be) and **edge nodes** (what runs next to the machines). Amends 013's
> runtime-plane breakdown and gives 010's cluster a management home.
>
> v0.2 incorporates the red-team review:
> - `(control_epoch, version)` projections with reconcile after restore;
> - projections to all eligible nodes;
> - node-side inbound command routing;
> - per-node upstream credentials;
> - node-local emergency operations.

## 1. Two roles, one binary

`aero-runtime` (013 §2) runs in one of three roles:

| Role | Hosts | Typical placement |
|---|---|---|
| `control` | `DeviceRegistryActor` (022), identity issuance (023), binding store (024), firmware catalog (011), Application store (009), `aero-api` for Studio/CLI, audit, health-history aggregation (026) | plant server / VM |
| `node` | drivers (006), native broker (017), `EdgeActor`s, `MachineActor`s (024), producer outboxes + `UpstreamGateway` (025), raw capture (026), node-local `aero-api` (§3.1) | edge box near the machines |
| `all` | both, in one process | small sites (default install) |

One binary, a role flag. A small site installs once and gets a full system. A larger site adds
`node` instances without changing anything else.

## 2. Responsibilities

```text
                ┌──────────────────── control ────────────────────┐
 Studio / CLI ─▶│ aero-api · registry · identity · bindings ·      │
                │ firmware catalog · Application store · audit     │
                └───────────┬─────────────────────────────────────┘
                            │ versioned projections (push, ack per node)
          ┌─────────────────┼─────────────────┐
          ▼                 ▼                 ▼
   ┌─── node A ───┐  ┌─── node B ───┐  ┌─── node C ───┐
   │ drivers      │  │              │  │              │◀── inbound commands, master data
   │ broker       │  │     ...      │  │     ...      │    (any node's gateway, §2.1)
   │ Edge/Machine │  │              │  │              │──▶ canonical events (025)
   │ outboxes     │  │              │  │              │
   └──────────────┘  └──────────────┘  └──────────────┘
```

- **Control → node:** **versioned projections** (devices, points, bindings, ACL data,
  Applications), pushed as config Commands. Each node **acks** each version, and Studio shows
  per-node apply status. Every node receives the projection for **every device it is eligible to
  host** (capability tags, 022 §4), so failover works without the control plane. Nodes persist
  projections as Snapshot actor state.
- **Node → control:** health, connectivity, health history, metrics, OTA progress (016).
- **Node → upstream:** canonical events, delivered directly from the node's `UpstreamGateway`
  (025).

### 2.1 Inbound path

Upstream systems (AeroMes, AeroOEE) send inbound messages (025 §5) to **any node's** gateway
endpoint. Typically that's a site-level DNS name or load balancer over the nodes.

- **Commands** (`SetOutput`, `SetCurrentProduct`, device commands per 028) are routed to the
  target actor by **Quark actor routing**, so upstream never needs to know which node hosts which
  machine.
- **Master-data events** (`MachinesUpserted`, …) are forwarded to the control plane. Bindings
  need them, but they're not time-critical. While the control plane is down they're held by the
  receiving node and forwarded later.
- A control-plane outage therefore never stops an andon command (CP3).

### 2.2 Upstream credentials

Each node delivers upstream as its **own node identity** (023 §1), not with a shared service
credential. The upstream system (e.g. AeroMes) checks that the sending node is allowed to report
for that machine's site/segment. A compromised edge box can then forge counts only within its
own scope, and its identity can be revoked alone.

## 3. Autonomy

A node must keep running when the control plane is unreachable:

| Function | Control plane down |
|---|---|
| Drivers, flows, state derivation, counting | ✅ continues on last-known-good projection |
| Broker auth for known devices | ✅ from local projection, subject to the credential max-age (023 §5) |
| Failover of devices from a dead node | ✅ the projection is already on every eligible node (§2) |
| Upstream delivery, inbound commands | ✅ direct (§2.1) |
| Emergency revoke, kick a client, read live I/O | ✅ via node-local `aero-api` (§3.1) |
| New devices, rebinding, credential issue, OTA rollout, Studio edits | ⏸ waits |

### 3.1 Node-local `aero-api`

Each node exposes a restricted local API for operations that can't wait for the control plane:
emergency revoke (023 §5), kick a client, live I/O tap, broker admin read-outs, and local status.
Local changes are recorded and **reconciled** into the control plane when it's reachable.

### 3.2 Projection versions: `(control_epoch, version)`

- `version` increases with every control-plane change.
- `control_epoch` increases on every **restore** of the control plane from backup (026 §5).
- A node applies a projection only if it's newer: a higher epoch, or the same epoch with a higher
  version.
- On a new epoch, nodes don't auto-apply. The control plane runs the **reconcile** described in
  026 §5. Local changes and revocations are merged, so a restore can never resurrect a revoked
  credential.

## 4. Relationship to 010 (cluster)

- Nodes form the Quark cluster 010 describes. Placement and membership are unchanged.
- The control plane is the **source of the capability tags** placement uses (022 §4, resolving
  010 §6). Device affinity follows registry edits automatically.
- **Cross-node migration is not assumed** (010 §5). Failover is a cold start with explicit
  `CountGap` reporting (024 §5.1). When fenced migration with state transfer lands in Quark,
  024 and 025 can tighten their guarantees.
- The control plane isn't on the actor hot path. Version 1 is a single instance with
  backup/restore (026 §5). An HA control plane comes later.

## 5. Module changes vs 013

| Module | Change |
|---|---|
| `aero-registry` *(new)* | `DeviceRegistryActor`, projections, import/export (022), identity (023), bindings (024 §2) |
| `aero-machine` *(new)* | `MachineActor`, processing rules, state derivation, point quality (024) |
| `aero-mes` → `aero-upstream` | `UpstreamGateway`, `IUpstreamAdapter`, producer outbox helpers, MES profile, AeroOEE/AeroMes adapters (025) |
| `aero-drivers` | device protocol v4 codec + command channel (028), gateway/child demux (022 §2.1) |
| `aero-runtime` | role flag (`control` / `node` / `all`), node-local API |
| `aero-schema` | registry DTOs, canonical events (025 §3), device protocol v4 (028) |

Layering (CONVENTIONS) stays one-way. `aero-machine` depends on `aero-registry` (binding types)
and `aero-upstream` (outbox staging). `aero-registry` and `aero-upstream` don't depend on each
other or on `aero-machine`:

```text
aero-sdk → aero-core → {aero-nodes, aero-drivers, aero-registry, aero-upstream}
                     → aero-machine → aero-runtime → aero-api → aero-cli
```

## 6. Invariants (normative)

- **CP1**: a node never depends on the control plane being reachable to keep producing data,
  failing over devices, or accepting inbound commands.
- **CP2**: configuration flows control → node as `(control_epoch, version)` projections, acked
  per node. The only node-local writes are §3.1's emergency operations, which are reconciled.
- **CP3**: production data and commands flow node ↔ upstream directly. The control plane isn't a
  data or command relay.
- **CP4**: `all` is a supported production role, not a dev shortcut.
- **CP5**: each node reports upstream under its own node identity, scoped to its site/segment.

## 7. Implementation status

Not started. Today `aero-runtime` is effectively `all` with no registry.

## 8. Open questions

- Control-plane HA: active/passive with a shared store, or backup/restore only (026 §5)?
- Multi-site: one control plane per site, or one for many sites (WAN latency, 010 §6)? With
  multi-site comes **multi-tenant** isolation for integrators hosting several customers (P2).
- Inbound endpoint: a site-level load balancer, or upstream holds a list of node endpoints?
- Edge-box OS and network commissioning (static IP, controller test before activation): in scope
  for Studio, or left to the installer image?
