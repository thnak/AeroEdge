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
});
