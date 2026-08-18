// The Flow Designer's custom @xyflow/react node card (Phase 11.8, 016 §5.2/S2; graph editing 019 §4):
// renders one FlowNode's catalog label/category/type_id plus the same add/remove/reorder controls the
// old linear list had, PLUS real connect handles now that the canvas is a graph editor. Handles are a
// Studio-only presentation detail — NOT a `NodeDescriptor.ports` concept (019 §2 explicitly cut that:
// nodes have no real per-input slot to advertise). Every node gets one target handle + one generic
// source handle, EXCEPT `aero.flow.switch`, which trades its one generic source handle for a labeled
// "true"/"false" pair (019 §4: "today that means exactly aero.flow.switch..."). Deliberately NOT
// gated by category (e.g. hiding an Output's source handle) — legacy array-order mode allows a
// mid-chain Output node (e.g. aero.output.mes as a side-effect that doesn't stop the flow), and the
// implicit chain fallback needs a real handle to draw that edge from; category-gating broke exactly
// this case in manual testing (react-flow error #008, "couldn't create edge for source handle").
import { Handle, Position, type NodeProps } from "@xyflow/react";
import { Button } from "./components";
import type { CatalogEntry } from "./catalog";

const SWITCH_TYPE_ID = "aero.flow.switch";

export interface FlowCanvasNodeData {
  [key: string]: unknown;
  index: number;
  typeId: string;
  entry?: CatalogEntry;
  isSelected: boolean;
  onSelect: (i: number) => void;
  onMove: (i: number, dir: -1 | 1) => void;
  onRemove: (i: number) => void;
}

export function FlowCanvasNode({ data }: NodeProps) {
  const d = data as FlowCanvasNodeData;
  const isSwitch = d.typeId === SWITCH_TYPE_ID;

  return (
    <div className={`flow-node-card${d.isSelected ? " sel" : ""}`} onClick={() => d.onSelect(d.index)}>
      <Handle type="target" position={Position.Top} />
      <span className="badge">{d.entry?.category ?? "?"}</span>
      <div className="node-label">{d.entry?.label ?? d.typeId}</div>
      <div className="node-id">{d.typeId}</div>
      {/* stopPropagation at the row so a button click doesn't also fire the card's onSelect */}
      <div className="row-actions" onClick={(e) => e.stopPropagation()}>
        <Button ariaLabel="Move up" onClick={() => d.onMove(d.index, -1)}>↑</Button>
        <Button ariaLabel="Move down" onClick={() => d.onMove(d.index, 1)}>↓</Button>
        <Button ariaLabel="Remove node" variant="danger" onClick={() => d.onRemove(d.index)}>✕</Button>
      </div>
      {isSwitch ? (
        <>
          <Handle type="source" id="true" position={Position.Bottom} style={{ left: "30%" }} className="branch-handle branch-true">
            <span className="branch-label">true</span>
          </Handle>
          <Handle type="source" id="false" position={Position.Bottom} style={{ left: "70%" }} className="branch-handle branch-false">
            <span className="branch-label">false</span>
          </Handle>
        </>
      ) : (
        <Handle type="source" position={Position.Bottom} />
      )}
    </div>
  );
}
