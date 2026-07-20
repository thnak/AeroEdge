// The MES Outbox page (016 §2.3): staged/pending/delivered counters + a manual drain, over GET
// /mes/outbox and POST /mes/outbox/drain. Unlike Fleet/OTA this subsystem is real end to end, no
// mock-driver gate — RestMesAdapter talks to a genuine HTTP MES (012 §3). The backend exposes
// aggregate counts only (Runtime::mes_outbox_stats), not a per-entry list, so this page doesn't
// invent one.
import { useEffect, useMemo, useState } from "react";
import { Panel, Button, StatCard } from "../components";
import { AeroApi, type MesOutboxStats } from "../api";

export function MesPage() {
  const api = useMemo(() => new AeroApi(), []);
  const [status, setStatus] = useState<MesOutboxStats | null>(null);
  const [log, setLog] = useState<string | null>(null);
  const [draining, setDraining] = useState(false);

  const refresh = async () => {
    const r = await api.mesOutboxStats();
    if (r.ok) setStatus(r.body as MesOutboxStats);
    else setLog(`mes status ${r.status}`);
  };

  useEffect(() => { void refresh(); }, [api]);

  const drain = async () => {
    setDraining(true);
    const r = await api.drainMesOutbox();
    setLog(r.ok ? "✓ drain requested" : `✗ drain ${r.status}: ${JSON.stringify(r.body)}`);
    if (r.ok) await refresh();
    setDraining(false);
  };

  if (!status) {
    return <p className="muted">{log ?? "Loading…"}</p>;
  }

  if (!status.configured) {
    return (
      <Panel title="MES Outbox">
        <p className="muted">
          No MES gateway configured. Start the daemon with <code>--mes-host H --mes-port P</code>
          (optionally <code>--mes-path</code>/<code>--mes-token</code>) to see outbox stats here.
        </p>
      </Panel>
    );
  }

  const pending = status.pending ?? 0;

  return (
    <Panel title="MES Outbox" actions={<Button onClick={refresh}>Refresh</Button>}>
      <div className="stat-row">
        <StatCard label="Staged" value={status.staged ?? 0} />
        <StatCard label="Pending" value={pending} tone={pending > 0 ? "warn" : "ok"} />
        <StatCard label="Delivered" value={status.delivered ?? 0} tone="ok" />
      </div>
      <p className="muted">
        A report is staged durably before it is acked (M3) — an MES outage delays delivery, it never
        drops a report. Pending entries stay retained until the MES is reachable again.
      </p>
      <Button variant="primary" onClick={drain} disabled={draining || pending === 0}>
        {draining ? "Draining…" : "Drain outbox"}
      </Button>
      {log && <p className="muted">{log}</p>}
    </Panel>
  );
}
