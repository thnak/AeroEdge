// jigsawPath is pure geometry — no DOM/react-flow needed to test it. These checks are structural
// (starts/ends correctly, produces a notch/tab command when asked, doesn't when not) rather than
// pixel-exact, since the exact curve shape is a visual call, not a correctness contract.
import { describe, it, expect } from "vitest";
import { jigsawPath } from "../jigsawPath";

describe("jigsawPath", () => {
  it("closes the path (starts and ends the outline)", () => {
    const d = jigsawPath({ width: 190, height: 140, topNotch: true, bottomTabs: 1 });
    expect(d.startsWith("M")).toBe(true);
    expect(d.trim().endsWith("Z")).toBe(true);
  });

  it("a hat piece (topNotch: false) has no curve command before the first corner", () => {
    const hat = jigsawPath({ width: 190, height: 140, topNotch: false, bottomTabs: 1 });
    const topEdge = hat.split("Q")[0]; // everything before the first rounded corner
    expect(topEdge.includes("C")).toBe(false);
  });

  it("a body piece (topNotch: true) has a scallop on the top edge", () => {
    const body = jigsawPath({ width: 190, height: 140, topNotch: true, bottomTabs: 1 });
    const topEdge = body.split("Q")[0];
    expect(topEdge.includes("C")).toBe(true);
  });

  it("bottomTabs: 2 produces two scallops on the bottom edge, bottomTabs: 1 produces one", () => {
    const two = jigsawPath({ width: 190, height: 140, topNotch: true, bottomTabs: 2 });
    const one = jigsawPath({ width: 190, height: 140, topNotch: true, bottomTabs: 1 });
    const countC = (d: string) => (d.match(/C /g) ?? []).length;
    // 1 scallop = 2 "C" commands (two bezier segments per scallop, per the `scallop()` helper).
    expect(countC(one)).toBe(countC(two) - 2);
  });

  it("bottomTabs: 0 has no scallop on the bottom edge at all", () => {
    const d = jigsawPath({ width: 190, height: 140, topNotch: false, bottomTabs: 0 });
    expect((d.match(/C /g) ?? []).length).toBe(0);
  });
});
