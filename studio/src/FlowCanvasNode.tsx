// The Flow Designer's custom @xyflow/react node card (Phase 11.8, 016 §5.2/S2): renders one
// FlowNode's catalog label/category/type_id plus the same add/remove/reorder controls the old linear
// list had. Purely a presentation upgrade — array order still IS the DAG (v0.1 flows are linear per
// spec 004); this card doesn't let a user express branching the schema doesn't support.
import { Handle, Position, type NodeProps } from "@xyflow/react";
import { Button } from "./components";
import type { CatalogEntry } from "./catalog";

export interface FlowCanvasNodeData {
  [key: string]: unknown;
  index: number;
  typeId: string;
  entry?: CatalogEntry;
  isSelected: boolean;
  first: boolean;
  last: boolean;
  onSelect: (i: number) => void;
  onMove: (i: number, dir: -1 | 1) => void;
  onRemove: (i: number) => void;
}

export function FlowCanvasNode({ data }: NodeProps) {
  const d = data as FlowCanvasNodeData;
  return (
    <div className={`flow-node-card${d.isSelected ? " sel" : ""}`} onClick={() => d.onSelect(d.index)}>
      {!d.first && <Handle type="target" position={Position.Top} />}
      <span className="badge">{d.entry?.category ?? "?"}</span>
      <div className="node-label">{d.entry?.label ?? d.typeId}</div>
      <div className="node-id">{d.typeId}</div>
      {/* stopPropagation at the row so a button click doesn't also fire the card's onSelect */}
      <div className="row-actions" onClick={(e) => e.stopPropagation()}>
        <Button ariaLabel="Move up" onClick={() => d.onMove(d.index, -1)}>↑</Button>
        <Button ariaLabel="Move down" onClick={() => d.onMove(d.index, 1)}>↓</Button>
        <Button ariaLabel="Remove node" variant="danger" onClick={() => d.onRemove(d.index)}>✕</Button>
      </div>
      {!d.last && <Handle type="source" position={Position.Bottom} />}
    </div>
  );
}
