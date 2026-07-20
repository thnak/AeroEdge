// The Fleet page (016 §2.1): nodes, capabilities, and device placement from GET /fleet. Real for what
// one daemon can see (§2.1's honest scope) — multi-node membership is still gated on Quark's
// productionized socket transport, unrelated to this page.
import { useEffect, useMemo, useState } from "react";
import { Panel, Button, StatCard, StatusPill, Table } from "../components";
import { AeroApi, type FleetStatus } from "../api";

export function FleetPage() {
  const api = useMemo(() => new AeroApi(), []);
  const [status, setStatus] = useState<FleetStatus | null>(null);
  const [error, setError] = useState<string | null>(null);

  const refresh = async () => {
    const r = await api.fleetStatus();
    if (r.ok) {
      setStatus(r.body as FleetStatus);
      setError(null);
    } else {
      setError(`fleet status ${r.status}`);
    }
  };

  useEffect(() => { void refresh(); }, [api]);

  if (!status) {
    return <p className="muted">{error ?? "Loading…"}</p>;
  }

  if (!status.configured) {
    return (
      <Panel title="Fleet">
        <p className="muted">
          No fleet configured. Start the daemon with <code>--fleet-device ID[:VERSION]</code> (repeat
          per device) and optionally <code>--fleet-node-flag FLAG</code> to see nodes and placement here.
        </p>
      </Panel>
    );
  }

  const nodes = status.nodes ?? [];
  const devices = status.devices ?? [];
  const unplaceable = devices.filter((d) => !d.eligible).length;

  return (
    <Panel title="Fleet" actions={<Button onClick={refresh}>Refresh</Button>}>
      <div className="stat-row">
        <StatCard label="Nodes" value={nodes.length} />
        <StatCard label="Devices" value={devices.length} />
        <StatCard label="Unplaceable" value={unplaceable} tone={unplaceable > 0 ? "error" : "ok"} />
      </div>

      <h3>Node capabilities</h3>
      <Table
        rows={nodes}
        rowKey={(n) => n.id}
        empty="No nodes."
        columns={[
          { header: "Node", render: (n) => n.id },
          { header: "Flags", render: (n) => (n.flags.length ? n.flags.join(", ") : <span className="muted">—</span>) },
        ]}
      />

      <h3>Device placement</h3>
      <Table
        rows={devices}
        rowKey={(d) => d.id}
        empty="No devices."
        columns={[
          { header: "Device", render: (d) => d.id },
          {
            header: "Placement",
            render: (d) =>
              d.eligible ? (
                <StatusPill tone="ok">node {d.node}</StatusPill>
              ) : (
                <StatusPill tone="error">unplaceable</StatusPill>
              ),
          },
        ]}
      />
    </Panel>
  );
}
