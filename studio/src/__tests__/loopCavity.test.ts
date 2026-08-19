// Pure-function tests for the loop C-block cavity logic (020 §8, Studio side). Same "plain data
// function, no DOM/react-flow simulation" posture cavity.test.ts already established for switch.
import { describe, it, expect } from "vitest";
import type { FlowModel, GraphEdge } from "../application";
import { computeLoops, loopAbsorbedEdgeIds, attachToLoopCavity, detachFromLoopCavity } from "../loopCavity";

function edge(from: string, to: string, fromPort?: string): GraphEdge {
  return { id: `e:${from}:${fromPort ?? ""}:${to}`, from, to, fromPort };
}

function baseModel(edges: GraphEdge[]): FlowModel {
  return {
    name: "loop-demo",
    version: "1.0.0",
    actorKind: "edge",
    actorKey: 1,
    nodes: [
      { id: "src", type_id: "aero.source.decode" },
      { id: "ls", type_id: "aero.flow.loop_start", config: { counter_tag: "i", start_expr: "0" } },
      { id: "acc", type_id: "aero.transform.set", config: { tag: "sum", expr: "tag(\"sum\") + tag(\"i\")" } },
      { id: "lb", type_id: "aero.flow.loop_back", config: { counter_tag: "i", step_expr: "1", end_expr: "9" } },
      { id: "out", type_id: "aero.output.sum" },
    ],
    edges,
  };
}

describe("computeLoops", () => {
  it("recognizes an empty-body loop from a self-referential loop_back edge", () => {
    const edges = [edge("src", "ls"), edge("ls", "lb"), edge("lb", "lb", "loop_back"), edge("lb", "out")];
    const { pairs, loopBackOf, membership } = computeLoops(edges);
    expect(pairs.get("ls")).toEqual({ loopBackId: "lb", members: [] });
    expect(loopBackOf.get("lb")).toBe("ls");
    expect(membership.size).toBe(0);
  });

  it("walks a single-member body to find a closed pair", () => {
    const edges = [
      edge("src", "ls"), edge("ls", "acc"), edge("acc", "lb"), edge("lb", "acc", "loop_back"), edge("lb", "out"),
    ];
    const { pairs, membership } = computeLoops(edges);
    expect(pairs.get("ls")).toEqual({ loopBackId: "lb", members: ["acc"] });
    expect(membership.get("acc")).toEqual({ loopStartId: "ls", loopBackId: "lb", index: 0 });
  });

  it("walks a multi-member chain of single-in/single-out unlabeled edges", () => {
    const edges = [
      edge("src", "ls"), edge("ls", "a"), edge("a", "b"), edge("b", "lb"),
      edge("lb", "a", "loop_back"), edge("lb", "out"),
    ];
    const { pairs, membership } = computeLoops(edges);
    expect(pairs.get("ls")).toEqual({ loopBackId: "lb", members: ["a", "b"] });
    expect(membership.get("b")).toEqual({ loopStartId: "ls", loopBackId: "lb", index: 1 });
  });

  it("isn't closed yet when no loop_back edge exists", () => {
    const edges = [edge("src", "ls"), edge("ls", "acc"), edge("acc", "lb"), edge("lb", "out")];
    const { pairs, membership } = computeLoops(edges);
    expect(pairs.size).toBe(0);
    expect(membership.size).toBe(0);
  });

  it("isn't closed when the forward chain never reaches the loop_back node (fan-out)", () => {
    const edges = [
      edge("src", "ls"), edge("ls", "acc"), edge("acc", "lb"), edge("acc", "other"),
      edge("lb", "acc", "loop_back"),
    ];
    const { pairs } = computeLoops(edges);
    expect(pairs.size).toBe(0);
  });

  it("isn't closed when the chain head has more than one incoming unlabeled edge (ambiguous source)", () => {
    const edges = [
      edge("src", "ls"), edge("ls", "acc"), edge("other", "acc"), edge("acc", "lb"),
      edge("lb", "acc", "loop_back"),
    ];
    const { pairs } = computeLoops(edges);
    expect(pairs.size).toBe(0);
  });
});

describe("loopAbsorbedEdgeIds", () => {
  it("absorbs the head/internal/tail links and the back edge, but not the exit edge", () => {
    const edges = [
      edge("src", "ls"), edge("ls", "acc"), edge("acc", "lb"), edge("lb", "acc", "loop_back"), edge("lb", "out"),
    ];
    const { pairs } = computeLoops(edges);
    const absorbed = loopAbsorbedEdgeIds(edges, pairs);
    expect(absorbed.has(edge("ls", "acc").id)).toBe(true);
    expect(absorbed.has(edge("acc", "lb").id)).toBe(true);
    expect(absorbed.has(edge("lb", "acc", "loop_back").id)).toBe(true);
    expect(absorbed.has(edge("lb", "out").id)).toBe(false); // the block's real "next" tab
    expect(absorbed.has(edge("src", "ls").id)).toBe(false); // unrelated edge untouched
  });
});

describe("attachToLoopCavity / detachFromLoopCavity", () => {
  it("attaches a free node as the sole member of an empty cavity, retargeting the back edge", () => {
    const model = baseModel([edge("src", "ls"), edge("ls", "lb"), edge("lb", "lb", "loop_back"), edge("lb", "out")]);
    const next = attachToLoopCavity(model, "acc", "ls", "lb");
    const { pairs } = computeLoops(next.edges);
    expect(pairs.get("ls")).toEqual({ loopBackId: "lb", members: ["acc"] });
    expect(next.edges.some((e) => e.from === "lb" && e.fromPort === "loop_back" && e.to === "acc")).toBe(true);
    expect(next.edges.some((e) => e.from === "lb" && e.fromPort === "loop_back" && e.to === "lb")).toBe(false);
  });

  it("attaches as the new TAIL when the cavity already has a member, back edge untouched", () => {
    const model = baseModel([
      edge("src", "ls"), edge("ls", "acc"), edge("acc", "lb"), edge("lb", "acc", "loop_back"), edge("lb", "out"),
    ]);
    const next = attachToLoopCavity(model, "second", "ls", "lb");
    const { pairs } = computeLoops(next.edges);
    expect(pairs.get("ls")).toEqual({ loopBackId: "lb", members: ["acc", "second"] });
    expect(next.edges.some((e) => e.from === "lb" && e.fromPort === "loop_back" && e.to === "acc")).toBe(true);
  });

  it("detaching the sole member collapses back to the empty-cavity self-loop shape", () => {
    const model = baseModel([
      edge("src", "ls"), edge("ls", "acc"), edge("acc", "lb"), edge("lb", "acc", "loop_back"), edge("lb", "out"),
    ]);
    const next = detachFromLoopCavity(model, "acc");
    const { pairs, membership } = computeLoops(next.edges);
    expect(pairs.get("ls")).toEqual({ loopBackId: "lb", members: [] });
    expect(membership.has("acc")).toBe(false);
    expect(next.edges.some((e) => e.from === "ls" && e.to === "lb" && !e.fromPort)).toBe(true);
    expect(next.edges.some((e) => e.from === "lb" && e.fromPort === "loop_back" && e.to === "lb")).toBe(true);
  });

  it("detaching the HEAD of a 2-member chain promotes the next member and retargets the back edge", () => {
    const model = baseModel([
      edge("src", "ls"), edge("ls", "a"), edge("a", "b"), edge("b", "lb"),
      edge("lb", "a", "loop_back"), edge("lb", "out"),
    ]);
    const next = detachFromLoopCavity(model, "a");
    const { pairs } = computeLoops(next.edges);
    expect(pairs.get("ls")).toEqual({ loopBackId: "lb", members: ["b"] });
    expect(next.edges.some((e) => e.from === "lb" && e.fromPort === "loop_back" && e.to === "b")).toBe(true);
  });

  it("detaching a MIDDLE member bridges predecessor -> successor directly, back edge untouched", () => {
    const model = baseModel([
      edge("src", "ls"), edge("ls", "a"), edge("a", "b"), edge("b", "c"), edge("c", "lb"),
      edge("lb", "a", "loop_back"), edge("lb", "out"),
    ]);
    const next = detachFromLoopCavity(model, "b");
    const { pairs } = computeLoops(next.edges);
    expect(pairs.get("ls")).toEqual({ loopBackId: "lb", members: ["a", "c"] });
    expect(next.edges.some((e) => e.from === "lb" && e.fromPort === "loop_back" && e.to === "a")).toBe(true);
  });

  it("detaching a node that isn't nested is a no-op", () => {
    const model = baseModel([edge("src", "ls"), edge("ls", "lb"), edge("lb", "lb", "loop_back")]);
    const next = detachFromLoopCavity(model, "out");
    expect(next).toBe(model);
  });
});
