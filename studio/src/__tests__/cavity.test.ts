// Pure-function tests for the switch C-block cavity logic (020 §4.2). Same "plain data function, no
// DOM/react-flow simulation" posture flow_graph_model.test.ts already established for application.ts's
// graph helpers — computeCavities/absorbedEdgeIds/attachToCavity/detachFromCavity never touch the DOM.
import { describe, it, expect } from "vitest";
import type { FlowModel, GraphEdge } from "../application";
import { computeCavities, absorbedEdgeIds, attachToCavity, detachFromCavity } from "../cavity";

function edge(from: string, to: string, fromPort?: string): GraphEdge {
  return { id: `e:${from}:${fromPort ?? ""}:${to}`, from, to, fromPort };
}

function baseModel(edges: GraphEdge[]): FlowModel {
  return {
    name: "cavity-demo",
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
    edges,
  };
}

describe("computeCavities", () => {
  it("a direct branch edge nests its target at index 0", () => {
    const edges = [edge("src", "sw"), edge("sw", "hi", "true"), edge("sw", "lo", "false")];
    const { chains, membership } = computeCavities(edges);
    expect(chains.get("sw")?.get("true")).toEqual(["hi"]);
    expect(chains.get("sw")?.get("false")).toEqual(["lo"]);
    expect(membership.get("hi")).toEqual({ switchId: "sw", label: "true", index: 0 });
    expect(membership.get("lo")).toEqual({ switchId: "sw", label: "false", index: 0 });
  });

  it("walks a chain of single-in/single-out unlabeled edges", () => {
    const edges = [
      edge("src", "sw"),
      edge("sw", "hi", "true"),
      edge("hi", "lo"), // unlabeled, hi has exactly 1 outgoing, lo has exactly 1 incoming -> chained
    ];
    const { chains, membership } = computeCavities(edges);
    expect(chains.get("sw")?.get("true")).toEqual(["hi", "lo"]);
    expect(membership.get("lo")).toEqual({ switchId: "sw", label: "true", index: 1 });
  });

  it("stops the chain at a merge point (shared with something else) — the rejoin stays un-nested", () => {
    const edges = [
      edge("src", "sw"),
      edge("sw", "hi", "true"),
      edge("sw", "lo", "false"),
      edge("hi", "out"), // "out" also reached from "lo" below -> in-degree 2 -> merge point
      edge("lo", "out"),
    ];
    const { chains, membership } = computeCavities(edges);
    expect(chains.get("sw")?.get("true")).toEqual(["hi"]);
    expect(chains.get("sw")?.get("false")).toEqual(["lo"]);
    expect(membership.has("out")).toBe(false); // "out" is NOT absorbed — it's the rejoin target
  });

  it("stops the chain at a fan-out (more than one outgoing edge) inside a branch", () => {
    const edges = [
      edge("src", "sw"),
      edge("sw", "hi", "true"),
      edge("hi", "lo"),
      edge("hi", "out"), // hi has 2 outgoing -> chain stops AT hi, lo/out stay un-nested
    ];
    const { chains, membership } = computeCavities(edges);
    expect(chains.get("sw")?.get("true")).toEqual(["hi"]);
    expect(membership.has("lo")).toBe(false);
    expect(membership.has("out")).toBe(false);
  });

  it("no branch-labeled edges -> no cavities at all (legacy/implicit-chain mode)", () => {
    const edges = [edge("src", "sw"), edge("sw", "out")]; // no fromPort anywhere
    const { chains, membership } = computeCavities(edges);
    expect(chains.size).toBe(0);
    expect(membership.size).toBe(0);
  });
});

describe("absorbedEdgeIds", () => {
  it("absorbs direct branch edges and internal chain links, but not a rejoin stub at a real merge point", () => {
    const edges = [
      edge("src", "sw"),
      edge("sw", "hi", "true"),
      edge("sw", "lo", "false"),
      edge("hi", "out"), // "out" is fed by BOTH branches -> a real merge point, stays un-nested (stub)
      edge("lo", "out"),
    ];
    const { chains } = computeCavities(edges);
    const absorbed = absorbedEdgeIds(edges, chains);
    expect(absorbed.has(edge("sw", "hi", "true").id)).toBe(true);
    expect(absorbed.has(edge("sw", "lo", "false").id)).toBe(true);
    expect(absorbed.has(edge("hi", "out").id)).toBe(false); // chain's tail -> outside: stays a real edge
    expect(absorbed.has(edge("lo", "out").id)).toBe(false);
    expect(absorbed.has(edge("src", "sw").id)).toBe(false); // unrelated edge untouched
  });

  it("absorbs a whole straight run with no real fan-out/merge, all the way to the end", () => {
    // Deliberately only one branch wired ("false" left unconnected) — since nothing else ever merges
    // into "out", the entire straight run counts as nested (020 §4.2: "a mini Previous/Next stack" has
    // no upper bound on length; it only stops at a REAL fan-out or a REAL merge, not just "far enough").
    const edges = [edge("src", "sw"), edge("sw", "hi", "true"), edge("hi", "lo"), edge("lo", "out")];
    const { chains } = computeCavities(edges);
    expect(chains.get("sw")?.get("true")).toEqual(["hi", "lo", "out"]);
    const absorbed = absorbedEdgeIds(edges, chains);
    expect(absorbed.has(edge("lo", "out").id)).toBe(true);
  });
});

describe("attachToCavity / detachFromCavity", () => {
  it("attaches a free node as the new chain head when the cavity is empty", () => {
    const model = baseModel([edge("src", "sw")]);
    const next = attachToCavity(model, "hi", "sw", "true");
    const { chains, membership } = computeCavities(next.edges);
    expect(chains.get("sw")?.get("true")).toEqual(["hi"]);
    expect(membership.get("hi")?.index).toBe(0);
  });

  it("attaches as the new TAIL when the cavity already has a member", () => {
    const model = baseModel([edge("src", "sw"), edge("sw", "hi", "true")]);
    const next = attachToCavity(model, "lo", "sw", "true");
    const { chains } = computeCavities(next.edges);
    expect(chains.get("sw")?.get("true")).toEqual(["hi", "lo"]);
  });

  it("moving a node from one cavity to the other detaches it first, cleanly", () => {
    const model = baseModel([edge("src", "sw"), edge("sw", "hi", "true")]);
    const next = attachToCavity(model, "hi", "sw", "false");
    const { chains, membership } = computeCavities(next.edges);
    // The old "true" cavity is empty again — no from_port="true" edge is left at all, so it has no
    // chains entry (an EMPTY cavity is "absent", not "present with 0 members" — computeCavities only
    // creates an entry for a label that actually has a branch-labeled edge).
    expect(chains.get("sw")?.get("true")).toBeUndefined();
    expect(chains.get("sw")?.get("false")).toEqual(["hi"]);
    expect(membership.get("hi")?.label).toBe("false");
  });

  it("detaching the chain HEAD of a 2-member chain re-links switch directly to the old successor", () => {
    const model = baseModel([edge("src", "sw"), edge("sw", "hi", "true"), edge("hi", "lo")]);
    const next = detachFromCavity(model, "hi");
    const { chains, membership } = computeCavities(next.edges);
    expect(chains.get("sw")?.get("true")).toEqual(["lo"]); // lo promoted to the new head
    expect(membership.has("hi")).toBe(false); // hi is free now
    expect(next.edges.some((e) => e.from === "hi")).toBe(false); // hi's old outgoing chain link is gone
  });

  it("detaching a MIDDLE member of a 3-chain bridges predecessor -> successor directly", () => {
    const model = baseModel([
      edge("src", "sw"),
      edge("sw", "hi", "true"),
      edge("hi", "lo"),
      edge("lo", "out"),
    ]);
    const next = detachFromCavity(model, "lo");
    const { chains } = computeCavities(next.edges);
    expect(chains.get("sw")?.get("true")).toEqual(["hi", "out"]); // hi now chains straight to out
  });

  it("detaching the TAIL of a chain just drops the incoming edge, nothing to bridge", () => {
    const model = baseModel([edge("src", "sw"), edge("sw", "hi", "true"), edge("hi", "lo")]);
    const next = detachFromCavity(model, "lo");
    const { chains, membership } = computeCavities(next.edges);
    expect(chains.get("sw")?.get("true")).toEqual(["hi"]);
    expect(membership.has("lo")).toBe(false);
    expect(next.edges.some((e) => e.to === "lo")).toBe(false);
  });

  it("detaching a node that isn't nested is a no-op", () => {
    const model = baseModel([edge("src", "sw"), edge("sw", "hi", "true")]);
    const next = detachFromCavity(model, "out");
    expect(next).toBe(model);
  });
});
