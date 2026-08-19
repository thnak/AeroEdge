// Shared geometry for a loop's single "body" cavity (020 §8, Studio side) — the same "one source of
// truth for the drawn box and the actual drop zone" posture switchCavityLayout.ts established, sized
// down to ONE cavity instead of two. Pure functions of member count only — no React, no react-flow
// types — used by BOTH LoopBlockNode.tsx (drawing the cavity box) and FlowDesigner.tsx (positioning
// nested member cards, positioning the loop_back "closing" card immediately below the cavity, and
// hit-testing a drag-and-drop as landing inside it).
//
// Unlike switch, loop_back is deliberately NOT fused into the header's own SVG — it stays a fully
// normal, independently selectable/movable jigsaw card (FlowCanvasNode), just positioned flush against
// the cavity's bottom edge so the two pieces read as one continuous C-block bracket purely through
// layout, the same trick cavity members already use to sit "inside" a switch card they aren't a DOM
// child of.
import { CARD_WIDTH, CARD_VISUAL_HEIGHT } from "./FlowCanvasNode";

export const CAVITY_WIDTH = CARD_WIDTH + 20;
export const CAVITY_PADDING = 12;
export const HEADER_HEIGHT = 110; // badge/label/type_id/counter+range summary + row-actions
export const MEMBER_GAP = 6;
export const CAVITY_MIN_HEIGHT = 60;
export const CLOSE_GAP = 10; // vertical gap between the cavity's bottom edge and loop_back's own card

export function cavityHeight(memberCount: number): number {
  if (memberCount <= 0) return CAVITY_MIN_HEIGHT;
  return 30 + memberCount * CARD_VISUAL_HEIGHT + (memberCount - 1) * MEMBER_GAP;
}

// The loop_start header+cavity card's own size — loop_back is a separate node, not part of this box.
export function loopBlockSize(memberCount: number): { width: number; height: number } {
  return {
    width: CAVITY_PADDING * 2 + CAVITY_WIDTH,
    height: HEADER_HEIGHT + cavityHeight(memberCount) + CAVITY_PADDING,
  };
}

export function cavityRect(memberCount: number): { x: number; y: number; width: number; height: number } {
  return { x: CAVITY_PADDING, y: HEADER_HEIGHT, width: CAVITY_WIDTH, height: cavityHeight(memberCount) };
}

// Where member `index` (0 = directly off loop_start) sits, relative to loop_start's own top-left —
// the same coordinate space a top-level react-flow node's `position` uses (no parentId/extent nesting,
// same reasoning switchCavityLayout.ts's cavityMemberPosition already documents).
export function cavityMemberPosition(index: number): { x: number; y: number } {
  const rect = cavityRect(index + 1); // ensure the rect is at least tall enough to hold this index
  return {
    x: rect.x + (CAVITY_WIDTH - CARD_WIDTH) / 2,
    y: rect.y + 22 + index * (CARD_VISUAL_HEIGHT + MEMBER_GAP),
  };
}

// Where loop_back's own card sits, relative to loop_start's top-left — flush below the cavity
// (regardless of how many members it currently holds), horizontally centered to match member cards.
export function loopBackPosition(memberCount: number): { x: number; y: number } {
  const rect = cavityRect(memberCount);
  return { x: rect.x + (CAVITY_WIDTH - CARD_WIDTH) / 2, y: rect.y + rect.height + CLOSE_GAP };
}
