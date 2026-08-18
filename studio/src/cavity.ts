// 020 §4.2 — which nodes are nested inside a switch's "true"/"false" cavities (Blockly-style C-block),
// computed purely from the existing edges[]/from_port model. No wire-schema change: a nested cavity
// member is exactly a `from_port`-labeled edge (or a chain of unlabeled edges hanging off one), and a
// "stub"/rejoin (the ANSI off-page-connector case — a branch's last member reaching a node shared with
// something else) is just a surviving, ordinary edge whose source happens to be nested. Everything here
// is pure/framework-free so it's unit-testable without touching react-flow or the DOM at all.
import type { FlowModel, GraphEdge } from "./application";
import { withEdge, withoutEdge } from "./application";

export type BranchLabel = "true" | "false";

export interface CavityMembership {
  switchId: string;
  label: BranchLabel;
  index: number; // position within the branch's chain — 0 = directly off the switch
}

export interface Cavities {
  // switchId -> label -> ordered chain of member node ids
  chains: Map<string, Map<BranchLabel, string[]>>;
  // node id -> where it's nested (absent if the node isn't nested in any cavity)
  membership: Map<string, CavityMembership>;
}

// For every switch node reachable in `edges`, the ordered member chain of each of its two branches. A
// branch's chain starts at the direct `from_port`-labeled edge's target and walks forward through
// single-outgoing/single-incoming UNLABELED edges (§4.2's "mini Previous/Next stack") — it stops at a
// fan-out (more than one outgoing edge — a further branch inside the cavity, out of scope for nesting),
// a merge point (the next node has more than one incoming edge, i.e. shared with something else — this
// becomes the "stub" case, not absorbed into the chain), or simply running out of outgoing edges. A
// node nests in at most one cavity; if it were somehow reachable from two branches the first-found
// chain wins (defensive — order_flow_graph's "at most one branch source" rule already prevents this).
export function computeCavities(edges: GraphEdge[]): Cavities {
  const outgoing = new Map<string, GraphEdge[]>();
  const incomingCount = new Map<string, number>();
  for (const e of edges) {
    if (!outgoing.has(e.from)) outgoing.set(e.from, []);
    outgoing.get(e.from)!.push(e);
    incomingCount.set(e.to, (incomingCount.get(e.to) ?? 0) + 1);
  }

  const chains: Cavities["chains"] = new Map();
  const membership: Cavities["membership"] = new Map();

  const branchHeads = edges.filter(
    (e): e is GraphEdge & { fromPort: BranchLabel } => e.fromPort === "true" || e.fromPort === "false",
  );
  for (const head of branchHeads) {
    const switchId = head.from;
    const label = head.fromPort;
    if (membership.has(head.to)) continue; // already claimed by another cavity — skip (defensive)

    const chain: string[] = [];
    let current: string | undefined = head.to;
    while (current !== undefined && !membership.has(current)) {
      chain.push(current);
      membership.set(current, { switchId, label, index: chain.length - 1 });
      const outs: GraphEdge[] = outgoing.get(current) ?? [];
      const unlabeledOuts: GraphEdge[] = outs.filter((e: GraphEdge) => !e.fromPort);
      if (unlabeledOuts.length !== 1) break; // fan-out or dead end — chain stops here
      const next: string = unlabeledOuts[0].to;
      if ((incomingCount.get(next) ?? 0) !== 1) break; // next is a merge point — leave it un-nested
      current = next;
    }

    if (!chains.has(switchId)) chains.set(switchId, new Map());
    chains.get(switchId)!.set(label, chain);
  }

  return { chains, membership };
}

// Edge ids "absorbed" into cavity nesting — the direct branch-labeled edge into a cavity's first
// member, plus every internal link between consecutive chain members — rendered as physical snapping,
// never a drawn line (§4.2). Everything else, INCLUDING a chain's tail reaching outside the cavity (the
// stub/rejoin case), still renders as a normal edge.
export function absorbedEdgeIds(edges: GraphEdge[], chains: Cavities["chains"]): Set<string> {
  const absorbed = new Set<string>();
  for (const [switchId, byLabel] of chains) {
    for (const [label, chain] of byLabel) {
      if (chain.length === 0) continue;
      const head = edges.find((e) => e.from === switchId && e.fromPort === label && e.to === chain[0]);
      if (head) absorbed.add(head.id);
      for (let i = 0; i + 1 < chain.length; i++) {
        const link = edges.find((e) => e.from === chain[i] && e.to === chain[i + 1] && !e.fromPort);
        if (link) absorbed.add(link.id);
      }
    }
  }
  return absorbed;
}

// Detach `nodeIdToDetach` from whatever cavity it's currently nested in (a no-op if it isn't nested).
// Bridges the gap left behind — if it had both a predecessor (the switch itself, for index 0, or the
// previous chain member) and a successor, a new edge connects them directly so the REST of the chain
// stays intact (the same relinking a doubly-linked-list removal needs). Precondition: `model` is
// already in graph mode (`model.edges` is the authoritative edge list) — cavities only exist once a
// flow has opted into graph mode in the first place (FlowDesigner.tsx only calls this then).
export function detachFromCavity(model: FlowModel, nodeIdToDetach: string): FlowModel {
  const { chains, membership } = computeCavities(model.edges);
  const mem = membership.get(nodeIdToDetach);
  if (!mem) return model;

  const chain = chains.get(mem.switchId)?.get(mem.label) ?? [];
  const predecessorId = mem.index === 0 ? mem.switchId : chain[mem.index - 1];
  const incoming = model.edges.find(
    (e) =>
      e.to === nodeIdToDetach &&
      e.from === predecessorId &&
      (mem.index === 0 ? e.fromPort === mem.label : !e.fromPort),
  );
  let next = incoming ? withoutEdge(model, incoming.id) : model;

  const successorId = chain[mem.index + 1];
  if (successorId !== undefined) {
    const link = model.edges.find((e) => e.from === nodeIdToDetach && e.to === successorId && !e.fromPort);
    if (link) next = withoutEdge(next, link.id);
    next = withEdge(
      next,
      mem.index === 0
        ? { id: `e:${mem.switchId}:${mem.label}:${successorId}`, from: mem.switchId, fromPort: mem.label, to: successorId }
        : { id: `e:${predecessorId}::${successorId}`, from: predecessorId, to: successorId },
    );
  }
  return next;
}

// Attach `nodeIdToAttach` as the new TAIL of `switchId`'s `label` cavity — first detaching it from
// wherever it was nested before (moving between cavities, or re-inserting elsewhere in the same one,
// both "just work" as detach-then-attach). Same graph-mode precondition as detachFromCavity.
export function attachToCavity(
  model: FlowModel,
  nodeIdToAttach: string,
  switchId: string,
  label: BranchLabel,
): FlowModel {
  const detached = detachFromCavity(model, nodeIdToAttach);
  const { chains } = computeCavities(detached.edges);
  const chain = chains.get(switchId)?.get(label) ?? [];
  const tail = chain[chain.length - 1];
  return withEdge(
    detached,
    tail !== undefined
      ? { id: `e:${tail}::${nodeIdToAttach}`, from: tail, to: nodeIdToAttach }
      : { id: `e:${switchId}:${label}:${nodeIdToAttach}`, from: switchId, fromPort: label, to: nodeIdToAttach },
  );
}
