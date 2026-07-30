// The Broker page (017 §1/§2/M4): whether the native MQTT broker is configured, and if so, its
// listen port — Runtime.broker_status() via GET /broker/status. Phase 1's NativeBroker exposes
// only listen_port() beyond start/stop/on_publish (no session/subscription introspection yet, see
// runtime.hpp broker_status()), so this page reports only that, honestly, rather than inventing
// counters the daemon can't back.
import { useEffect, useMemo, useState } from "react";
import { Panel, Button, StatCard } from "../components";
import { AeroApi, type BrokerStatus } from "../api";

export function BrokerPage() {
  const api = useMemo(() => new AeroApi(), []);
  const [status, setStatus] = useState<BrokerStatus | null>(null);
  const [error, setError] = useState<string | null>(null);

  const refresh = async () => {
    const r = await api.brokerStatus();
    if (r.ok) {
      setStatus(r.body as BrokerStatus);
      setError(null);
    } else {
      setError(`broker status ${r.status}`);
    }
  };

  useEffect(() => { void refresh(); }, [api]);

  if (!status) {
    return <p className="muted">{error ?? "Loading…"}</p>;
  }

  if (!status.configured) {
    return (
      <Panel title="Broker">
        <p className="muted">
          No native broker configured. Start the daemon with <code>--broker-port PORT</code>{" "}
          (optionally <code>--broker-bind ADDR</code>) to see broker status here.
        </p>
      </Panel>
    );
  }

  return (
    <Panel title="Broker" actions={<Button onClick={refresh}>Refresh</Button>}>
      <div className="stat-row">
        <StatCard label="Listen port" value={status.listen_port ?? "—"} tone="ok" />
      </div>
      <p className="muted">
        Session and subscription introspection isn't exposed by the broker yet (017 Phase 1) — this
        is all the daemon can report today.
      </p>
    </Panel>
  );
}
