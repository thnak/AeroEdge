// LOAD-BEARING (013 T3 / 015 U1): the Studio's emitted Application must match the runtime's canonical
// schema exactly, or what the Designer builds won't deploy. We prove alignment with a ROUND-TRIP
// against the REAL examples/hello_flow.json the C++ runtime deploys.
import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { toApplication, fromApplication, validateApplication, type Application } from "../application";
import { sourceIds, outputIds } from "../catalog";

const here = dirname(fileURLToPath(import.meta.url));
const helloPath = resolve(here, "../../../examples/hello_flow.json");
const hello: Application = JSON.parse(readFileSync(helloPath, "utf8"));

describe("Application schema alignment", () => {
  it("round-trips the runtime's hello_flow.json unchanged", () => {
    const model = fromApplication(hello);
    const emitted = toApplication(model);
    // Deep-equal against the exact JSON the runtime ships — the alignment guarantee.
    expect(emitted).toEqual(hello);
  });

  it("emits the minimal shape (no empty config objects)", () => {
    const app = toApplication(fromApplication(hello));
    // decode + sum carry no config key; scale + driver do.
    expect(app.flow[0]).toEqual({ type_id: "aero.source.decode" });
    expect(app.flow[1]).toEqual({ type_id: "aero.transform.scale", config: { factor: 2 } });
    expect(app.flow[2]).toEqual({ type_id: "aero.output.sum" });
    expect(app.driver).toEqual({ type_id: "aero.driver.generator", config: { frame_count: 100 } });
  });

  it("validates a good Application and rejects structural errors", () => {
    expect(validateApplication(hello, sourceIds(), outputIds())).toEqual([]);
    const noSource: Application = { ...hello, flow: [{ type_id: "aero.output.sum" }] };
    expect(validateApplication(noSource, sourceIds(), outputIds())).toContain("flow must start with a Source node");
    const noOutput: Application = { ...hello, flow: [{ type_id: "aero.source.decode" }] };
    expect(validateApplication(noOutput, sourceIds(), outputIds())).toContain("flow must contain an Output node");
    const empty: Application = { ...hello, flow: [] };
    expect(validateApplication(empty, sourceIds(), outputIds())).toContain("flow must have at least one node");
  });

  // 019 §4: a graph Application (explicit node ids + edges[], including a branch label) round-trips
  // just as losslessly as the legacy shape above — proves the new opt-in path is alignment-safe too.
  it("round-trips a graph Application (edges[] + branch labels)", () => {
    const graph: Application = {
      name: "branching", version: "0.1.0",
      actor: { kind: "edge", key: 1 },
      flow: [
        { id: "src", type_id: "aero.source.decode" },
        { id: "sw", type_id: "aero.flow.switch", config: { expr: "raw > 100" } },
        { id: "hi", type_id: "aero.transform.scale", config: { factor: 10 } },
        { id: "lo", type_id: "aero.transform.scale", config: { factor: 1 } },
        { id: "out", type_id: "aero.output.sum" },
      ],
      edges: [
        { from: "src", to: "sw" },
        { from: "sw", from_port: "true", to: "hi" },
        { from: "sw", from_port: "false", to: "lo" },
        { from: "hi", to: "out" },
        { from: "lo", to: "out" },
      ],
    };
    const model = fromApplication(graph);
    expect(toApplication(model)).toEqual(graph);
  });

  // 019 §4: also confirms a graph Application does NOT trip the legacy "flow[0] must be a Source"
  // check just because its root isn't at array index 0.
  it("validates a graph Application by root/reachability, not array position", () => {
    const graph: Application = {
      name: "branching", version: "0.1.0", actor: { kind: "edge", key: 1 },
      flow: [
        { id: "sw", type_id: "aero.flow.switch", config: { expr: "raw > 100" } },
        { id: "src", type_id: "aero.source.decode" },
        { id: "out", type_id: "aero.output.sum" },
      ],
      edges: [{ from: "src", to: "sw" }, { from: "sw", from_port: "true", to: "out" }],
    };
    expect(validateApplication(graph, sourceIds(), outputIds())).toEqual([]);
  });
});
