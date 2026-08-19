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
    // Labels are humanized client-side from the type_id (catalog.ts) now that the catalog comes from
    // GET /catalog, which carries no per-node-type display label (015 U1) — see test-setup.ts's fixture.
    expect(screen.getByText("Decode")).toBeTruthy();
    expect(screen.getByText("Scale")).toBeTruthy();
    expect(screen.getByText("Sum")).toBeTruthy();
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

  // 019 §4 jigsaw slice: a Source card renders no target handle at all — nothing can ever be wired
  // into it, by shape, not just by a validation message (the literal "Sum before Decode" case).
  it("a Source card has no target handle; other categories do", () => {
    const { container } = render(<Wrapper />);
    const cards = container.querySelectorAll(".flow-node-card");
    expect(cards.length).toBe(3); // Decode (Source), Scale (Transform), Sum (Output)
    expect(cards[0].classList.contains("cat-source")).toBe(true);
    expect(cards[0].querySelector(".react-flow__handle.target")).toBeNull();
    expect(cards[1].querySelector(".react-flow__handle.target")).not.toBeNull();
    expect(cards[2].querySelector(".react-flow__handle.target")).not.toBeNull();
  });

  // 020 §4.3: the Cap-shape symmetric case — aero.output.sum is `terminal` in the catalog fixture, so
  // its card renders no SOURCE handle either (nothing may follow it, made visible as a shape, not just
  // a deploy-time validation message). Scale (non-terminal Transform) keeps its source handle.
  it("a terminal (Cap-shape) card has no source handle; a non-terminal one does", () => {
    const { container } = render(<Wrapper />);
    const cards = container.querySelectorAll(".flow-node-card");
    expect(cards[1].querySelector(".react-flow__handle.source")).not.toBeNull(); // Scale
    expect(cards[2].querySelector(".react-flow__handle.source")).toBeNull(); // Sum, terminal
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

// 019 §4: the canvas is a real graph editor now — a switch node's card renders both branch handles,
// and removing a node in graph mode prunes any edge that referenced it.
const BRANCHING_APP: Application = {
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

function BranchingWrapper() {
  const [model, setModel] = useState<FlowModel>(() => fromApplication(BRANCHING_APP));
  return <FlowDesigner model={model} onChange={setModel} />;
}

describe("FlowDesigner canvas — graph mode (019 §4)", () => {
  it("renders true/false branch handle labels on a switch node's card", () => {
    render(<BranchingWrapper />);
    expect(screen.getByText("true")).toBeTruthy();
    expect(screen.getByText("false")).toBeTruthy();
  });

  it("removing a node in graph mode drops edges that referenced it", () => {
    render(<BranchingWrapper />);
    const json = () => JSON.parse(screen.getByText(/"flow"/).textContent!) as Application;
    expect(json().edges?.length).toBe(5);

    const removeButtons = screen.getAllByText("✕");
    // cards render in `model.nodes` order: src, sw, hi, lo, out — remove "hi".
    fireEvent.click(removeButtons[2]);

    const after = json();
    expect(after.flow.some((n) => n.id === "hi")).toBe(false);
    expect(after.edges?.some((e) => e.from === "hi" || e.to === "hi")).toBe(false);
    expect(after.edges?.length).toBe(3); // sw->hi and hi->out both dropped
  });
});

// 020 §8 (Studio side): a closed loop_start/loop_back pair renders as one C-block (loopBlock) with a
// single "body" cavity, once graph mode recognizes the "loop_back"-labeled back edge.
const LOOP_APP: Application = {
  name: "looping", version: "0.1.0",
  actor: { kind: "edge", key: 1 },
  flow: [
    { id: "src", type_id: "aero.source.decode" },
    { id: "ls", type_id: "aero.flow.loop_start", config: { counter_tag: "i", start_expr: "0" } },
    { id: "acc", type_id: "aero.transform.scale", config: { factor: 2 } },
    { id: "lb", type_id: "aero.flow.loop_back", config: { counter_tag: "i", step_expr: "1", end_expr: "9" } },
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

function LoopWrapper() {
  const [model, setModel] = useState<FlowModel>(() => fromApplication(LOOP_APP));
  return <FlowDesigner model={model} onChange={setModel} />;
}

describe("FlowDesigner canvas — loop C-block (020 §8)", () => {
  it("renders the fused loop-block header + body cavity once the pair is closed", () => {
    const { container } = render(<LoopWrapper />);
    expect(screen.getByText("⟲ Loop")).toBeTruthy();
    expect(screen.getByText("body")).toBeTruthy();
    expect(container.querySelectorAll(".loop-block").length).toBe(1);
    // "acc" is absorbed as a cavity member card (still a plain flow-node-card, just repositioned) —
    // its own label stays visible, distinct from the fused header's "⟲ Loop".
    expect(screen.getByText("Scale")).toBeTruthy();
  });

  it("still renders loop_back as its own selectable card, not folded into the header", () => {
    render(<LoopWrapper />);
    // humanize("aero.flow.loop_back") -> "Loop Back", prefixed with the loop glyph.
    expect(screen.getByText("⟲ Loop Back")).toBeTruthy();
  });

  it("selecting the fused header's Configure panel shows loop_start's own fields", () => {
    render(<LoopWrapper />);
    fireEvent.click(screen.getByText("⟲ Loop"));
    expect(screen.getByDisplayValue("i")).toBeTruthy(); // counter_tag
    expect(screen.getByDisplayValue("0")).toBeTruthy(); // start_expr
  });
});

const RULE_APP: Application = {
  name: "rule-demo",
  version: "1.0.0",
  actor: { kind: "edge", key: 1 },
  flow: [
    { type_id: "aero.source.decode" },
    { type_id: "aero.rule.expr", config: { expr: "raw > 50" } },
    { type_id: "aero.output.sum" },
  ],
};

function RuleWrapper() {
  const [model, setModel] = useState<FlowModel>(() => fromApplication(RULE_APP));
  return <FlowDesigner model={model} onChange={setModel} />;
}

describe("FlowDesigner Configure panel — expr-tree editor (020 §5)", () => {
  it("mounts the block-tree editor, not a plain text input, for a selected rule node's expr field", () => {
    render(<RuleWrapper />);
    // Node 0 (Decode) is selected by default; select the rule node's card to see its Configure panel.
    fireEvent.click(screen.getByText("Expr")); // humanize("aero.rule.expr") -> "Expr"
    expect(screen.getByText("Greater than (>)")).toBeTruthy(); // the ">" block's kind-select option
    expect(screen.getByDisplayValue("raw")).toBeTruthy(); // the tag leaf's inline input
    expect(screen.getByDisplayValue("50")).toBeTruthy(); // the number leaf's inline input
  });
});
