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
//   mul     := unary  ( ('*'|'/'|'%') unary )*             // '%' = mod, 020 §6.1
//   unary   := ('!'|'-') unary | primary
//   primary := number | tagref | funccall | '(' expr ')'
//   tagref  := 'tag' '(' '"' NAME '"' ')' | '"' NAME '"' | IDENT
//   funccall:= IDENT '(' expr (',' expr)? ')'               // 020 §6.2/§6.3 math functions
// All three tagref forms are accepted on INPUT (a value hand-typed before this editor existed must
// still load) — but `serializeExpr` always OUTPUTS the canonical `tag("name")` form, so generated text
// is unambiguous. Round-trip is SEMANTIC, not byte-exact: original spacing/redundant parens/which
// tagref form was used are not preserved, only the evaluated meaning.
//
// The DSL's own lexer has NO escape mechanism for `"` inside a tag name (`parse_string_body` scans to
// the next raw `"`, no backslash handling) — a tag name containing `"` cannot be represented at all.
// Callers building tag names from user input (ExprTreeEditor.tsx) must keep `"` out at the source.
//
// FUNCTIONS (020 §6.2/§6.3, mirroring expr_rule_node.hpp's parse_function_call kUnary/kBinary tables
// exactly, including arity — a unary function's arg count and a binary function's are both enforced
// hard, not just conventional): unary — abs, floor, ceil, round, sqrt, sin, cos, tan, asin, acos, atan,
// ln, log, exp; binary — pow(base, exp), min(a, b), max(a, b). An IDENT immediately followed by '('
// that isn't `tag` or a name in one of these two tables is a parse error ("unknown function"), same
// posture the DSL's own parser takes — never a silent misparse into something else.

export type BinOp = "+" | "-" | "*" | "/" | "%" | "<" | ">" | "<=" | ">=" | "==" | "!=" | "&&" | "||";
export type UnaryOp = "neg" | "not";

export const UNARY_FUNCS = [
  "abs", "floor", "ceil", "round", "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "ln", "log", "exp",
] as const;
export const BINARY_FUNCS = ["pow", "min", "max"] as const;
export type UnaryFuncName = (typeof UNARY_FUNCS)[number];
export type BinaryFuncName = (typeof BINARY_FUNCS)[number];
export type FuncName = UnaryFuncName | BinaryFuncName;

export type ExprNode =
  | { kind: "num"; value: number }
  | { kind: "tag"; name: string }
  | { kind: "unary"; op: UnaryOp; arg: ExprNode }
  | { kind: "binary"; op: BinOp; left: ExprNode; right: ExprNode }
  | { kind: "call"; name: FuncName; args: ExprNode[] };

export type ParseResult = { ok: true; tree: ExprNode } | { ok: false; error: string };

export type Shape = "reporter" | "boolean";

const BOOLEAN_BIN_OPS = new Set<BinOp>(["<", ">", "<=", ">=", "==", "!=", "&&", "||"]);

// Reporter (produces a number) vs Boolean (produces true/false) — the doc's §2/§3 shape taxonomy,
// derived structurally from the node's own operator, never stored redundantly on the node itself.
// Every function (020 §6.2/§6.3) is numeric — no function produces a boolean.
export function shapeOf(node: ExprNode): Shape {
  switch (node.kind) {
    case "num":
    case "tag":
    case "call":
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
  "%": 6,
};

const UNARY_FUNC_SET = new Set<string>(UNARY_FUNCS);
const BINARY_FUNC_SET = new Set<string>(BINARY_FUNCS);

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
      else if (this.match1("%")) left = { kind: "binary", op: "%", left, right: this.parseUnary() };
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
    this.skipWs();
    if (this.peek() === "(") return this.parseFunctionCall(ident);
    // A bare identifier IS a tag reference (e.g. `raw`).
    return { kind: "tag", name: ident };
  }
  // 020 §6.2/§6.3: IDENT '(' expr (',' expr)? ')' — `ident` reached here is anything but `tag`
  // (already handled above); a name matching neither table is a parse error, mirroring
  // expr_rule_node.hpp's parse_function_call exactly, arity included.
  private parseFunctionCall(name: string): ExprNode {
    const isUnary = UNARY_FUNC_SET.has(name);
    const isBinary = !isUnary && BINARY_FUNC_SET.has(name);
    if (!isUnary && !isBinary) throw new ParseError(`unknown function '${name}'`);
    if (!this.match1("(")) throw new ParseError(`expected '(' after '${name}'`);
    const args = [this.parseOr()];
    if (isBinary) {
      this.skipWs();
      if (!this.match1(",")) throw new ParseError(`function '${name}' requires 2 arguments`);
      args.push(this.parseOr());
    }
    this.skipWs();
    if (!this.match1(")")) throw new ParseError(`expected ')' closing '${name}(...)'`);
    return { kind: "call", name: name as FuncName, args };
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
    case "call":
      // A call is self-delimiting (its own parens), so its args never need extra wrapping — same
      // reasoning needsParensAsChild already applies to num/tag/unary children.
      return `${node.name}(${node.args.map(serializeExpr).join(", ")})`;
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
    case "call":
      return node.args.flatMap(collectTagRefs);
  }
}

export function getChildren(node: ExprNode): ExprNode[] {
  if (node.kind === "unary") return [node.arg];
  if (node.kind === "binary") return [node.left, node.right];
  if (node.kind === "call") return node.args;
  return [];
}

export function withChild(node: ExprNode, index: number, child: ExprNode): ExprNode {
  if (node.kind === "unary") return { ...node, arg: child };
  if (node.kind === "binary") return index === 0 ? { ...node, left: child } : { ...node, right: child };
  if (node.kind === "call") return { ...node, args: node.args.map((a, i) => (i === index ? child : a)) };
  return node;
}

export function defaultNodeFor(shape: Shape): ExprNode {
  if (shape === "reporter") return { kind: "num", value: 0 };
  return { kind: "binary", op: "==", left: { kind: "num", value: 0 }, right: { kind: "num", value: 0 } };
}

export function kindIdOf(node: ExprNode): string {
  if (node.kind === "unary") return node.op;
  if (node.kind === "binary") return node.op;
  if (node.kind === "call") return node.name;
  return node.kind; // "num" | "tag"
}

// Groups the kind-select dropdown into `<optgroup>`s (ExprTreeEditor.tsx) — purely presentational,
// the palette more than doubled once the 020 §6.2/§6.3 math functions joined the original 4 categories'
// worth of operators, and a single flat 30+ option list stopped being scannable.
export type BlockCategory = "Value" | "Arithmetic" | "Comparison" | "Logic" | "Math function";

export interface BlockKindDef {
  id: string;
  label: string;
  shape: Shape;
  category: BlockCategory;
  childShapes: Shape[];
  make: () => ExprNode;
}

const binaryKind = (
  op: BinOp, label: string, shape: Shape, category: BlockCategory, childShape: Shape,
): BlockKindDef => ({
  id: op,
  label,
  shape,
  category,
  childShapes: [childShape, childShape],
  make: () => ({ kind: "binary", op, left: defaultNodeFor(childShape), right: defaultNodeFor(childShape) }),
});

const unaryFuncKind = (name: UnaryFuncName, label: string): BlockKindDef => ({
  id: name,
  label,
  shape: "reporter",
  category: "Math function",
  childShapes: ["reporter"],
  make: () => ({ kind: "call", name, args: [defaultNodeFor("reporter")] }),
});

const binaryFuncKind = (name: BinaryFuncName, label: string): BlockKindDef => ({
  id: name,
  label,
  shape: "reporter",
  category: "Math function",
  childShapes: ["reporter", "reporter"],
  make: () => ({ kind: "call", name, args: [defaultNodeFor("reporter"), defaultNodeFor("reporter")] }),
});

// The full palette an "add block" menu can offer — one entry per concrete block kind. A menu filters
// this list by the target socket's required shape (`blockKindsFor`), so an incompatible block can
// never even appear as an option — the doc's "shape is the type check" idea enforced at menu time
// rather than by drag-drop rejection (020 §5's chosen v1 interaction model).
export const BLOCK_KINDS: BlockKindDef[] = [
  { id: "num", label: "Number", shape: "reporter", category: "Value", childShapes: [],
    make: () => ({ kind: "num", value: 0 }) },
  {
    id: "tag",
    label: "Tag reference",
    shape: "reporter",
    category: "Value",
    childShapes: [],
    make: () => ({ kind: "tag", name: "raw" }),
  },
  {
    id: "neg",
    label: "Negate (−x)",
    shape: "reporter",
    category: "Arithmetic",
    childShapes: ["reporter"],
    make: () => ({ kind: "unary", op: "neg", arg: defaultNodeFor("reporter") }),
  },
  {
    id: "not",
    label: "Not (!x)",
    shape: "boolean",
    category: "Logic",
    childShapes: ["boolean"],
    make: () => ({ kind: "unary", op: "not", arg: defaultNodeFor("boolean") }),
  },
  binaryKind("+", "Add (+)", "reporter", "Arithmetic", "reporter"),
  binaryKind("-", "Subtract (−)", "reporter", "Arithmetic", "reporter"),
  binaryKind("*", "Multiply (×)", "reporter", "Arithmetic", "reporter"),
  binaryKind("/", "Divide (÷)", "reporter", "Arithmetic", "reporter"),
  binaryKind("%", "Modulo (%)", "reporter", "Arithmetic", "reporter"),
  binaryKind("<", "Less than (<)", "boolean", "Comparison", "reporter"),
  binaryKind(">", "Greater than (>)", "boolean", "Comparison", "reporter"),
  binaryKind("<=", "Less or equal (≤)", "boolean", "Comparison", "reporter"),
  binaryKind(">=", "Greater or equal (≥)", "boolean", "Comparison", "reporter"),
  binaryKind("==", "Equal (=)", "boolean", "Comparison", "reporter"),
  binaryKind("!=", "Not equal (≠)", "boolean", "Comparison", "reporter"),
  binaryKind("&&", "And", "boolean", "Logic", "boolean"),
  binaryKind("||", "Or", "boolean", "Logic", "boolean"),
  unaryFuncKind("abs", "Absolute value"),
  unaryFuncKind("floor", "Floor"),
  unaryFuncKind("ceil", "Ceiling"),
  unaryFuncKind("round", "Round"),
  unaryFuncKind("sqrt", "Square root"),
  unaryFuncKind("sin", "Sine"),
  unaryFuncKind("cos", "Cosine"),
  unaryFuncKind("tan", "Tangent"),
  unaryFuncKind("asin", "Arcsine"),
  unaryFuncKind("acos", "Arccosine"),
  unaryFuncKind("atan", "Arctangent"),
  unaryFuncKind("ln", "Natural log (ln)"),
  unaryFuncKind("log", "Log base 10"),
  unaryFuncKind("exp", "Exponential (eˣ)"),
  binaryFuncKind("pow", "Power (base, exp)"),
  binaryFuncKind("min", "Minimum (a, b)"),
  binaryFuncKind("max", "Maximum (a, b)"),
];

export function blockKindsFor(shape: Shape | "any"): BlockKindDef[] {
  return shape === "any" ? BLOCK_KINDS : BLOCK_KINDS.filter((k) => k.shape === shape);
}
