// Component tests for the expr-tree block editor (020 §5) — mounts the real controlled component
// behind a small stateful wrapper (same pattern as ModbusRegisterMap would need) so interacting with
// the rendered blocks and observing the resulting `expr` string exercises the real onChange contract,
// not a mocked one.
import { useState } from "react";
import { describe, it, expect } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { ExprTreeEditor } from "../tier2/ExprTreeEditor";

function Wrapper({ initial, knownTags = [] }: { initial: string; knownTags?: string[] }) {
  const [value, setValue] = useState(initial);
  return (
    <div>
      <div data-testid="current-value">{value}</div>
      <ExprTreeEditor value={value} onChange={setValue} knownTags={knownTags} />
    </div>
  );
}

const currentValue = () => screen.getByTestId("current-value").textContent ?? "";

describe("ExprTreeEditor", () => {
  it("renders a valid expression as blocks, not raw text", () => {
    const { container } = render(<Wrapper initial="raw > 50" />);
    expect(screen.getByText("Greater than (>)")).toBeTruthy();
    expect(container.querySelector("textarea")).toBeNull(); // no raw <textarea> while parse succeeds
  });

  it("falls back to raw-text mode for a malformed value, without crashing", () => {
    render(<Wrapper initial="raw >" />);
    expect(screen.getByText(/Can.t show as blocks/)).toBeTruthy();
    expect(screen.getByRole("textbox")).toBeTruthy();
  });

  it("falls back to raw-text mode (silently, no error banner) for a brand-new empty value", () => {
    render(<Wrapper initial="" />);
    expect(screen.queryByText(/Can.t show as blocks/)).toBeNull();
    expect(screen.getByRole("textbox")).toBeTruthy();
    expect(screen.getByText("Start from a blank expression")).toBeTruthy();
  });

  it("'Start from a blank expression' seeds a valid boolean expr and switches to blocks", () => {
    render(<Wrapper initial="" />);
    fireEvent.click(screen.getByText("Start from a blank expression"));
    expect(currentValue()).toBe("0 == 0");
    expect(screen.getByText("Equal (=)")).toBeTruthy();
  });

  it("changing the root block's kind replaces the expression with that kind's default", () => {
    render(<Wrapper initial="raw > 50" />);
    // Every socket (leaf or compound) shows its own "Block kind" select, so the tree has 3 here
    // (root ">" plus its two leaf children) — the root's is first in DOM order (depth-first).
    const [rootSelect] = screen.getAllByLabelText("Block kind") as HTMLSelectElement[];
    fireEvent.change(rootSelect, { target: { value: "&&" } });
    expect(currentValue()).toBe("0 == 0 && 0 == 0");
  });

  it("editing a tag leaf's name updates the serialized expr", () => {
    render(<Wrapper initial="raw > 50" />);
    const tagInput = screen.getByDisplayValue("raw") as HTMLInputElement;
    fireEvent.change(tagInput, { target: { value: "temp" } });
    expect(currentValue()).toBe('tag("temp") > 50');
  });

  it("editing a number leaf's value updates the serialized expr", () => {
    render(<Wrapper initial="raw > 50" />);
    const numInput = screen.getByDisplayValue("50") as HTMLInputElement;
    fireEvent.change(numInput, { target: { value: "75" } });
    expect(currentValue()).toBe('tag("raw") > 75');
  });

  it("strips a double quote typed into a tag name (the DSL has no escape for it)", () => {
    render(<Wrapper initial="raw > 50" />);
    const tagInput = screen.getByDisplayValue("raw") as HTMLInputElement;
    fireEvent.change(tagInput, { target: { value: 'te"mp' } });
    expect(currentValue()).toBe('tag("temp") > 50');
  });

  it("'Edit as text' switches a valid tree into raw-text mode without altering the value", () => {
    render(<Wrapper initial="raw > 50" />);
    fireEvent.click(screen.getByText("Edit as text"));
    expect(screen.getByRole("textbox")).toBeTruthy();
    expect(currentValue()).toBe("raw > 50");
  });

  it("offers known tags as datalist autocomplete options", () => {
    render(<Wrapper initial="raw > 50" knownTags={["raw", "temp", "flag"]} />);
    const list = document.getElementById("expr-known-tags");
    expect(list).toBeTruthy();
    expect(list!.querySelectorAll("option").length).toBe(3);
  });

  // 020 §6.2/§6.3 follow-up: the math-function palette.
  it("renders a function-call expr with a unary function's single argument as a nested block", () => {
    render(<Wrapper initial="sqrt(raw) > 3" />);
    // getByDisplayValue on a <select> matches its currently SELECTED option's text — unlike getByText,
    // which would also match the same label appearing as an unselected <option> in every OTHER
    // reporter-shaped socket's select in the tree (every socket offers the full matching palette).
    expect(screen.getByDisplayValue("Square root")).toBeTruthy();
    expect(screen.getByDisplayValue("raw")).toBeTruthy();
  });

  it("renders a binary function's two arguments as two nested blocks", () => {
    render(<Wrapper initial="pow(2, raw) > 3" />);
    expect(screen.getByDisplayValue("Power (base, exp)")).toBeTruthy();
    const numInputs = screen.getAllByRole("spinbutton") as HTMLInputElement[];
    expect(numInputs.map((i) => i.value)).toContain("2");
  });

  it("changing a reporter block's kind to a function replaces it with that function's default args", () => {
    render(<Wrapper initial="raw > 50" />);
    const [rootSelect, leftSelect] = screen.getAllByLabelText("Block kind") as HTMLSelectElement[];
    void rootSelect;
    fireEvent.change(leftSelect, { target: { value: "sqrt" } });
    expect(currentValue()).toBe("sqrt(0) > 50");
  });

  it("groups the kind-select options into optgroups by category", () => {
    render(<Wrapper initial="raw > 50" />);
    // ">"'s own select is shape="any" (the tree's root socket) so it sees every category; its LEFT
    // child socket is shape="reporter" (">"'s childShapes), which excludes Comparison/Logic entirely —
    // that's the one that actually proves filtering-by-shape reaches the new Math function group too.
    const [, leftSelect] = screen.getAllByLabelText("Block kind") as HTMLSelectElement[];
    const groupLabels = Array.from(leftSelect.querySelectorAll("optgroup")).map((g) => g.getAttribute("label"));
    expect(groupLabels).toContain("Math function");
    expect(groupLabels).not.toContain("Comparison");
    expect(groupLabels).not.toContain("Logic");
  });
});
