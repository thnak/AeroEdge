// Parametric SVG path for a jigsaw/puzzle-piece node card (019 §4 jigsaw slice). Renders a rounded
// rectangle with an optional scalloped notch cut into the top-center edge and 1-2 scalloped tabs
// bumped out of the bottom edge — the "hat" (Source: no notch) vs "body" (Transform/Rule/Output:
// notch + 1 tab) vs "body-branch" (aero.flow.switch: notch + 2 tabs) distinction FlowCanvasNode
// renders per NodeCategory. Only two real shapes exist because the architecture has no per-node
// port/type system (019 §2: nodes share one ProcessingContext, no per-input slots) — there is nothing
// richer to encode than "can something feed this node, or not."
//
// No SVG-shape library exists for this (researched: react-jigsaw-puzzle/react-puzzle are photo-
// slicing puzzle GAMES, not node shapes; Blockly is the real jigsaw-block editor but models programs
// as statement stacks with no native fan-out/merge, a worse fit than building this ourselves — see
// 019-Flow-Graph-Model-and-Studio-Canvas-API.md §4). This is deliberately one small parametric
// function, not a from-scratch shape system: same two bezier-scallop primitives reused for every
// notch/tab.

// Default tab/notch bump depth, in the same units as `height`/`width`. Exported so callers can size
// their SVG viewBox/element tall enough to hold the bottom tab — it protrudes `lobeDepth` PAST
// `height`, and SVG clips anything outside its own box by default (no `overflow: visible` needed if
// the caller just sizes the box to `height + lobeDepth` up front, which is simpler and keeps react-
// flow's own notion of the node's bounding box matching what's actually drawn).
export const DEFAULT_LOBE_DEPTH = 9;

export interface JigsawOptions {
  width: number;
  height: number;
  cornerRadius?: number;
  topNotch: boolean;
  bottomTabs: number; // 0, 1 (centered), or 2 (at 30%/70%, matching the switch branch handles)
  lobeWidth?: number;
  lobeDepth?: number;
}

// One rounded scallop between two points on the same edge-line — entering at (x1, y), leaving at
// (x2, y) — bumping perpendicular to the edge by `depth` (SVG y-axis: positive = downward). Written
// purely in terms of the two endpoints (not "forward" vs "backward") so the same helper draws a
// notch on the top edge (left-to-right traversal) and a tab on the bottom edge (right-to-left
// traversal, x2 < x1) without a separate mirrored implementation.
function scallop(x1: number, x2: number, y: number, depth: number): string {
  const cx = (x1 + x2) / 2;
  const apexY = y + depth;
  const span = Math.abs(x2 - x1);
  const k = span * 0.22;
  const dir = x2 > x1 ? 1 : -1;
  return (
    `C ${x1 + dir * k} ${apexY}, ${cx - dir * span * 0.18} ${apexY}, ${cx} ${apexY} ` +
    `C ${cx + dir * span * 0.18} ${apexY}, ${x2 - dir * k} ${apexY}, ${x2} ${y} `
  );
}

export function jigsawPath(opts: JigsawOptions): string {
  const { width: W, height: H, topNotch, bottomTabs } = opts;
  const r = opts.cornerRadius ?? 8;
  const lw = opts.lobeWidth ?? 40;
  const ld = opts.lobeDepth ?? DEFAULT_LOBE_DEPTH;

  const tabCenters =
    bottomTabs === 2 ? [W * 0.3, W * 0.7] : bottomTabs === 1 ? [W * 0.5] : [];

  let d = `M ${r} 0 `;

  // Top edge, left -> right, with an optional inward notch centered at W/2.
  if (topNotch) {
    d += `L ${W / 2 - lw / 2} 0 `;
    d += scallop(W / 2 - lw / 2, W / 2 + lw / 2, 0, ld);
  }
  d += `L ${W - r} 0 `;
  d += `Q ${W} 0, ${W} ${r} `; // top-right corner
  d += `L ${W} ${H - r} `;
  d += `Q ${W} ${H}, ${W - r} ${H} `; // bottom-right corner

  // Bottom edge, right -> left, with an outward tab at each center.
  for (const cx of [...tabCenters].reverse()) {
    d += `L ${cx + lw / 2} ${H} `;
    d += scallop(cx + lw / 2, cx - lw / 2, H, ld);
  }
  d += `L ${r} ${H} `;

  d += `Q ${0} ${H}, ${0} ${H - r} `; // bottom-left corner
  d += `L ${0} ${r} `;
  d += `Q ${0} ${0}, ${r} ${0} `; // top-left corner
  d += `Z`;

  return d;
}
