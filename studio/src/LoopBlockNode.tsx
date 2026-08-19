// The Blockly-style C-block render for aero.flow.loop_start (020 §8, Studio side): a header card with
// one "body" cavity nested INSIDE it, mirroring SwitchBlockNode.tsx's own shape but with a single
// cavity instead of two. aero.flow.loop_back stays a fully independent, plain jigsaw card
// (FlowCanvasNode) that FlowDesigner.tsx positions flush against the cavity's bottom edge — see
// loopCavityLayout.ts's own note on why it's deliberately not fused into this component's SVG.
import { Handle, Position, type NodeProps } from "@xyflow/react";
import { Button } from "./components";
import { jigsawPath } from "./jigsawPath";
import { cavityRect, loopBlockSize } from "./loopCavityLayout";

export interface LoopBlockNodeData {
  [key: string]: unknown;
  index: number;
  typeId: string;
  counterTag?: string;
  startExpr?: string;
  isSelected: boolean;
  memberCount: number;
  onSelect: (i: number) => void;
  onMove: (i: number, dir: -1 | 1) => void;
  onRemove: (i: number) => void;
}

function CavityBox({ memberCount }: { memberCount: number }) {
  const rect = cavityRect(memberCount);
  return (
    <div className="cavity cavity-loop" style={{ left: rect.x, top: rect.y, width: rect.width, height: rect.height }}>
      <span className="cavity-label">body</span>
    </div>
  );
}

export function LoopBlockNode({ data }: NodeProps) {
  const d = data as LoopBlockNodeData;
  const { width, height } = loopBlockSize(d.memberCount);
  const path = jigsawPath({ width, height, topNotch: true, bottomTabs: 0 });

  return (
    <div className={`loop-block${d.isSelected ? " sel" : ""}`} style={{ width, height }} onClick={() => d.onSelect(d.index)}>
      <svg className="jigsaw-shape" viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none">
        <path d={path} />
      </svg>
      <Handle type="target" position={Position.Top} />
      <div className="loop-block-header node-content">
        <span className="badge">Rule</span>
        <div className="node-label">⟲ Loop</div>
        <div className="node-id">{d.typeId}</div>
        {d.counterTag && (
          <div className="loop-summary">{d.counterTag} = {d.startExpr ?? "?"} …</div>
        )}
        <div className="row-actions" onClick={(e) => e.stopPropagation()}>
          <Button ariaLabel="Move up" onClick={() => d.onMove(d.index, -1)}>↑</Button>
          <Button ariaLabel="Move down" onClick={() => d.onMove(d.index, 1)}>↓</Button>
          <Button ariaLabel="Remove node" variant="danger" onClick={() => d.onRemove(d.index)}>✕</Button>
        </div>
      </div>
      <CavityBox memberCount={d.memberCount} />
    </div>
  );
}
