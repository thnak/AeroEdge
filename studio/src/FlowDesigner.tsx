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
import { FlowCanvasNode, SWITCH_TYPE_ID, CARD_WIDTH, CARD_VISUAL_HEIGHT, type FlowCanvasNodeData } from "./FlowCanvasNode";
import { SelfConnectingEdge } from "./SelfConnectingEdge";
import { SwitchBlockNode, type SwitchBlockNodeData } from "./SwitchBlockNode";
import { computeCavities, absorbedEdgeIds, attachToCavity, detachFromCavity, type BranchLabel } from "./cavity";
import { cavityRect, cavityMemberPosition } from "./switchCavityLayout";
import { parseExpr, collectTagRefs } from "./exprAst";

// 020 §4.2: a switch node renders as a C-block with nested cavities (switchBlock) once a flow has
// opted into graph mode; in legacy/array-order mode it keeps the plain two-tab jigsaw card (flowNode) —
// legacy mode has no `from_port` concept at all, so there is nothing to nest yet.
const nodeTypes = { flowNode: FlowCanvasNode, switchBlock: SwitchBlockNode };
// Registered as the "default" edge type (react-flow's own documented pattern) so every edge routes
// through it; it only special-cases rendering when source === target, otherwise it's a plain bezier.
const edgeTypes = { default: SelfConnectingEdge };

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
    positions[id] = { x: col * 220, y: d * 180 };
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

  // 020 §5: tag names to suggest in the expr-tree editor's tag-reference autocomplete. No server-side
  // tag metadata exists (Modbus/JSON source tag names are payload-dependent, unknowable statically) —
  // "raw" is the one hardcoded true constant (aero.source.decode's fixed literal tag), plus every tag
  // name already referenced by some OTHER node's expr in this flow, best-effort and free-text either way.
  const knownTags = useMemo(() => {
    const tags = new Set<string>(["raw"]);
    for (const n of model.nodes) {
      const expr = n.config?.expr;
      if (typeof expr === "string") {
        const parsed = parseExpr(expr);
        if (parsed.ok) for (const t of collectTagRefs(parsed.tree)) tags.add(t);
      }
    }
    return [...tags].sort();
  }, [model.nodes]);

  // Empty = legacy/array-order mode (same definition toApplication itself uses) — a switch node only
  // gets the C-block treatment once the flow has actually opted into edges[] (020 §4.2).
  const graphMode = model.edges.length > 0;
  const graphEdges = useMemo(
    () => (model.edges.length > 0 ? model.edges : implicitEdges(model.nodes)),
    [model.nodes, model.edges],
  );
  const layout = useMemo(() => autoLayout(model.nodes, graphEdges), [model.nodes, graphEdges]);

  // 020 §4.2: which nodes are nested inside a switch's cavities, purely a function of graphEdges — in
  // legacy mode this is always empty (implicit edges never carry a from_port), so no separate gate is
  // needed here, only where a switch's RENDER TYPE is chosen below.
  const cavities = useMemo(() => computeCavities(graphEdges), [graphEdges]);
  const absorbed = useMemo(() => absorbedEdgeIds(graphEdges, cavities.chains), [graphEdges, cavities.chains]);

  // A cavity member's position is ALWAYS derived (switch's own absolute position + its fixed slot in
  // the cavity) — never read from `positions`/`layout`, so it can't drift out of sync with the boxes
  // SwitchBlockNode itself draws. A free (non-nested) node keeps the existing drag/auto-layout posture.
  const absolutePosition = (id: string) => positions[id] ?? layout[id] ?? { x: 0, y: 0 };

  const rfNodes: Node[] = useMemo(() => {
    const built = model.nodes.map((n, i) => {
      const id = nodeId(n, i);
      const mem = cavities.membership.get(id);
      const isSwitch = n.type_id === SWITCH_TYPE_ID;

      if (mem) {
        const memBranches = cavities.chains.get(mem.switchId);
        const offset = cavityMemberPosition(
          mem.label, mem.index,
          memBranches?.get("true")?.length ?? 0, memBranches?.get("false")?.length ?? 0,
        );
        const switchPos = absolutePosition(mem.switchId);
        const data: FlowCanvasNodeData = {
          index: i, typeId: n.type_id, entry: catalogEntry(n.type_id),
          isSelected: selected === i, onSelect: setSelected, onMove: move, onRemove: removeNode,
        };
        return {
          id, type: "flowNode",
          position: { x: switchPos.x + offset.x, y: switchPos.y + offset.y },
          // Layer above the switch card's own SVG background WITHOUT reordering the `nodes` array —
          // moving members later in the array (instead of using zIndex) would change their rendered DOM
          // order relative to model.nodes, which flow_designer_canvas.test.tsx's row-action button
          // indices (and any future code with the same assumption) depend on staying in model order.
          zIndex: 1,
          data,
        };
      }

      if (isSwitch && graphMode) {
        const branches = cavities.chains.get(id);
        const data: SwitchBlockNodeData = {
          index: i, typeId: n.type_id,
          expr: typeof n.config?.expr === "string" ? n.config.expr : undefined,
          isSelected: selected === i,
          trueCount: branches?.get("true")?.length ?? 0,
          falseCount: branches?.get("false")?.length ?? 0,
          onSelect: setSelected, onMove: move, onRemove: removeNode,
        };
        return { id, type: "switchBlock", position: positions[id] ?? layout[id] ?? { x: 0, y: i * 110 }, data };
      }

      const data: FlowCanvasNodeData = {
        index: i, typeId: n.type_id, entry: catalogEntry(n.type_id),
        isSelected: selected === i, onSelect: setSelected, onMove: move, onRemove: removeNode,
      };
      return { id, type: "flowNode", position: positions[id] ?? layout[id] ?? { x: 0, y: i * 110 }, data };
    });
    return built;
  }, [model.nodes, selected, positions, layout, cavities, graphMode]);

  const rfEdges: Edge[] = useMemo(
    () =>
      graphEdges
        .filter((e) => !absorbed.has(e.id))
        .map((e) => ({
          id: e.id,
          source: e.from,
          sourceHandle: e.fromPort,
          target: e.to,
          label: e.fromPort,
          // A surviving edge whose source is itself nested can only be a "stub"/rejoin (020 §4.2's
          // off-page-connector case) — every chain-internal link was already filtered out above.
          className: cavities.membership.has(e.from) ? "edge-stub" : undefined,
        })),
    [graphEdges, absorbed, cavities.membership],
  );

  const onNodesChangeHandler = (changes: NodeChange[]) => {
    setPositions((prev) => {
      let next = prev;
      for (const c of changes) {
        if (c.type === "position" && c.position) {
          if (cavities.membership.has(c.id)) continue; // nested — position is always derived, never stored
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

  // 020 §4.2 — the actual "physical snap": dropping a free node so it overlaps a switch's cavity nests
  // it there (attachToCavity); dragging an already-nested node until it no longer overlaps its OWN
  // cavity detaches it (detachFromCavity). No react-flow parentId/extent is ever set (see
  // switchCavityLayout.ts's note on why), so `node.position` here is always an ABSOLUTE canvas
  // coordinate — no parent-relative conversion needed either way.
  const onNodeDragStop = (_event: unknown, node: Node) => {
    if (!graphMode) return; // legacy mode has no cavities to snap into at all
    const draggedIdx = model.nodes.findIndex((n, i) => nodeId(n, i) === node.id);
    if (draggedIdx === -1 || model.nodes[draggedIdx].type_id === SWITCH_TYPE_ID) return;

    const centerX = node.position.x + CARD_WIDTH / 2;
    const centerY = node.position.y + CARD_VISUAL_HEIGHT / 2;
    let landed: { switchId: string; label: BranchLabel } | undefined;
    model.nodes.forEach((n, i) => {
      if (n.type_id !== SWITCH_TYPE_ID) return;
      const switchId = nodeId(n, i);
      if (switchId === node.id) return;
      const switchPos = absolutePosition(switchId);
      const branches = cavities.chains.get(switchId);
      const trueCount = branches?.get("true")?.length ?? 0;
      const falseCount = branches?.get("false")?.length ?? 0;
      (["true", "false"] as const).forEach((label) => {
        const rect = cavityRect(label, trueCount, falseCount);
        const left = switchPos.x + rect.x;
        const top = switchPos.y + rect.y;
        if (centerX >= left && centerX <= left + rect.width && centerY >= top && centerY <= top + rect.height) {
          landed = { switchId, label };
        }
      });
    });

    const current = cavities.membership.get(node.id);
    if (landed) {
      if (!current || current.switchId !== landed.switchId || current.label !== landed.label) {
        onChange(attachToCavity(model, node.id, landed.switchId, landed.label));
      }
    } else if (current) {
      onChange(detachFromCavity(model, node.id));
    }
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
              edgeTypes={edgeTypes}
              nodesDraggable
              nodesConnectable
              edgesReconnectable
              onNodesChange={onNodesChangeHandler}
              onNodeDragStop={onNodeDragStop}
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
            knownTags={knownTags}
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
