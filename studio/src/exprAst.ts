// A tree-shaped bridge to the `expr_detail::Program` DSL (`include/aero/nodes/expr_rule_node.hpp`)
// consumed by aero.rule.expr / aero.flow.switch's `expr` config field (020 §5). Pure functions, no
// React: a straight TypeScript port of that header's own recursive-descent grammar, producing/
// consuming a TREE (the natural shape for a block editor) rather than replicating its RPN compilation —
// that's purely a C++-side execution artifact (`Program`/`Instr`/`evaluate()`); the frontend never
// needs it, only the surface text syntax below.
//
// GRAMMAR (mirrors expr_rule_node.hpp's own header comment, verified against the real parser code —
// including one form the header comment omits: a bare `"name"` string literal is ALSO a tag reference,
// same as `tag("name")`, per `parse_primary`'s `c == '"'` branch):
//   expr    := or
//   or      := and    ( '||' and )*
//   and     := equ    ( '&&' equ )*
//   equ     := cmp    ( ('=='|'!=') cmp )*
//   cmp     := add    ( ('<'|'>'|'<='|'>=') add )*
//   add     := mul    ( ('+'|'-') mul )*
//   mul     := unary  ( ('*'|'/') unary )*
//   unary   := ('!'|'-') unary | primary
//   primary := number | tagref | '(' expr ')'
//   tagref  := 'tag' '(' '"' NAME '"' ')' | '"' NAME '"' | IDENT
// All three tagref forms are accepted on INPUT (a value hand-typed before this editor existed must
// still load) — but `serializeExpr` always OUTPUTS the canonical `tag("name")` form, so generated text
// is unambiguous. Round-trip is SEMANTIC, not byte-exact: original spacing/redundant parens/which
// tagref form was used are not preserved, only the evaluated meaning.
//
// The DSL's own lexer has NO escape mechanism for `"` inside a tag name (`parse_string_body` scans to
// the next raw `"`, no backslash handling) — a tag name containing `"` cannot be represented at all.
// Callers building tag names from user input (ExprTreeEditor.tsx) must keep `"` out at the source.

export type BinOp = "+" | "-" | "*" | "/" | "<" | ">" | "<=" | ">=" | "==" | "!=" | "&&" | "||";
export type UnaryOp = "neg" | "not";

export type ExprNode =
  | { kind: "num"; value: number }
  | { kind: "tag"; name: string }
  | { kind: "unary"; op: UnaryOp; arg: ExprNode }
  | { kind: "binary"; op: BinOp; left: ExprNode; right: ExprNode };

export type ParseResult = { ok: true; tree: ExprNode } | { ok: false; error: string };

export type Shape = "reporter" | "boolean";

const BOOLEAN_BIN_OPS = new Set<BinOp>(["<", ">", "<=", ">=", "==", "!=", "&&", "||"]);

// Reporter (produces a number) vs Boolean (produces true/false) — the doc's §2/§3 shape taxonomy,
// derived structurally from the node's own operator, never stored redundantly on the node itself.
export function shapeOf(node: ExprNode): Shape {
  switch (node.kind) {
    case "num":
    case "tag":
      return "reporter";
    case "unary":
      return node.op === "not" ? "boolean" : "reporter";
    case "binary":
      return BOOLEAN_BIN_OPS.has(node.op) ? "boolean" : "reporter";
  }
}

// --- parsing (string -> tree) ---------------------------------------------------------------------

class ParseError extends Error {}

const PRECEDENCE: Record<BinOp, number> = {
  "||": 1,
  "&&": 2,
  "==": 3,
  "!=": 3,
  "<": 4,
  ">": 4,
  "<=": 4,
  ">=": 4,
  "+": 5,
  "-": 5,
  "*": 6,
  "/": 6,
};

function isDigit(c: string): boolean {
  return c >= "0" && c <= "9";
}
function isIdentStart(c: string): boolean {
  return (c >= "a" && c <= "z") || (c >= "A" && c <= "Z") || c === "_";
}
function isIdentChar(c: string): boolean {
  return isIdentStart(c) || isDigit(c) || c === ".";
}
function isSpace(c: string): boolean {
  return c === " " || c === "\t" || c === "\n" || c === "\r";
}

class Parser {
  private pos = 0;
  constructor(private src: string) {}

  parse(): ExprNode {
    const tree = this.parseOr();
    this.skipWs();
    if (this.pos !== this.src.length) {
      throw new ParseError(`unexpected trailing input at position ${this.pos}`);
    }
    return tree;
  }

  private peek(): string {
    return this.pos < this.src.length ? this.src[this.pos] : "";
  }
  private skipWs(): void {
    while (this.pos < this.src.length && isSpace(this.src[this.pos])) this.pos++;
  }
  private match1(c: string): boolean {
    this.skipWs();
    if (this.peek() === c) {
      this.pos++;
      return true;
    }
    return false;
  }
  private match2(a: string, b: string): boolean {
    this.skipWs();
    if (this.pos + 1 < this.src.length && this.src[this.pos] === a && this.src[this.pos + 1] === b) {
      this.pos += 2;
      return true;
    }
    return false;
  }

  private parseOr(): ExprNode {
    let left = this.parseAnd();
    for (;;) {
      if (this.match2("|", "|")) left = { kind: "binary", op: "||", left, right: this.parseAnd() };
      else break;
    }
    return left;
  }
  private parseAnd(): ExprNode {
    let left = this.parseEqu();
    for (;;) {
      if (this.match2("&", "&")) left = { kind: "binary", op: "&&", left, right: this.parseEqu() };
      else break;
    }
    return left;
  }
  private parseEqu(): ExprNode {
    let left = this.parseCmp();
    for (;;) {
      if (this.match2("=", "=")) left = { kind: "binary", op: "==", left, right: this.parseCmp() };
      else if (this.match2("!", "=")) left = { kind: "binary", op: "!=", left, right: this.parseCmp() };
      else break;
    }
    return left;
  }
  private parseCmp(): ExprNode {
    let left = this.parseAdd();
    for (;;) {
      if (this.match2("<", "=")) left = { kind: "binary", op: "<=", left, right: this.parseAdd() };
      else if (this.match2(">", "=")) left = { kind: "binary", op: ">=", left, right: this.parseAdd() };
      else if (this.match1("<")) left = { kind: "binary", op: "<", left, right: this.parseAdd() };
      else if (this.match1(">")) left = { kind: "binary", op: ">", left, right: this.parseAdd() };
      else break;
    }
    return left;
  }
  private parseAdd(): ExprNode {
    let left = this.parseMul();
    for (;;) {
      if (this.match1("+")) left = { kind: "binary", op: "+", left, right: this.parseMul() };
      else if (this.match1("-")) left = { kind: "binary", op: "-", left, right: this.parseMul() };
      else break;
    }
    return left;
  }
  private parseMul(): ExprNode {
    let left = this.parseUnary();
    for (;;) {
      if (this.match1("*")) left = { kind: "binary", op: "*", left, right: this.parseUnary() };
      else if (this.match1("/")) left = { kind: "binary", op: "/", left, right: this.parseUnary() };
      else break;
    }
    return left;
  }
  private parseUnary(): ExprNode {
    this.skipWs();
    if (this.match1("!")) return { kind: "unary", op: "not", arg: this.parseUnary() };
    if (this.match1("-")) return { kind: "unary", op: "neg", arg: this.parseUnary() };
    return this.parsePrimary();
  }
  private parsePrimary(): ExprNode {
    this.skipWs();
    const c = this.peek();
    if (c === "(") {
      this.pos++;
      const tree = this.parseOr();
      this.skipWs();
      if (!this.match1(")")) throw new ParseError("expected ')'");
      return tree;
    }
    if (c === '"') return this.parseStringBody();
    if (isDigit(c) || c === ".") return this.parseNumber();
    if (isIdentStart(c)) return this.parseIdent();
    throw new ParseError(`unexpected character '${c || "?"}' at position ${this.pos}`);
  }
  private parseNumber(): ExprNode {
    const start = this.pos;
    while (
      this.pos < this.src.length &&
      (isDigit(this.src[this.pos]) ||
        this.src[this.pos] === "." ||
        this.src[this.pos] === "e" ||
        this.src[this.pos] === "E" ||
        ((this.src[this.pos] === "+" || this.src[this.pos] === "-") &&
          this.pos > start &&
          (this.src[this.pos - 1] === "e" || this.src[this.pos - 1] === "E")))
    ) {
      this.pos++;
    }
    const num = this.src.slice(start, this.pos);
    // Mirrors std::stod's "consume the whole scanned substring or fail" contract.
    if (!/[0-9]/.test(num) || !/^[0-9]*\.?[0-9]*([eE][+-]?[0-9]+)?$/.test(num)) {
      throw new ParseError(`malformed number '${num}'`);
    }
    const v = Number(num);
    if (!Number.isFinite(v)) throw new ParseError(`malformed number '${num}'`);
    return { kind: "num", value: v };
  }
  private parseIdent(): ExprNode {
    const start = this.pos;
    while (this.pos < this.src.length && isIdentChar(this.src[this.pos])) this.pos++;
    const ident = this.src.slice(start, this.pos);
    if (ident === "tag") {
      this.skipWs();
      if (!this.match1("(")) throw new ParseError("expected '(' after 'tag'");
      this.skipWs();
      if (this.peek() !== '"') throw new ParseError('expected a quoted tag name in tag("...")');
      const node = this.parseStringBody();
      this.skipWs();
      if (!this.match1(")")) throw new ParseError("expected ')' closing tag(...)");
      return node;
    }
    // A bare identifier IS a tag reference (e.g. `raw`).
    return { kind: "tag", name: ident };
  }
  // A `"..."`-quoted tag name, used both for `tag("...")`'s argument and a bare `"..."` primary.
  private parseStringBody(): ExprNode {
    if (!this.match1('"')) throw new ParseError("expected '\"'");
    const start = this.pos;
    while (this.pos < this.src.length && this.src[this.pos] !== '"') this.pos++;
    if (this.pos >= this.src.length) throw new ParseError("unterminated string literal");
    const name = this.src.slice(start, this.pos);
    this.pos++; // closing quote
    if (name === "") throw new ParseError("empty tag name");
    return { kind: "tag", name };
  }
}

export function parseExpr(src: string): ParseResult {
  try {
    return { ok: true, tree: new Parser(src).parse() };
  } catch (e) {
    if (e instanceof ParseError) return { ok: false, error: e.message };
    throw e;
  }
}

// --- serializing (tree -> string) ------------------------------------------------------------------

function needsParensAsChild(child: ExprNode, parentPrec: number, side: "left" | "right"): boolean {
  if (child.kind !== "binary") return false; // num/tag/unary are always directly attachable, no parens
  const childPrec = PRECEDENCE[child.op];
  return side === "left" ? childPrec < parentPrec : childPrec <= parentPrec;
}

export function serializeExpr(node: ExprNode): string {
  switch (node.kind) {
    case "num":
      return String(node.value);
    case "tag":
      return `tag("${node.name}")`;
    case "unary": {
      // A unary op's operand is `unary | primary` in the grammar — a binary child needs explicit
      // parens (parsed back in via primary's `'(' expr ')'`), anything else attaches directly.
      const argStr =
        node.arg.kind === "binary" ? `(${serializeExpr(node.arg)})` : serializeExpr(node.arg);
      return (node.op === "neg" ? "-" : "!") + argStr;
    }
    case "binary": {
      const p = PRECEDENCE[node.op];
      const leftStr = needsParensAsChild(node.left, p, "left")
        ? `(${serializeExpr(node.left)})`
        : serializeExpr(node.left);
      const rightStr = needsParensAsChild(node.right, p, "right")
        ? `(${serializeExpr(node.right)})`
        : serializeExpr(node.right);
      return `${leftStr} ${node.op} ${rightStr}`;
    }
  }
}

// --- tree utilities used by the editor UI -----------------------------------------------------------

export function collectTagRefs(node: ExprNode): string[] {
  switch (node.kind) {
    case "num":
      return [];
    case "tag":
      return [node.name];
    case "unary":
      return collectTagRefs(node.arg);
    case "binary":
      return [...collectTagRefs(node.left), ...collectTagRefs(node.right)];
  }
}

export function getChildren(node: ExprNode): ExprNode[] {
  if (node.kind === "unary") return [node.arg];
  if (node.kind === "binary") return [node.left, node.right];
  return [];
}

export function withChild(node: ExprNode, index: number, child: ExprNode): ExprNode {
  if (node.kind === "unary") return { ...node, arg: child };
  if (node.kind === "binary") return index === 0 ? { ...node, left: child } : { ...node, right: child };
  return node;
}

export function defaultNodeFor(shape: Shape): ExprNode {
  if (shape === "reporter") return { kind: "num", value: 0 };
  return { kind: "binary", op: "==", left: { kind: "num", value: 0 }, right: { kind: "num", value: 0 } };
}

export function kindIdOf(node: ExprNode): string {
  if (node.kind === "unary") return node.op;
  if (node.kind === "binary") return node.op;
  return node.kind; // "num" | "tag"
}

export interface BlockKindDef {
  id: string;
  label: string;
  shape: Shape;
  childShapes: Shape[];
  make: () => ExprNode;
}

const binaryKind = (op: BinOp, label: string, shape: Shape, childShape: Shape): BlockKindDef => ({
  id: op,
  label,
  shape,
  childShapes: [childShape, childShape],
  make: () => ({ kind: "binary", op, left: defaultNodeFor(childShape), right: defaultNodeFor(childShape) }),
});

// The full palette an "add block" menu can offer — one entry per concrete block kind. A menu filters
// this list by the target socket's required shape (`blockKindsFor`), so an incompatible block can
// never even appear as an option — the doc's "shape is the type check" idea enforced at menu time
// rather than by drag-drop rejection (020 §5's chosen v1 interaction model).
export const BLOCK_KINDS: BlockKindDef[] = [
  { id: "num", label: "Number", shape: "reporter", childShapes: [], make: () => ({ kind: "num", value: 0 }) },
  {
    id: "tag",
    label: "Tag reference",
    shape: "reporter",
    childShapes: [],
    make: () => ({ kind: "tag", name: "raw" }),
  },
  {
    id: "neg",
    label: "Negate (−x)",
    shape: "reporter",
    childShapes: ["reporter"],
    make: () => ({ kind: "unary", op: "neg", arg: defaultNodeFor("reporter") }),
  },
  {
    id: "not",
    label: "Not (!x)",
    shape: "boolean",
    childShapes: ["boolean"],
    make: () => ({ kind: "unary", op: "not", arg: defaultNodeFor("boolean") }),
  },
  binaryKind("+", "Add (+)", "reporter", "reporter"),
  binaryKind("-", "Subtract (−)", "reporter", "reporter"),
  binaryKind("*", "Multiply (×)", "reporter", "reporter"),
  binaryKind("/", "Divide (÷)", "reporter", "reporter"),
  binaryKind("<", "Less than (<)", "boolean", "reporter"),
  binaryKind(">", "Greater than (>)", "boolean", "reporter"),
  binaryKind("<=", "Less or equal (≤)", "boolean", "reporter"),
  binaryKind(">=", "Greater or equal (≥)", "boolean", "reporter"),
  binaryKind("==", "Equal (=)", "boolean", "reporter"),
  binaryKind("!=", "Not equal (≠)", "boolean", "reporter"),
  binaryKind("&&", "And", "boolean", "boolean"),
  binaryKind("||", "Or", "boolean", "boolean"),
];

export function blockKindsFor(shape: Shape | "any"): BlockKindDef[] {
  return shape === "any" ? BLOCK_KINDS : BLOCK_KINDS.filter((k) => k.shape === shape);
}
