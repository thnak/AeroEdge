// FlowDesigner canvas (Phase 11.8, 016 §5.2): the node-graph presentation upgrade must still drive
// the same FlowModel — select/move/remove via the card controls update `model.nodes` exactly like the
// old linear list did, and the emitted Application JSON stays aligned (the real regression guard is
// application.test.ts's hello_flow.json round-trip, unaffected by this file since application.ts
// wasn't touched — this test covers the NEW interaction surface: the canvas cards themselves).
import { describe, it, expect } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { useState } from "react";
import { FlowDesigner } from "../FlowDesigner";
import { fromApplication, type Application, type FlowModel } from "../application";

const APP: Application = {
  name: "demo",
  version: "1.0.0",
  actor: { kind: "edge", key: 1 },
  flow: [
    { type_id: "aero.source.decode" },
    { type_id: "aero.transform.scale", config: { factor: 2 } },
    { type_id: "aero.output.sum" },
  ],
};

function Wrapper() {
  const [model, setModel] = useState<FlowModel>(() => fromApplication(APP));
  return <FlowDesigner model={model} onChange={setModel} />;
}

describe("FlowDesigner canvas", () => {
  it("renders a card per node with its catalog label and type_id", () => {
    render(<Wrapper />);
    expect(screen.getByText("Decode (scalar)")).toBeTruthy();
    expect(screen.getByText("Scale")).toBeTruthy();
    expect(screen.getByText("Sum Output")).toBeTruthy();
    expect(screen.getByText("aero.source.decode")).toBeTruthy();
  });

  // Buttons are queried by their visible glyph text, not role+accessible-name: @xyflow/react nodes
  // render `visibility: hidden` until a real ResizeObserver measures them (a browser-only transient
  // jsdom never reaches), and dom-accessibility-api computes an empty accessible name for anything
  // under a `visibility: hidden` ancestor regardless of aria-label — even with RTL's role queries
  // given `{ hidden: true }` to stop excluding the elements outright. getByText isn't
  // accessibility-tree-based, so it isn't affected; the buttons keep real aria-labels (Move up/down,
  // Remove node) for actual screen-reader users, who see them in a properly laid-out browser.
  it("removing a node's card drops it from the emitted Application", () => {
    render(<Wrapper />);
    const removeButtons = screen.getAllByText("✕"); // one per card, in its row-actions
    expect(removeButtons.length).toBe(3);
    fireEvent.click(removeButtons[1]); // remove the Scale node
    expect(screen.queryByText("aero.transform.scale")).toBeNull();
    expect(screen.getByText("aero.source.decode")).toBeTruthy();
    expect(screen.getByText("aero.output.sum")).toBeTruthy();
  });

  it("moving a node reorders the emitted Application (order still IS the DAG)", () => {
    render(<Wrapper />);
    const json = () => JSON.parse(screen.getByText(/"flow"/).textContent!) as Application;
    expect(json().flow.map((n) => n.type_id)).toEqual([
      "aero.source.decode", "aero.transform.scale", "aero.output.sum",
    ]);

    const downButtons = screen.getAllByText("↓");
    fireEvent.click(downButtons[0]); // move the Decode node down past Scale
    expect(json().flow.map((n) => n.type_id)).toEqual([
      "aero.transform.scale", "aero.source.decode", "aero.output.sum",
    ]);
  });
});
