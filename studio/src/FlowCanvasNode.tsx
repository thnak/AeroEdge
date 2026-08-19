// The Flow Designer's custom @xyflow/react node card (Phase 11.8, 016 §5.2/S2; graph editing 019 §4;
// jigsaw shapes 019 §4 follow-up): renders one FlowNode's catalog label/category/type_id, the same
// add/remove/reorder controls the old linear list had, and real connect handles — but now shaped as a
// puzzle piece (`jigsawPath.ts`) instead of a plain rectangle, so a category that can't legitimately
// receive an incoming edge (Source — it decodes `ctx.payload`, not anything upstream) has no input
// notch to plug into at all. This is deliberately a 2-shape system, not per-type: the architecture has
// no per-node port/type contract (019 §2 — nodes share one ProcessingContext, no per-input slots), so
// there is nothing richer than "can something feed this node, or not" to encode in the shape.
//
// - "Hat" piece (Source): no top notch, one bottom tab. Mirrors Scratch's hat blocks (nothing
//   connects above an event block either).
// - "Body" piece (Transform/Rule/Output): top notch + one bottom tab. Output deliberately keeps its
//   source tab — legacy array-order mode allows a mid-chain Output as a non-stopping side effect
//   (e.g. `aero.output.mes`), and `implicitEdges` (application.ts) needs a real handle to draw that
//   edge from; category-gating the SOURCE handle broke exactly this case in earlier manual testing
//   (react-flow error #008). Only the TARGET handle is category-gated (Source only), which is safe —
//   `implicitEdges` was updated to never draw an edge INTO a Source node in the first place.
// - "Body-branch" piece (`aero.flow.switch`): top notch + two bottom tabs (true/false), unchanged from
//   the plain-handle version, just shaped now.
import { Handle, Position, type NodeProps } from "@xyflow/react";
import { Button } from "./components";
import type { CatalogEntry, Category } from "./catalog";
import { jigsawPath, DEFAULT_LOBE_DEPTH } from "./jigsawPath";

// Exported (020 §4.2): SwitchBlockNode.tsx/switchCavityLayout.ts reuse the exact same type_id check and
// card dimensions so a nested cavity member lines up pixel-for-pixel with a free-floating card of the
// same shape — one source of truth instead of two copies drifting apart.
export const SWITCH_TYPE_ID = "aero.flow.switch";
export const CARD_WIDTH = 190;
export const CARD_HEIGHT = 140; // the "logical" box: notch sits at y=0, bottom edge at y=CARD_HEIGHT
// The bottom tab protrudes DEFAULT_LOBE_DEPTH past CARD_HEIGHT — the actual DOM element/SVG viewBox
// must be that much taller, or the tab gets clipped by SVG's default overflow:hidden (found via the
// live smoke test: the tab was invisible until this was added — matches styles.css's `.flow-node-card`
// height, which must stay in sync with this constant).
export const CARD_VISUAL_HEIGHT = CARD_HEIGHT + DEFAULT_LOBE_DEPTH;

const CATEGORY_CLASS: Record<Category, string> = {
  Source: "cat-source",
  Transform: "cat-transform",
  Rule: "cat-rule",
  Output: "cat-output",
};

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
  const category = d.entry?.category;
  const isSwitch = d.typeId === SWITCH_TYPE_ID;
  const isSource = category === "Source";
  const path = jigsawPath({
    width: CARD_WIDTH,
    height: CARD_HEIGHT,
    topNotch: !isSource,
    bottomTabs: isSwitch ? 2 : 1,
  });
  const catClass = category ? CATEGORY_CLASS[category] : "";

  return (
    <div
      className={`flow-node-card ${catClass}${d.isSelected ? " sel" : ""}`}
      onClick={() => d.onSelect(d.index)}
    >
      <svg className="jigsaw-shape" viewBox={`0 0 ${CARD_WIDTH} ${CARD_VISUAL_HEIGHT}`} preserveAspectRatio="none">
        <path d={path} />
      </svg>
      {!isSource && <Handle type="target" position={Position.Top} />}
      <div className="node-content">
        <span className="badge">{category ?? "?"}</span>
        <div className="node-label">{d.entry?.label ?? d.typeId}</div>
        <div className="node-id">{d.typeId}</div>
        {/* stopPropagation at the row so a button click doesn't also fire the card's onSelect */}
        <div className="row-actions" onClick={(e) => e.stopPropagation()}>
          <Button ariaLabel="Move up" onClick={() => d.onMove(d.index, -1)}>↑</Button>
          <Button ariaLabel="Move down" onClick={() => d.onMove(d.index, 1)}>↓</Button>
          <Button ariaLabel="Remove node" variant="danger" onClick={() => d.onRemove(d.index)}>✕</Button>
        </div>
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
