// The Flows page (016 §1): the Flow Designer + Deploy/Monitor panel, unchanged from the pre-016
// single-page Studio other than losing its own <header> (the AppShell now owns page chrome).
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

  const app = toApplication(model);
  const errors = validateApplication(app, sourceIds(), outputIds());

  const say = (m: string) => setLog((l) => [m, ...l].slice(0, 20));

  // Live monitoring: subscribe to the aero-api SSE metrics stream (013 §5). Each snapshot updates the
  // panel in place. The subscription is torn down when Live turns off or the component unmounts.
  useEffect(() => {
    if (!live) return;
    const unsubscribe = api.subscribeMetrics((s) => setStatus(s));
    return unsubscribe;
  }, [live, api]);

  const deploy = async () => {
    if (errors.length) { say(`✗ invalid: ${errors.join("; ")}`); return; }
    const r = await api.deploy(app);
    say(r.ok ? `✓ deployed ${app.name}@${app.version}` : `✗ deploy ${r.status}: ${JSON.stringify(r.body)}`);
  };
  const refresh = async () => {
    const r = await api.status();
    if (r.ok) setStatus(r.body as StatusSnapshot);
    say(r.ok ? "✓ status" : `✗ status ${r.status}`);
  };

  return (
    <div className="app">
      <FlowDesigner model={model} onChange={setModel} />

      <Panel title="Deploy & Monitor"
        actions={<><Button variant="primary" onClick={deploy} disabled={errors.length > 0}>Deploy</Button>
                   <Button onClick={refresh}>Refresh status</Button>
                   <Button variant={live ? "danger" : "default"} onClick={() => setLive((v) => !v)}>
                     {live ? "■ Stop live" : "● Go live"}</Button></>}>
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
