// Parse/serialize round-trip tests for the expr DSL bridge (020 §5). Valid/malformed example strings
// are lifted directly from `tests/nodes/expr_rule.cpp` — the C++ grammar's own test fixture — so this
// suite is cross-checked against the real parser, not guessed independently of it.
import { describe, it, expect } from "vitest";
import {
  parseExpr,
  serializeExpr,
  shapeOf,
  collectTagRefs,
  getChildren,
  withChild,
  defaultNodeFor,
  kindIdOf,
  blockKindsFor,
  BLOCK_KINDS,
  type ExprNode,
} from "../exprAst";

// Evaluate a parsed tree the same way the C++ interpreter would (booleans as 1.0/0.0), for cross-
// checking semantic round-trip against known (expr, tags) -> value pairs from the C++ test fixture.
function evaluate(node: ExprNode, tags: Record<string, number>): number {
  switch (node.kind) {
    case "num":
      return node.value;
    case "tag":
      return tags[node.name] ?? 0;
    case "unary": {
      const v = evaluate(node.arg, tags);
      return node.op === "neg" ? -v : v === 0 ? 1 : 0;
    }
    case "binary": {
      const l = evaluate(node.left, tags);
      const r = evaluate(node.right, tags);
      switch (node.op) {
        case "+": return l + r;
        case "-": return l - r;
        case "*": return l * r;
        case "/": return r === 0 ? 0 : l / r;
        case "<": return l < r ? 1 : 0;
        case ">": return l > r ? 1 : 0;
        case "<=": return l <= r ? 1 : 0;
        case ">=": return l >= r ? 1 : 0;
        case "==": return l === r ? 1 : 0;
        case "!=": return l !== r ? 1 : 0;
        case "&&": return l !== 0 && r !== 0 ? 1 : 0;
        case "||": return l !== 0 || r !== 0 ? 1 : 0;
      }
    }
  }
}

describe("parseExpr — valid strings from tests/nodes/expr_rule.cpp", () => {
  const cases: [string, Record<string, number>, number][] = [
    ["raw > 50", { raw: 60 }, 1],
    ["raw > 50", { raw: 40 }, 0],
    ["raw >= 10 && raw < 20", { raw: 15 }, 1],
    ["raw >= 10 && raw < 20", { raw: 25 }, 0],
    ["raw == 0", { raw: 0 }, 1],
    ["raw != 0", { raw: 0 }, 0],
    ["raw * 2 + 1 > 10", { raw: 5 }, 1],
    ["raw * 2 + 1 > 10", { raw: 4 }, 0],
    ["(raw + 1) * 2 == 12", { raw: 5 }, 1],
    ["raw / 0 > 1", { raw: 100 }, 0],
    ["!(raw == 0)", { raw: 5 }, 1],
    ["raw < 0 || raw > 100", { raw: 150 }, 1],
    ["raw < 0 || raw > 100", { raw: 50 }, 0],
    ["-raw > -10", { raw: 5 }, 1],
    ['tag("temp") > 100', { temp: 120, raw: 0 }, 1],
    ["temp - raw > 5", { temp: 20, raw: 10 }, 1],
    ["ghost > 1", { raw: 99 }, 0],
  ];

  for (const [expr, tags, expected] of cases) {
    it(`parses and evaluates "${expr}" @ ${JSON.stringify(tags)} -> ${expected}`, () => {
      const parsed = parseExpr(expr);
      expect(parsed.ok).toBe(true);
      if (!parsed.ok) return;
      expect(evaluate(parsed.tree, tags)).toBe(expected);
    });
  }

  it("accepts a bare quoted-string tag reference (undocumented-but-real third tagref form)", () => {
    const parsed = parseExpr('"temp" > 100');
    expect(parsed.ok).toBe(true);
    if (parsed.ok) expect(evaluate(parsed.tree, { temp: 120 })).toBe(1);
  });
});

describe("parseExpr — malformed strings from tests/nodes/expr_rule.cpp", () => {
  for (const bad of ["raw >", "(1 + 2", "raw & 3", "* 5", "tag(raw)", "1 2 3", ""]) {
    it(`rejects "${bad}"`, () => {
      const parsed = parseExpr(bad);
      expect(parsed.ok).toBe(false);
      if (!parsed.ok) expect(parsed.error.length).toBeGreaterThan(0);
    });
  }
});

describe("serializeExpr — precedence/associativity", () => {
  it("round-trips every valid example string to a structurally-equivalent tree", () => {
    for (const expr of ["raw > 50", "raw >= 10 && raw < 20", "raw * 2 + 1 > 10", "(raw + 1) * 2 == 12",
      "raw / 0 > 1", "!(raw == 0)", "raw < 0 || raw > 100", "-raw > -10"]) {
      const first = parseExpr(expr);
      expect(first.ok).toBe(true);
      if (!first.ok) continue;
      const serialized = serializeExpr(first.tree);
      const second = parseExpr(serialized);
      expect(second.ok).toBe(true);
      if (!second.ok) continue;
      expect(second.tree).toEqual(first.tree);
    }
  });

  it("keeps parens on the right operand of a non-associative op at equal precedence (a - (b - c))", () => {
    const tree: ExprNode = {
      kind: "binary", op: "-",
      left: { kind: "tag", name: "a" },
      right: { kind: "binary", op: "-", left: { kind: "tag", name: "b" }, right: { kind: "tag", name: "c" } },
    };
    const text = serializeExpr(tree);
    expect(text).toBe('tag("a") - (tag("b") - tag("c"))');
    // and evaluating this specific string must match the original tree's shape, not (a-b)-c's.
    const reparsed = parseExpr(text);
    expect(reparsed.ok).toBe(true);
    if (reparsed.ok) expect(evaluate(reparsed.tree, { a: 10, b: 3, c: 1 })).toBe(8); // 10 - (3 - 1)
  });

  it("drops parens on the left operand at equal precedence ((a - b) - c stays a - b - c)", () => {
    const tree: ExprNode = {
      kind: "binary", op: "-",
      left: { kind: "binary", op: "-", left: { kind: "tag", name: "a" }, right: { kind: "tag", name: "b" } },
      right: { kind: "tag", name: "c" },
    };
    expect(serializeExpr(tree)).toBe('tag("a") - tag("b") - tag("c")');
  });

  it("wraps a binary child of a unary op in parens", () => {
    const tree: ExprNode = {
      kind: "unary", op: "neg",
      arg: { kind: "binary", op: "+", left: { kind: "num", value: 1 }, right: { kind: "num", value: 2 } },
    };
    expect(serializeExpr(tree)).toBe("-(1 + 2)");
  });

  it("always serializes a tag node in canonical tag(\"name\") form", () => {
    expect(serializeExpr({ kind: "tag", name: "raw" })).toBe('tag("raw")');
  });
});

describe("shapeOf", () => {
  it("classifies leaves and arithmetic as reporter, comparisons/boolean as boolean", () => {
    expect(shapeOf({ kind: "num", value: 1 })).toBe("reporter");
    expect(shapeOf({ kind: "tag", name: "x" })).toBe("reporter");
    expect(shapeOf({ kind: "unary", op: "neg", arg: { kind: "num", value: 1 } })).toBe("reporter");
    expect(shapeOf({ kind: "unary", op: "not", arg: { kind: "num", value: 1 } })).toBe("boolean");
    const num = (v: number): ExprNode => ({ kind: "num", value: v });
    expect(shapeOf({ kind: "binary", op: "+", left: num(1), right: num(2) })).toBe("reporter");
    expect(shapeOf({ kind: "binary", op: ">", left: num(1), right: num(2) })).toBe("boolean");
    expect(shapeOf({ kind: "binary", op: "&&", left: num(1), right: num(2) })).toBe("boolean");
  });
});

describe("collectTagRefs", () => {
  it("collects every tag name referenced anywhere in the tree", () => {
    const parsed = parseExpr("temp - raw > 5 && flag == 1");
    expect(parsed.ok).toBe(true);
    if (parsed.ok) expect(collectTagRefs(parsed.tree).sort()).toEqual(["flag", "raw", "temp"]);
  });
});

describe("tree editing helpers", () => {
  it("getChildren/withChild round-trip for unary and binary nodes", () => {
    const bin: ExprNode = { kind: "binary", op: "+", left: { kind: "num", value: 1 }, right: { kind: "num", value: 2 } };
    expect(getChildren(bin)).toEqual([{ kind: "num", value: 1 }, { kind: "num", value: 2 }]);
    const replaced = withChild(bin, 1, { kind: "num", value: 9 });
    expect(replaced).toEqual({ kind: "binary", op: "+", left: { kind: "num", value: 1 }, right: { kind: "num", value: 9 } });

    const un: ExprNode = { kind: "unary", op: "neg", arg: { kind: "num", value: 1 } };
    expect(getChildren(un)).toEqual([{ kind: "num", value: 1 }]);

    expect(getChildren({ kind: "num", value: 1 })).toEqual([]);
    expect(getChildren({ kind: "tag", name: "x" })).toEqual([]);
  });

  it("BLOCK_KINDS defaults are all internally valid and shape-consistent", () => {
    for (const k of BLOCK_KINDS) {
      const node = k.make();
      expect(shapeOf(node)).toBe(k.shape);
      expect(kindIdOf(node)).toBe(k.id);
      expect(getChildren(node).length).toBe(k.childShapes.length);
      getChildren(node).forEach((c, i) => expect(shapeOf(c)).toBe(k.childShapes[i]));
    }
  });

  it("blockKindsFor filters by shape, and 'any' returns everything", () => {
    expect(blockKindsFor("reporter").every((k) => k.shape === "reporter")).toBe(true);
    expect(blockKindsFor("boolean").every((k) => k.shape === "boolean")).toBe(true);
    expect(blockKindsFor("any").length).toBe(BLOCK_KINDS.length);
  });

  it("defaultNodeFor produces a valid node of the requested shape", () => {
    expect(shapeOf(defaultNodeFor("reporter"))).toBe("reporter");
    expect(shapeOf(defaultNodeFor("boolean"))).toBe("boolean");
  });
});
