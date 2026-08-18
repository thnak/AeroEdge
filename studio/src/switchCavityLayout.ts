// Shared geometry for the switch C-block's two nested "true"/"false" cavities (020 §4.2). Pure
// functions of member counts only — no React, no react-flow types — used by BOTH SwitchBlockNode.tsx
// (drawing the cavity boxes) and FlowDesigner.tsx (positioning nested member cards + hit-testing a
// drag-and-drop as landing inside a cavity). One source of truth so the visual boxes and the actual
// drop zones can never drift apart, the way jigsawPath's DEFAULT_LOBE_DEPTH/CARD_VISUAL_HEIGHT pairing
// already guards against for the plain card shapes.
//
// STACKED vertically, one full-width cavity below the other — NOT side by side. Matches both the
// design doc's own §4.2 ASCII diagram (header, then "├─ true ─┤" body, then "├─ false ─┤" body) and
// Blockly's real C-block convention (docs.blockly.com/.../inline-vs-external: a body cavity ("do")
// sits BELOW its header row, full block width — this is what every real Blockly C-block looks like,
// loops included). An earlier draft of this file put them side by side; corrected after review.
import { CARD_WIDTH, CARD_VISUAL_HEIGHT } from "./FlowCanvasNode";

export type BranchLabel = "true" | "false";

export const CAVITY_WIDTH = CARD_WIDTH + 20; // roomy enough for one nested card + a little breathing room
export const CAVITY_GAP = 14; // vertical gap between the "true" cavity and the "false" cavity below it
export const CAVITY_PADDING = 12; // outer margin of the whole switch-block card
export const HEADER_HEIGHT = 150; // badge/label/type_id/expr summary + row-actions, above both cavities
export const MEMBER_GAP = 6; // small but nonzero gap between chained members — "snapped", not overlapping
export const CAVITY_MIN_HEIGHT = 60; // empty-cavity placeholder height — always a visible drop target

// Height of one cavity given how many nodes are currently chained inside it.
export function cavityHeight(memberCount: number): number {
  if (memberCount <= 0) return CAVITY_MIN_HEIGHT;
  // 22 clears the top label banner (matches cavityMemberPosition's own offset), +8 bottom margin.
  return 30 + memberCount * CARD_VISUAL_HEIGHT + (memberCount - 1) * MEMBER_GAP;
}

// Overall switch-block card size — one column wide, tall enough for the header plus BOTH cavities
// stacked (true above false).
export function switchBlockSize(trueCount: number, falseCount: number): { width: number; height: number } {
  return {
    width: CAVITY_PADDING * 2 + CAVITY_WIDTH,
    height: HEADER_HEIGHT + cavityHeight(trueCount) + CAVITY_GAP + cavityHeight(falseCount) + CAVITY_PADDING,
  };
}

// One cavity's box, relative to the switch-block node's own top-left corner. Used both to render the
// dashed drop-zone rectangle and, added to the switch's own absolute canvas position, to hit-test
// whether a drag-and-drop landed inside it (FlowDesigner.tsx's onNodeDragStop). "false"'s y-position
// depends on how tall "true" currently is — both counts are needed, not just the target label's own.
export function cavityRect(
  label: BranchLabel,
  trueCount: number,
  falseCount: number,
): { x: number; y: number; width: number; height: number } {
  const y = label === "true" ? HEADER_HEIGHT : HEADER_HEIGHT + cavityHeight(trueCount) + CAVITY_GAP;
  return { x: CAVITY_PADDING, y, width: CAVITY_WIDTH, height: cavityHeight(label === "true" ? trueCount : falseCount) };
}

// Where the member at `index` (0 = the one directly off the switch) sits, relative to the switch
// node's own top-left — the SAME coordinate space `position` uses for a top-level react-flow node, so
// a member's rendered `position` is simply the switch's own absolute position plus this offset (no
// react-flow parentId/extent nesting: that mechanism CLAMPS a child inside its parent's bounds, which
// would make "drag a member back out to detach it" physically impossible — see FlowDesigner.tsx).
export function cavityMemberPosition(
  label: BranchLabel,
  index: number,
  trueCount: number,
  falseCount: number,
): { x: number; y: number } {
  const rect = cavityRect(label, trueCount, falseCount);
  return {
    x: rect.x + (CAVITY_WIDTH - CARD_WIDTH) / 2,
    // +22 clears the cavity's own top label banner (styles.css's .cavity-label) before the first member.
    y: rect.y + 22 + index * (CARD_VISUAL_HEIGHT + MEMBER_GAP),
  };
}
