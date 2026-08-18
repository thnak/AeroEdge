// The Blockly-style C-block render for aero.flow.switch (020 §4.2): one big card with two labeled
// "true"/"false" cavities physically drawn INSIDE it. Branch members are ordinary FlowCanvasNode cards
// FlowDesigner.tsx positions to sit visually inside those cavities — not react-flow parentId/extent
// nesting (that clamps a child inside its parent's bounds, which would make dragging a member back OUT
// to detach it physically impossible; see switchCavityLayout.ts's own note) — so this component only
// draws the switch's own header + the two dashed drop-zone boxes, never the members themselves.
//
// Replaces the "body-branch" two-dangling-tabs shape (019 follow-up, FlowCanvasNode.tsx) for a switch
// node ONCE a flow is in graph mode — a legacy/array-order flow has no `from_port` concept at all (see
// FlowDesigner.tsx's `graphMode` gate), so a switch there still renders as the old two-tab card until
// the user actually wires it into the graph.
import { Handle, Position, type NodeProps } from "@xyflow/react";
import { Button } from "./components";
import { jigsawPath } from "./jigsawPath";
import { cavityRect, switchBlockSize, type BranchLabel } from "./switchCavityLayout";

export interface SwitchBlockNodeData {
  [key: string]: unknown;
  index: number;
  typeId: string;
  expr?: string;
  isSelected: boolean;
  trueCount: number;
  falseCount: number;
  onSelect: (i: number) => void;
  onMove: (i: number, dir: -1 | 1) => void;
  onRemove: (i: number) => void;
}

function CavityBox({ label, trueCount, falseCount }: { label: BranchLabel; trueCount: number; falseCount: number }) {
  const rect = cavityRect(label, trueCount, falseCount);
  return (
    <div
      className={`cavity cavity-${label}`}
      data-cavity-label={label}
      style={{ left: rect.x, top: rect.y, width: rect.width, height: rect.height }}
    >
      <span className="cavity-label">{label}</span>
    </div>
  );
}

export function SwitchBlockNode({ data }: NodeProps) {
  const d = data as SwitchBlockNodeData;
  const { width, height } = switchBlockSize(d.trueCount, d.falseCount);
  const path = jigsawPath({ width, height, topNotch: true, bottomTabs: 0 });

  return (
    <div
      className={`switch-block${d.isSelected ? " sel" : ""}`}
      style={{ width, height }}
      onClick={() => d.onSelect(d.index)}
    >
      <svg className="jigsaw-shape" viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none">
        <path d={path} />
      </svg>
      <Handle type="target" position={Position.Top} />
      <div className="switch-block-header node-content">
        <span className="badge">Rule</span>
        <div className="node-label">Switch</div>
        <div className="node-id">{d.typeId}</div>
        {d.expr && <div className="switch-expr">{d.expr}</div>}
        {/* stopPropagation so a button click doesn't also fire the card's onSelect (FlowCanvasNode's
            own row-actions do the same) */}
        <div className="row-actions" onClick={(e) => e.stopPropagation()}>
          <Button ariaLabel="Move up" onClick={() => d.onMove(d.index, -1)}>↑</Button>
          <Button ariaLabel="Move down" onClick={() => d.onMove(d.index, 1)}>↓</Button>
          <Button ariaLabel="Remove node" variant="danger" onClick={() => d.onRemove(d.index)}>✕</Button>
        </div>
      </div>
      <CavityBox label="true" trueCount={d.trueCount} falseCount={d.falseCount} />
      <CavityBox label="false" trueCount={d.trueCount} falseCount={d.falseCount} />
    </div>
  );
}
