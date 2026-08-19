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
});
