// The Flow Designer (013 §5): add/remove/reorder nodes into a linear flow, pick types from the
// catalog, edit config via the Tier-1 form, set actor + driver — and emit a schema-aligned
// Application (application.ts). The emitted JSON is what deploys, so alignment is the whole point.
import { useState } from "react";
import { NODE_CATALOG, DRIVER_CATALOG, catalogEntry } from "./catalog";
import type { FlowModel, FlowNode } from "./application";
import { toApplication } from "./application";
import { Panel, Button } from "./components";
import { ConfigForm } from "./ConfigForm";

export function FlowDesigner({ model, onChange }: { model: FlowModel; onChange: (m: FlowModel) => void }) {
  const [selected, setSelected] = useState<number | null>(model.nodes.length ? 0 : null);

  const setNodes = (nodes: FlowNode[]) => onChange({ ...model, nodes });
  const addNode = (type_id: string) => {
    const entry = catalogEntry(type_id);
    const config: Record<string, number | string | boolean> = {};
    entry?.fields.forEach((f) => { if (f.default !== undefined) config[f.key] = f.default; });
    const nodes = [...model.nodes, { type_id, config }];
    setNodes(nodes);
    setSelected(nodes.length - 1);
  };
  const removeNode = (i: number) => {
    setNodes(model.nodes.filter((_, idx) => idx !== i));
    setSelected(null);
  };
  const move = (i: number, dir: -1 | 1) => {
    const j = i + dir;
    if (j < 0 || j >= model.nodes.length) return;
    const nodes = [...model.nodes];
    [nodes[i], nodes[j]] = [nodes[j], nodes[i]];
    setNodes(nodes);
    setSelected(j);
  };
  const setConfig = (i: number, config: Record<string, number | string | boolean>) =>
    setNodes(model.nodes.map((n, idx) => (idx === i ? { ...n, config } : n)));

  const app = toApplication(model);

  return (
    <div className="designer">
      <Panel title="Flow"
        actions={
          <select value="" onChange={(e) => e.target.value && addNode(e.target.value)}>
            <option value="">+ Add node…</option>
            {NODE_CATALOG.map((e) => <option key={e.type_id} value={e.type_id}>{e.category}: {e.label}</option>)}
          </select>
        }>
        <ol className="flow-list">
          {model.nodes.map((n, i) => {
            const e = catalogEntry(n.type_id);
            return (
              <li key={i} className={selected === i ? "sel" : ""} onClick={() => setSelected(i)}>
                <span className="badge">{e?.category ?? "?"}</span>
                <span className="node-label">{e?.label ?? n.type_id}</span>
                <span className="node-id">{n.type_id}</span>
                <span className="row-actions">
                  <Button onClick={() => move(i, -1)}>↑</Button>
                  <Button onClick={() => move(i, 1)}>↓</Button>
                  <Button variant="danger" onClick={() => removeNode(i)}>✕</Button>
                </span>
              </li>
            );
          })}
          {model.nodes.length === 0 && <li className="muted">Empty flow. Add a Source node to start.</li>}
        </ol>
      </Panel>

      <Panel title="Configure">
        {selected !== null && model.nodes[selected] ? (
          <ConfigForm
            entry={catalogEntry(model.nodes[selected].type_id)!}
            config={model.nodes[selected].config ?? {}}
            onChange={(c) => setConfig(selected, c)}
          />
        ) : (
          <p className="muted">Select a node to configure it.</p>
        )}
      </Panel>

      <Panel title="Driver + Actor">
        <label className="field"><span className="field-label">Actor kind</span>
          <input value={model.actorKind} onChange={(e) => onChange({ ...model, actorKind: e.target.value })} /></label>
        <label className="field"><span className="field-label">Actor key</span>
          <input type="number" value={model.actorKey} onChange={(e) => onChange({ ...model, actorKey: Number(e.target.value) })} /></label>
        <label className="field"><span className="field-label">Driver</span>
          <select value={model.driver?.type_id ?? ""}
            onChange={(e) => onChange({ ...model, driver: e.target.value ? { type_id: e.target.value, config: { frame_count: 100 } } : undefined })}>
            <option value="">— none —</option>
            {DRIVER_CATALOG.map((d) => <option key={d.type_id} value={d.type_id}>{d.label}</option>)}
          </select>
        </label>
      </Panel>

      <Panel title="Application (generated)">
        <pre className="json">{JSON.stringify(app, null, 2)}</pre>
      </Panel>
    </div>
  );
}
