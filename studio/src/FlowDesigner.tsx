// The Flow Designer (013 §5, 016 §5.2, graph editing 019 §4): add/remove/reorder nodes, pick types
// from the catalog, edit config via the Tier-1 form, set actor + driver, and wire real connections on
// a node-graph canvas — emitting a schema-aligned Application (application.ts). The emitted JSON is
// what deploys, so alignment is the whole point.
//
// A flow with no manually-drawn connection stays in LEGACY mode: array order IS the DAG (v0.1, spec
// 004), `model.edges` is empty, and `toApplication` emits the historical minimal shape — the
// hello_flow round-trip (application.test.ts) is unaffected. The first time a user draws or deletes a
// connection on the canvas, the model materializes into GRAPH mode (`model.edges` becomes
// authoritative; see application.ts's `withEdge`/`withoutEdge`) and stays there. Node *position* is
// separate again: session-only client state (`positions`), auto-laid-out by topological depth and
// draggable, but never part of `model`/the wire Application (019 §4 — nothing re-fetches a deployed
// Application to repopulate the Designer today, so persisting layout on the wire would be dead weight
// and would break the round-trip test's exact-shape guarantee).
import { useMemo, useState } from "react";
import {
  ReactFlow,
  type Node,
  type Edge,
  type NodeChange,
  type Connection,
} from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import { getCatalog, catalogEntry } from "./catalog";
import type { FlowModel, FlowNode, GraphEdge } from "./application";
import { toApplication, implicitEdges, withEdge, withoutEdge, removeNodeAndEdges, nodeId } from "./application";
import { Panel } from "./components";
import { ConfigForm } from "./ConfigForm";
import { FlowCanvasNode, type FlowCanvasNodeData } from "./FlowCanvasNode";

const nodeTypes = { flowNode: FlowCanvasNode };

// Layered topological auto-layout: depth = longest path from a root (BFS-memoized), x spread by
// column within each depth layer. Purely a rendering default — `positions` (drag results) always
// wins once set. A cycle can't happen here without `validateGraph` already flagging it elsewhere; the
// `seen` guard just keeps this from infinite-looping while that error is being fixed.
function autoLayout(nodes: FlowNode[], edges: GraphEdge[]): Record<string, { x: number; y: number }> {
  const ids = nodes.map((n, i) => nodeId(n, i));
  const idSet = new Set(ids);
  const incoming = new Map<string, string[]>();
  for (const id of ids) incoming.set(id, []);
  for (const e of edges) {
    if (idSet.has(e.to) && idSet.has(e.from)) incoming.get(e.to)!.push(e.from);
  }
  const depth = new Map<string, number>();
  const resolve = (id: string, seen: Set<string>): number => {
    if (depth.has(id)) return depth.get(id)!;
    if (seen.has(id)) return 0;
    seen.add(id);
    const preds = incoming.get(id) ?? [];
    const d = preds.length === 0 ? 0 : Math.max(...preds.map((p) => resolve(p, seen))) + 1;
    depth.set(id, d);
    return d;
  };
  for (const id of ids) resolve(id, new Set());

  const perLayer = new Map<number, number>();
  const positions: Record<string, { x: number; y: number }> = {};
  for (const id of ids) {
    const d = depth.get(id) ?? 0;
    const col = perLayer.get(d) ?? 0;
    perLayer.set(d, col + 1);
    positions[id] = { x: col * 220, y: d * 140 };
  }
  return positions;
}

export function FlowDesigner({ model, onChange }: { model: FlowModel; onChange: (m: FlowModel) => void }) {
  const [selected, setSelected] = useState<number | null>(model.nodes.length ? 0 : null);
  const [positions, setPositions] = useState<Record<string, { x: number; y: number }>>({});

  const setNodes = (nodes: FlowNode[]) => onChange({ ...model, nodes });
  const addNode = (type_id: string) => {
    const entry = catalogEntry(type_id);
    const config: Record<string, number | string | boolean> = {};
    entry?.fields.forEach((f) => { if (f.default !== undefined) config[f.key] = f.default; });
    const nodes = [...model.nodes, { id: crypto.randomUUID(), type_id, config }];
    setNodes(nodes);
    setSelected(nodes.length - 1);
  };
  const removeNode = (i: number) => {
    onChange(removeNodeAndEdges(model, i));
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

  const graphEdges = useMemo(
    () => (model.edges.length > 0 ? model.edges : implicitEdges(model.nodes)),
    [model.nodes, model.edges],
  );
  const layout = useMemo(() => autoLayout(model.nodes, graphEdges), [model.nodes, graphEdges]);

  const rfNodes: Node<FlowCanvasNodeData>[] = useMemo(
    () =>
      model.nodes.map((n, i) => {
        const id = nodeId(n, i);
        return {
          id,
          type: "flowNode",
          position: positions[id] ?? layout[id] ?? { x: 0, y: i * 110 },
          data: {
            index: i,
            typeId: n.type_id,
            entry: catalogEntry(n.type_id),
            isSelected: selected === i,
            onSelect: setSelected,
            onMove: move,
            onRemove: removeNode,
          },
        };
      }),
    [model.nodes, selected, positions, layout],
  );

  const rfEdges: Edge[] = useMemo(
    () =>
      graphEdges.map((e) => ({
        id: e.id,
        source: e.from,
        sourceHandle: e.fromPort,
        target: e.to,
        label: e.fromPort,
      })),
    [graphEdges],
  );

  const onNodesChangeHandler = (changes: NodeChange[]) => {
    setPositions((prev) => {
      let next = prev;
      for (const c of changes) {
        if (c.type === "position" && c.position) {
          if (next === prev) next = { ...prev };
          next[c.id] = c.position;
        }
      }
      return next;
    });
  };

  const onConnect = (conn: Connection) => {
    if (!conn.source || !conn.target) return;
    const fromPort = conn.sourceHandle ?? undefined;
    if (graphEdges.some((e) => e.from === conn.source && e.to === conn.target && e.fromPort === fromPort)) return;
    onChange(withEdge(model, { id: `e:${conn.source}:${fromPort ?? ""}:${conn.target}`, from: conn.source, fromPort, to: conn.target }));
  };

  const onEdgesDelete = (edges: Edge[]) => {
    let next = model;
    for (const e of edges) next = withoutEdge(next, e.id);
    onChange(next);
  };

  return (
    <div className="designer">
      <Panel title="Flow"
        actions={
          <select value="" onChange={(e) => e.target.value && addNode(e.target.value)}>
            <option value="">+ Add node…</option>
            {getCatalog().nodes.map((e) => <option key={e.type_id} value={e.type_id}>{e.category}: {e.label}</option>)}
          </select>
        }>
        {model.nodes.length === 0 ? (
          <p className="muted">Empty flow. Add a Source node to start.</p>
        ) : (
          <div className="flow-canvas">
            <ReactFlow
              nodes={rfNodes}
              edges={rfEdges}
              nodeTypes={nodeTypes}
              nodesDraggable
              nodesConnectable
              edgesReconnectable
              onNodesChange={onNodesChangeHandler}
              onConnect={onConnect}
              onEdgesDelete={onEdgesDelete}
              panOnScroll
              fitView
              fitViewOptions={{ padding: 0.3 }}
              proOptions={{ hideAttribution: true }}
            />
          </div>
        )}
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
            {getCatalog().drivers.map((d) => <option key={d.type_id} value={d.type_id}>{d.label}</option>)}
          </select>
        </label>
      </Panel>

      <Panel title="Application (generated)">
        <pre className="json">{JSON.stringify(app, null, 2)}</pre>
      </Panel>
    </div>
  );
}
