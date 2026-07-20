// The Flows page (016 §1): the Flow Designer + Deploy/Monitor/History panel. `Runtime` hosts exactly
// one Application at a time (016 §1 correction) — POST /apps only works the FIRST time; once
// something is deployed, changing the flow and clicking Deploy again must PUT /apps/{name} (hot-
// reload, 009 §4) instead, or the runtime rejects it ("a runtime hosts one Application"). Rollback
// (009 §6) reverts to the one prior version Runtime keeps.
import { useEffect, useMemo, useState } from "react";
import { FlowDesigner } from "../FlowDesigner";
import { Panel, Button } from "../components";
import { AeroApi, type StatusSnapshot } from "../api";
import { toApplication, validateApplication, fromApplication, type FlowModel, type Application } from "../application";
import { sourceIds, outputIds } from "../catalog";

const HELLO: Application = {
  name: "hello_flow",
  version: "0.1.0",
  actor: { kind: "edge", key: 7 },
  flow: [
    { type_id: "aero.source.decode" },
    { type_id: "aero.transform.scale", config: { factor: 2 } },
    { type_id: "aero.output.sum" },
  ],
  driver: { type_id: "aero.driver.generator", config: { frame_count: 100 } },
};

export function FlowsPage() {
  const api = useMemo(() => new AeroApi(), []);
  const [model, setModel] = useState<FlowModel>(() => fromApplication(HELLO));
  const [log, setLog] = useState<string[]>([]);
  const [status, setStatus] = useState<StatusSnapshot | null>(null);
  const [live, setLive] = useState(false);
  const [deployed, setDeployed] = useState(false);

  const app = toApplication(model);
  const errors = validateApplication(app, sourceIds(), outputIds());

  const say = (m: string) => setLog((l) => [m, ...l].slice(0, 20));

  // On mount: pick up whatever the daemon already has running (e.g. a page reload) so Deploy targets
  // reload/PUT instead of a doomed second POST.
  useEffect(() => {
    void api.status().then((r) => {
      if (r.ok) {
        const s = r.body as StatusSnapshot;
        setStatus(s);
        setDeployed(Boolean(s.deployed));
      }
    });
  }, [api]);

  // Live monitoring: subscribe to the aero-api SSE metrics stream (013 §5). Each snapshot updates the
  // panel in place. The subscription is torn down when Live turns off or the component unmounts.
  useEffect(() => {
    if (!live) return;
    const unsubscribe = api.subscribeMetrics((s) => setStatus(s));
    return unsubscribe;
  }, [live, api]);

  const deploy = async () => {
    if (errors.length) { say(`✗ invalid: ${errors.join("; ")}`); return; }
    const r = deployed ? await api.reload(app.name, app) : await api.deploy(app);
    if (r.ok) {
      setDeployed(true);
      say(`✓ ${deployed ? "reloaded" : "deployed"} ${app.name}@${app.version}`);
    } else {
      say(`✗ ${deployed ? "reload" : "deploy"} ${r.status}: ${JSON.stringify(r.body)}`);
    }
  };
  const refresh = async () => {
    const r = await api.status();
    if (r.ok) setStatus(r.body as StatusSnapshot);
    say(r.ok ? "✓ status" : `✗ status ${r.status}`);
  };
  const rollback = async () => {
    const r = await api.rollback(app.name);
    say(r.ok ? `✓ rolled back ${app.name}` : `✗ rollback ${r.status}: ${JSON.stringify(r.body)}`);
    if (r.ok) await refresh();
  };
  const undeploy = async () => {
    const r = await api.undeploy(app.name);
    if (r.ok) {
      setDeployed(false);
      setStatus(null);
      say(`✓ undeployed ${app.name}`);
    } else {
      say(`✗ undeploy ${r.status}: ${JSON.stringify(r.body)}`);
    }
  };

  return (
    <div className="app">
      <FlowDesigner model={model} onChange={setModel} />

      <Panel title="Deploy & Monitor"
        actions={<>
          <Button variant="primary" onClick={deploy} disabled={errors.length > 0}>
            {deployed ? "Redeploy (hot-reload)" : "Deploy"}
          </Button>
          <Button onClick={refresh}>Refresh status</Button>
          <Button onClick={rollback} disabled={!deployed}>Rollback</Button>
          <Button variant="danger" onClick={undeploy} disabled={!deployed}>Undeploy</Button>
          <Button variant={live ? "danger" : "default"} onClick={() => setLive((v) => !v)}>
            {live ? "■ Stop live" : "● Go live"}</Button>
        </>}>
        {live && <p className="live-badge">● live — streaming metrics over SSE</p>}
        {errors.length > 0 && <p className="field-error">{errors.join("; ")}</p>}
        {status && (
          <dl className="status">
            <div><dt>deployed</dt><dd>{String(status.deployed)}</dd></div>
            <div><dt>frames</dt><dd>{status.frames_processed ?? "—"}</dd></div>
            <div><dt>events</dt><dd>{status.events_published ?? "—"}</dd></div>
            <div><dt>last output</dt><dd>{status.last_output ?? "—"}</dd></div>
          </dl>
        )}
        <ul className="log">{log.map((l, i) => <li key={i}>{l}</li>)}</ul>
      </Panel>
    </div>
  );
}
