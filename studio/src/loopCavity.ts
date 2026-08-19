// 020 §8 (Studio side) — which nodes are nested inside a loop's body cavity (Blockly-style C-block),
// computed purely from the existing edges[]/from_port model, the same posture cavity.ts already
// established for switch's true/false cavities. The one real structural difference from switch: a
// loop's cavity isn't owned by a single node — it's bounded on BOTH ends by a matched
// aero.flow.loop_start/aero.flow.loop_back PAIR, discovered from the "loop_back"-labeled edge the user
// draws from loop_back back to the body's first member (or to loop_back itself, self-referential, for
// an empty body — the same shape SelfConnectingEdge.tsx already renders). Until that edge exists, both
// nodes render as plain jigsaw cards (FlowDesigner.tsx's graphMode-gated fallback, same as switch).
import type { FlowModel, GraphEdge } from "./application";
import { withEdge, withoutEdge } from "./application";

export interface LoopMembership {
  loopStartId: string;
  loopBackId: string;
  index: number; // position within the body chain — 0 = directly off loop_start
}

export interface LoopPair {
  loopBackId: string;
  members: string[]; // ordered chain between loop_start and loop_back, empty for a body-less loop
}

export interface Loops {
  // loop_start node id -> its paired loop_back id + ordered body chain
  pairs: Map<string, LoopPair>;
  // loop_back node id -> its paired loop_start id (reverse lookup — FlowDesigner uses this to skip
  // rendering loop_back as an independent top-level node once it's closing a real pair)
  loopBackOf: Map<string, string>;
  // node id -> where it's nested (absent if the node isn't a body member of any loop)
  membership: Map<string, LoopMembership>;
}

// For every "loop_back"-labeled edge in `edges`, attempt to resolve the full pair: walk FORWARD from
// the edge's target through single-out/single-in UNLABELED edges (the exact same "mini Previous/Next
// stack" walk cavity.ts's computeCavities uses for a branch chain) until landing back on the loop_back
// node itself — the walk's stopping condition, not a fan-out/merge/dead-end as it would be for a
// switch branch, since a loop body is bounded on both ends, never left open to rejoin elsewhere. An
// empty body is the degenerate case: the "loop_back"-labeled edge is a self-edge (source === target),
// so the walk finds the closing node immediately, with zero members. A walk that runs out (fan-out,
// merge, or a dead end before reaching loop_back) means the pair isn't closed yet — skipped entirely,
// same as cavity.ts skips membership.has(head.to) already-claimed heads defensively.
export function computeLoops(edges: GraphEdge[]): Loops {
  const outgoing = new Map<string, GraphEdge[]>();
  const incomingCount = new Map<string, number>();
  for (const e of edges) {
    if (!outgoing.has(e.from)) outgoing.set(e.from, []);
    outgoing.get(e.from)!.push(e);
    incomingCount.set(e.to, (incomingCount.get(e.to) ?? 0) + 1);
  }

  const pairs: Loops["pairs"] = new Map();
  const loopBackOf: Loops["loopBackOf"] = new Map();
  const membership: Loops["membership"] = new Map();

  const backEdges = edges.filter((e) => e.fromPort === "loop_back");
  for (const back of backEdges) {
    const loopBackId = back.from;
    if (loopBackOf.has(loopBackId)) continue; // a second loop_back edge from the same node — ignore

    const members: string[] = [];
    let current: string | undefined = back.to;
    let closed = current === loopBackId; // empty-body self-loop: found immediately
    if (!closed) {
      const seen = new Set<string>();
      while (current !== undefined && current !== loopBackId && !seen.has(current)) {
        seen.add(current);
        members.push(current);
        const outs = (outgoing.get(current) ?? []).filter((e) => !e.fromPort);
        if (outs.length !== 1) { current = undefined; break; } // fan-out or dead end
        const next: string = outs[0].to;
        if (next !== loopBackId && (incomingCount.get(next) ?? 0) !== 1) { current = undefined; break; }
        current = next;
      }
      closed = current === loopBackId;
    }
    if (!closed) continue;

    // The loop_start id is whoever reaches the chain's head (members[0], or loopBackId itself when the
    // body is empty) via a single unlabeled incoming edge — structurally implied, same permissive
    // posture cavity.ts already takes toward the switch's own type_id (never checked directly either).
    const headId = members.length > 0 ? members[0] : loopBackId;
    const headIncoming = edges.filter((e) => e.to === headId && !e.fromPort);
    if (headIncoming.length !== 1) continue; // ambiguous entry point — not a valid single-source loop

    const loopStartId = headIncoming[0].from;
    pairs.set(loopStartId, { loopBackId, members });
    loopBackOf.set(loopBackId, loopStartId);
    members.forEach((id, index) => membership.set(id, { loopStartId, loopBackId, index }));
  }

  return { pairs, loopBackOf, membership };
}

// Edge ids "absorbed" into the loop's physical C-block rendering — the head edge (loop_start into the
// body, or straight into loop_back for an empty body), every internal chain link, the tail edge into
// loop_back, and the "loop_back"-labeled back edge itself (implied by the closed C-shape, never drawn
// as a floating/arced SelfConnectingEdge once the pair is recognized). The loop_back node's OTHER
// outgoing edge — its unconditional exit, continuing the flow after the loop — is deliberately NOT
// absorbed; that's the block's real "next" tab.
export function loopAbsorbedEdgeIds(edges: GraphEdge[], pairs: Loops["pairs"]): Set<string> {
  const absorbed = new Set<string>();
  for (const [loopStartId, pair] of pairs) {
    const headId = pair.members.length > 0 ? pair.members[0] : pair.loopBackId;
    const head = edges.find((e) => e.from === loopStartId && e.to === headId && !e.fromPort);
    if (head) absorbed.add(head.id);
    for (let i = 0; i + 1 < pair.members.length; i++) {
      const link = edges.find((e) => e.from === pair.members[i] && e.to === pair.members[i + 1] && !e.fromPort);
      if (link) absorbed.add(link.id);
    }
    if (pair.members.length > 0) {
      const tail = pair.members[pair.members.length - 1];
      const tailToBack = edges.find((e) => e.from === tail && e.to === pair.loopBackId && !e.fromPort);
      if (tailToBack) absorbed.add(tailToBack.id);
    }
    const back = edges.find((e) => e.from === pair.loopBackId && e.fromPort === "loop_back");
    if (back) absorbed.add(back.id);
  }
  return absorbed;
}

// Detach `nodeIdToDetach` from whatever loop cavity it's currently nested in (a no-op if it isn't
// nested). Unlike cavity.ts's detachFromCavity, a loop cavity is ALWAYS closed on both ends, so the
// successor is never "possibly absent" — it's the next member, or loop_back itself. If the detached
// member was the chain's head (index 0), the "loop_back"-labeled back edge is retargeted to the new
// head (the promoted next member, or loop_back itself if the cavity is now empty) so the pair stays
// correctly wired without the user ever having to manually redraw it.
export function detachFromLoopCavity(model: FlowModel, nodeIdToDetach: string): FlowModel {
  const { pairs, membership } = computeLoops(model.edges);
  const mem = membership.get(nodeIdToDetach);
  if (!mem) return model;
  const pair = pairs.get(mem.loopStartId);
  if (!pair) return model;

  const predecessorId = mem.index === 0 ? mem.loopStartId : pair.members[mem.index - 1];
  const successorId = pair.members[mem.index + 1] ?? mem.loopBackId;

  let next = model;
  const incoming = model.edges.find((e) => e.to === nodeIdToDetach && e.from === predecessorId && !e.fromPort);
  if (incoming) next = withoutEdge(next, incoming.id);
  const outgoing = model.edges.find((e) => e.from === nodeIdToDetach && e.to === successorId && !e.fromPort);
  if (outgoing) next = withoutEdge(next, outgoing.id);

  next = withEdge(next, { id: `e:${predecessorId}::${successorId}`, from: predecessorId, to: successorId });

  if (mem.index === 0) {
    const back = next.edges.find((e) => e.from === mem.loopBackId && e.fromPort === "loop_back");
    if (back) {
      next = withoutEdge(next, back.id);
      next = withEdge(next, {
        id: `e:${mem.loopBackId}:loop_back:${successorId}`,
        from: mem.loopBackId, fromPort: "loop_back", to: successorId,
      });
    }
  }
  return next;
}

// Attach `nodeIdToAttach` as the new TAIL of the loop body between `loopStartId`/`loopBackId` — first
// detaching it from wherever it was nested before, same "detach-then-attach just works" posture as
// cavity.ts's attachToCavity. Requires the pair to already exist (the user has drawn at least the
// initial loop_start -> ... -> loop_back chain and the closing "loop_back"-labeled edge) — dragging a
// free node onto an as-yet-unpaired loop_start/loop_back does nothing (FlowDesigner.tsx only offers
// this as a drop target once computeLoops recognizes the pair).
export function attachToLoopCavity(
  model: FlowModel,
  nodeIdToAttach: string,
  loopStartId: string,
  loopBackId: string,
): FlowModel {
  const detached = detachFromLoopCavity(model, nodeIdToAttach);
  const { pairs } = computeLoops(detached.edges);
  const members = pairs.get(loopStartId)?.members ?? [];
  const tail = members[members.length - 1];

  let next = detached;
  if (tail !== undefined) {
    const oldLink = next.edges.find((e) => e.from === tail && e.to === loopBackId && !e.fromPort);
    if (oldLink) next = withoutEdge(next, oldLink.id);
    next = withEdge(next, { id: `e:${tail}::${nodeIdToAttach}`, from: tail, to: nodeIdToAttach });
    next = withEdge(next, { id: `e:${nodeIdToAttach}::${loopBackId}`, from: nodeIdToAttach, to: loopBackId });
    return next;
  }

  // Empty cavity: nodeIdToAttach becomes the sole member, spliced between loop_start and loop_back.
  const headLink = next.edges.find((e) => e.from === loopStartId && e.to === loopBackId && !e.fromPort);
  if (headLink) next = withoutEdge(next, headLink.id);
  next = withEdge(next, { id: `e:${loopStartId}::${nodeIdToAttach}`, from: loopStartId, to: nodeIdToAttach });
  next = withEdge(next, { id: `e:${nodeIdToAttach}::${loopBackId}`, from: nodeIdToAttach, to: loopBackId });

  const back = next.edges.find((e) => e.from === loopBackId && e.fromPort === "loop_back");
  if (back) {
    next = withoutEdge(next, back.id);
    next = withEdge(next, {
      id: `e:${loopBackId}:loop_back:${nodeIdToAttach}`,
      from: loopBackId, fromPort: "loop_back", to: nodeIdToAttach,
    });
  }
  return next;
}
