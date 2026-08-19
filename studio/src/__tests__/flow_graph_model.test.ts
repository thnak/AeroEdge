// Pure-function tests for the graph-editing helpers (019 §4 slice). Deliberately NOT DOM/react-flow
// simulation — flow_designer_canvas.test.tsx already documents why: @xyflow/react nodes stay
// `visibility: hidden` in jsdom until a real ResizeObserver measures them, which jsdom never provides,
// making drag/connect gestures unreliable to simulate. These helpers are plain data functions
// (application.ts), so they're testable directly — and mirror tests/core/flow_graph.cpp's PART A
// cases (cycle, dangling edge, conflicting branch labels, two-branch-sources) at the TS layer, same
// "instant feedback, runtime remains authority" posture as catalog.ts's validateConfig.
import { describe, it, expect } from "vitest";
import {
  implicitEdges,
  withEdge,
  withoutEdge,
  removeNodeAndEdges,
  toApplication,
  validateGraph,
  type FlowModel,
  type Application,
} from "../application";

function baseModel(): FlowModel {
  return {
    name: "graph-demo",
    version: "1.0.0",
    actorKind: "edge",
    actorKey: 1,
    nodes: [
      { id: "src", type_id: "aero.source.decode" },
      { id: "sw", type_id: "aero.flow.switch", config: { expr: "raw > 100" } },
      { id: "hi", type_id: "aero.transform.scale", config: { factor: 10 } },
      { id: "lo", type_id: "aero.transform.scale", config: { factor: 1 } },
      { id: "out", type_id: "aero.output.sum" },
    ],
    edges: [],
  };
}

describe("implicitEdges", () => {
  it("chains nodes n[i] -> n[i+1] by id, empty for 0/1 nodes", () => {
    expect(implicitEdges([])).toEqual([]);
    expect(implicitEdges([{ id: "a", type_id: "x" }])).toEqual([]);
    const chain = implicitEdges([{ id: "a", type_id: "x" }, { id: "b", type_id: "y" }]);
    expect(chain).toEqual([{ id: "implicit-0", from: "a", to: "b" }]);
  });

  // 019 §4 jigsaw slice: a Source card has no target handle (nothing can plug into it), so drawing
  // that implicit edge would hit the same react-flow error the earlier Output-hiding bug did — skip
  // it instead. A Source node also genuinely ignores upstream context (it decodes ctx.payload, not
  // ctx.tags), so this is the semantically honest choice too, not just a rendering workaround.
  it("skips the implicit edge into a Source-category node, wherever it sits in the array", () => {
    const chain = implicitEdges([
      { id: "a", type_id: "aero.transform.scale" },
      { id: "b", type_id: "aero.source.decode" }, // Source, not at index 0 — unusual but not forbidden
      { id: "c", type_id: "aero.output.sum" },
    ]);
    expect(chain).toEqual([{ id: "implicit-1", from: "b", to: "c" }]); // a->b skipped, b->c kept
  });
});

describe("withEdge / withoutEdge — materialize-on-first-edit", () => {
  it("first connect seeds the implicit chain, keeping earlier connections", () => {
    const linear: FlowModel = {
      name: "n", version: "1", actorKind: "edge", actorKey: 1,
      nodes: [{ id: "a", type_id: "aero.source.decode" }, { id: "b", type_id: "aero.output.sum" }],
      edges: [],
    };
    const withBranch = withEdge(linear, { id: "manual", from: "a", to: "c", fromPort: "true" });
    // the original a->b chain survives materialization, plus the new manual edge.
    expect(withBranch.edges).toEqual([
      { id: "implicit-0", from: "a", to: "b" },
      { id: "manual", from: "a", to: "c", fromPort: "true" },
    ]);
  });

  it("deleting an implicit edge also materializes, then removes it", () => {
    const linear: FlowModel = {
      name: "n", version: "1", actorKind: "edge", actorKey: 1,
      nodes: [{ id: "a", type_id: "aero.source.decode" }, { id: "b", type_id: "aero.output.sum" }],
      edges: [],
    };
    const after = withoutEdge(linear, "implicit-0");
    expect(after.edges).toEqual([]);
  });
});

describe("removeNodeAndEdges", () => {
  it("in legacy mode, stays legacy (array removal only, no materialization)", () => {
    const linear: FlowModel = {
      name: "n", version: "1", actorKind: "edge", actorKey: 1,
      nodes: [
        { id: "a", type_id: "aero.source.decode" },
        { id: "b", type_id: "aero.transform.scale" },
        { id: "c", type_id: "aero.output.sum" },
      ],
      edges: [],
    };
    const after = removeNodeAndEdges(linear, 1);
    expect(after.nodes.map((n) => n.id)).toEqual(["a", "c"]);
    expect(after.edges).toEqual([]); // still legacy — toApplication keeps emitting the minimal shape
  });

  it("in graph mode, drops edges referencing the removed node", () => {
    const m = withEdge(baseModel(), { id: "e1", from: "sw", to: "hi", fromPort: "true" });
    const after = removeNodeAndEdges(m, m.nodes.findIndex((n) => n.id === "hi"));
    expect(after.edges.some((e) => e.from === "hi" || e.to === "hi")).toBe(false);
  });
});

describe("toApplication — legacy vs graph mode shape", () => {
  it("omits id/edges entirely when edges is empty (legacy shape unchanged)", () => {
    const linear: FlowModel = {
      name: "n", version: "1", actorKind: "edge", actorKey: 1,
      nodes: [{ id: "a", type_id: "aero.source.decode" }, { id: "b", type_id: "aero.output.sum" }],
      edges: [],
    };
    const app = toApplication(linear);
    expect(app.flow).toEqual([{ type_id: "aero.source.decode" }, { type_id: "aero.output.sum" }]);
    expect(app.edges).toBeUndefined();
  });

  it("emits explicit id + edges (with branch labels) once in graph mode", () => {
    const m: FlowModel = {
      ...baseModel(),
      edges: [
        { id: "e1", from: "src", to: "sw" },
        { id: "e2", from: "sw", fromPort: "true", to: "hi" },
        { id: "e3", from: "sw", fromPort: "false", to: "lo" },
        { id: "e4", from: "hi", to: "out" },
        { id: "e5", from: "lo", to: "out" },
      ],
    };
    const app = toApplication(m);
    expect(app.flow.every((n) => typeof n.id === "string")).toBe(true);
    expect(app.edges).toEqual([
      { from: "src", to: "sw" },
      { from: "sw", from_port: "true", to: "hi" },
      { from: "sw", from_port: "false", to: "lo" },
      { from: "hi", to: "out" },
      { from: "lo", to: "out" },
    ]);
  });
});

describe("validateGraph — instant client feedback mirroring flow_compiler.hpp's order_flow_graph", () => {
  it("no-ops when the application has no edges (legacy mode)", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [{ type_id: "aero.source.decode" }, { type_id: "aero.output.sum" }],
    };
    expect(validateGraph(app)).toEqual([]);
  });

  it("rejects a cycle", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [{ id: "a", type_id: "x" }, { id: "b", type_id: "y" }],
      edges: [{ from: "a", to: "b" }, { from: "b", to: "a" }],
    };
    expect(validateGraph(app).some((e) => e.includes("cycle"))).toBe(true);
  });

  it("rejects a dangling edge endpoint", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [{ id: "a", type_id: "x" }],
      edges: [{ from: "a", to: "nope" }],
    };
    expect(validateGraph(app).some((e) => e.includes("unknown node"))).toBe(true);
  });

  it("rejects a node reached by conflicting branch labels", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [{ id: "src", type_id: "x" }, { id: "sw", type_id: "aero.flow.switch" }, { id: "x", type_id: "y" }],
      edges: [
        { from: "src", to: "sw" },
        { from: "sw", from_port: "true", to: "x" },
        { from: "src", to: "x" },
      ],
    };
    expect(validateGraph(app).some((e) => e.includes("conflicting branch labels"))).toBe(true);
  });

  it("rejects more than one branch-producing node", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [
        { id: "src", type_id: "x" }, { id: "sw1", type_id: "aero.flow.switch" },
        { id: "sw2", type_id: "aero.flow.switch" }, { id: "a", type_id: "y" }, { id: "b", type_id: "y" },
      ],
      edges: [
        { from: "src", to: "sw1" }, { from: "src", to: "sw2" },
        { from: "sw1", from_port: "true", to: "a" }, { from: "sw2", from_port: "true", to: "b" },
      ],
    };
    expect(validateGraph(app).some((e) => e.includes("more than one branch-producing node"))).toBe(true);
  });

  // The "Sum before Decode" root case (from the puzzle-piece design discussion): a flow whose root
  // isn't Source-category is rejected outright, mirroring order_flow_graph exactly.
  it("rejects a root node that isn't Source-category", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [{ id: "sum", type_id: "aero.output.sum" }, { id: "out", type_id: "aero.output.sum" }],
      edges: [{ from: "sum", to: "out" }],
    };
    expect(validateGraph(app).some((e) => e.includes("must be a Source node"))).toBe(true);
  });

  // NOTE on scope: a node several hops downstream of the root (e.g. Source -> Sum -> Decode ->
  // Output) is NOT independently checkable beyond the root check above. Once "exactly one root, and
  // it's Source" holds, topological order already guarantees the root runs before every other node —
  // so every node is provably reachable from it (a finite acyclic graph's single indegree-0 node is
  // the sole terminus of every predecessor chain). Whether a specific downstream node's actual FIELD
  // prerequisites have been populated by the time it runs is a different, richer question that would
  // need typed ports to answer — deliberately out of scope (019 §2). An earlier draft of this file
  // added a redundant ancestor-BFS to "catch" that case; it never actually fired once the root check
  // passed, so it was removed rather than kept as decoration.

  it("rejects more than one root, even when both roots are Source-category", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [
        { id: "src1", type_id: "aero.source.decode" }, { id: "src2", type_id: "aero.source.json" },
        { id: "out", type_id: "aero.output.sum" },
      ],
      edges: [{ from: "src1", to: "out" }], // src2 is disconnected — a second, unreachable root
    };
    expect(validateGraph(app).some((e) => e.includes("exactly one root"))).toBe(true);
  });

  it("accepts a valid fan-out + switch graph", () => {
    const app = toApplication({
      ...baseModel(),
      edges: [
        { id: "e1", from: "src", to: "sw" },
        { id: "e2", from: "sw", fromPort: "true", to: "hi" },
        { id: "e3", from: "sw", fromPort: "false", to: "lo" },
        { id: "e4", from: "hi", to: "out" },
        { id: "e5", from: "lo", to: "out" },
      ],
    });
    expect(validateGraph(app)).toEqual([]);
  });

  // 020 §8.7 (Studio-side mirror of order_flow_graph's own loop_back exclusion) — a loop_back edge is
  // a backward edge BY CONSTRUCTION and must never trip the generic cycle check, unlike a genuinely
  // unsupported hand-drawn cycle.
  it("accepts a valid loop graph — the loop_back edge is not a cycle", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [
        { id: "src", type_id: "aero.source.decode" },
        { id: "ls", type_id: "aero.flow.loop_start" },
        { id: "acc", type_id: "aero.transform.set" },
        { id: "lb", type_id: "aero.flow.loop_back" },
        { id: "out", type_id: "aero.output.sum" },
      ],
      edges: [
        { from: "src", to: "ls" },
        { from: "ls", to: "acc" },
        { from: "acc", to: "lb" },
        { from: "lb", from_port: "loop_back", to: "acc" },
        { from: "lb", to: "out" },
      ],
    };
    expect(validateGraph(app)).toEqual([]);
  });

  it("still rejects a genuine hand-drawn cycle that ISN'T a loop_back edge", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [{ id: "a", type_id: "x" }, { id: "b", type_id: "y" }],
      edges: [{ from: "a", to: "b" }, { from: "b", to: "a" }],
    };
    expect(validateGraph(app).some((e) => e.includes("cycle"))).toBe(true);
  });

  it("rejects more than one loop_back edge", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [
        { id: "src", type_id: "aero.source.decode" }, { id: "ls", type_id: "aero.flow.loop_start" },
        { id: "lb", type_id: "aero.flow.loop_back" }, { id: "out", type_id: "aero.output.sum" },
      ],
      edges: [
        { from: "src", to: "ls" }, { from: "ls", to: "lb" },
        { from: "lb", from_port: "loop_back", to: "lb" }, { from: "lb", from_port: "loop_back", to: "ls" },
        { from: "lb", to: "out" },
      ],
    };
    expect(validateGraph(app).some((e) => e.includes("more than one loop_back edge"))).toBe(true);
  });

  it("a switch and a loop can coexist — not 'more than one branch-producing node'", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [
        { id: "src", type_id: "aero.source.decode" }, { id: "ls", type_id: "aero.flow.loop_start" },
        { id: "lb", type_id: "aero.flow.loop_back" }, { id: "sw", type_id: "aero.flow.switch" },
        { id: "hi", type_id: "aero.transform.scale" }, { id: "lo", type_id: "aero.transform.scale" },
        { id: "out", type_id: "aero.output.sum" },
      ],
      edges: [
        // The loop is reached unconditionally from the root, closes on itself (empty body), then feeds
        // the switch — which is what's actually being asserted here: BOTH a loop_back edge and a
        // true/false pair coexisting shouldn't trip "more than one branch-producing node".
        { from: "src", to: "ls" }, { from: "ls", to: "lb" }, { from: "lb", from_port: "loop_back", to: "lb" },
        { from: "lb", to: "sw" },
        { from: "sw", from_port: "true", to: "hi" }, { from: "sw", from_port: "false", to: "lo" },
        { from: "hi", to: "out" }, { from: "lo", to: "out" },
      ],
    };
    expect(validateGraph(app)).toEqual([]);
  });

  it("rejects a loop_start reached only via a labeled branch edge (nested inside a branch)", () => {
    const app: Application = {
      name: "n", version: "1", actor: { kind: "edge", key: 1 },
      flow: [
        { id: "src", type_id: "aero.source.decode" }, { id: "sw", type_id: "aero.flow.switch" },
        { id: "ls", type_id: "aero.flow.loop_start" }, { id: "lb", type_id: "aero.flow.loop_back" },
        { id: "out", type_id: "aero.output.sum" },
      ],
      edges: [
        { from: "src", to: "sw" },
        { from: "sw", from_port: "true", to: "ls" },
        { from: "ls", to: "lb" }, { from: "lb", from_port: "loop_back", to: "lb" }, { from: "lb", to: "out" },
        { from: "sw", from_port: "false", to: "out" },
      ],
    };
    expect(validateGraph(app).some((e) => e.includes("reached only via a labeled branch edge"))).toBe(true);
  });
});
