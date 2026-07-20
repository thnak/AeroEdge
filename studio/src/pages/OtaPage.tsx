// The OTA Rollout page (016 §2.2): wave-by-wave firmware rollout status + a start-rollout action,
// over GET/POST /ota/rollouts. Real orchestration policy (FleetActor canary→staged→full auto-pause,
// 011 §4/O5) driven against whatever OTA drivers are actually registered — today MockOtaDriver, since
// no real firmware-push driver/crypto exists yet (honest scope, R5). The UI surfaces that plainly
// rather than implying it pushes firmware to real hardware.
import { useEffect, useMemo, useState } from "react";
import { Panel, Button, Field, StatCard, StatusPill, Table } from "../components";
import { AeroApi, type OtaStatus } from "../api";

const STATE_TONE: Record<string, "ok" | "warn" | "error" | "neutral"> = {
  Idle: "neutral",
  Running: "warn",
  Paused: "error",
  Completed: "ok",
};

export function OtaPage() {
  const api = useMemo(() => new AeroApi(), []);
  const [status, setStatus] = useState<OtaStatus | null>(null);
  const [version, setVersion] = useState("2.0");
  const [bytes, setBytes] = useState("firmware-payload");
  const [log, setLog] = useState<string | null>(null);
  const [starting, setStarting] = useState(false);

  const refresh = async () => {
    const r = await api.otaStatus();
    if (r.ok) setStatus(r.body as OtaStatus);
    else setLog(`ota status ${r.status}`);
  };

  useEffect(() => { void refresh(); }, [api]);

  const start = async () => {
    setStarting(true);
    const r = await api.startRollout(version, bytes);
    setLog(r.ok ? `✓ rollout started for ${version}` : `✗ rollout ${r.status}: ${JSON.stringify(r.body)}`);
    if (r.ok) await refresh();
    setStarting(false);
  };

  if (!status) {
    return <p className="muted">{log ?? "Loading…"}</p>;
  }

  if (!status.configured) {
    return (
      <Panel title="OTA Rollout">
        <p className="muted">
          No fleet configured. Start the daemon with <code>--fleet-device ID[:VERSION]</code> to see
          rollout status and start a wave-by-wave update here.
        </p>
      </Panel>
    );
  }

  const waves = status.waves ?? [];

  return (
    <Panel title="OTA Rollout" actions={<Button onClick={refresh}>Refresh</Button>}>
      <div className="stat-row">
        <StatCard label="State" value={<StatusPill tone={STATE_TONE[status.state ?? "Idle"]}>{status.state}</StatusPill>} />
        <StatCard label="Devices updated" value={status.devices_updated ?? 0} tone="ok" />
        <StatCard label="Devices rolled back" value={status.devices_rolled_back ?? 0}
                  tone={(status.devices_rolled_back ?? 0) > 0 ? "error" : undefined} />
      </div>

      <h3>Waves</h3>
      <Table
        rows={waves}
        rowKey={(w) => w.name}
        empty="No rollout has run yet."
        columns={[
          { header: "Wave", render: (w) => w.name },
          { header: "Attempted", render: (w) => w.attempted },
          { header: "Succeeded", render: (w) => w.succeeded },
          { header: "Success rate", render: (w) => `${Math.round(w.success_rate * 100)}%` },
          { header: "Result", render: (w) => <StatusPill tone={w.passed ? "ok" : "error"}>{w.passed ? "passed" : "failed"}</StatusPill> },
        ]}
      />

      <h3>Start a rollout</h3>
      <Field label="Version"><input value={version} onChange={(e) => setVersion(e.target.value)} /></Field>
      <Field label="Payload bytes" help="A mock image body — no real crypto/signing yet (011 §6 gate, 016 §2.2)">
        <input value={bytes} onChange={(e) => setBytes(e.target.value)} />
      </Field>
      <Button variant="primary" onClick={start} disabled={starting || !version}>
        {starting ? "Starting…" : "Start rollout"}
      </Button>
      {log && <p className="muted">{log}</p>}
    </Panel>
  );
}
