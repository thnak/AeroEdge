// A visual Reporter/Boolean value-block editor for `expr` DSL fields (020 §5) — same Tier-2 controlled
// contract as ModbusRegisterMap.tsx: { value: string; onChange: (v: string) => void }, plus one extra
// prop (`knownTags`) for a tag-name autocomplete list (FlowDesigner.tsx computes it — no server-side
// tag metadata exists to source it from; see exprAst.ts's header comment).
//
// Interaction model: click/menu-based, not drag-and-drop. `ExprNode` (exprAst.ts) is always a
// COMPLETE, valid tree — the DSL grammar has no empty production, so there is no separate "empty
// socket" state to model. Every occupied node instead shows a kind SELECT filtered to that socket's
// required shape (Reporter or Boolean), so an incompatible block can never even be chosen — the doc's
// "shape is the type check" idea enforced at menu time. Picking a different kind replaces that whole
// subtree with the new kind's default children (a deliberate v1 simplification: no child-preserving
// migration when an operator's arity/shape changes, e.g. swapping "+" for "not").
import { useMemo, useState } from "react";
import { Button } from "../components";
import {
  parseExpr,
  serializeExpr,
  shapeOf,
  kindIdOf,
  getChildren,
  withChild,
  defaultNodeFor,
  blockKindsFor,
  BLOCK_KINDS,
  type ExprNode,
  type Shape,
} from "../exprAst";

const KNOWN_TAGS_LIST_ID = "expr-known-tags";

function Block({ shape, node, onChange }: {
  shape: Shape | "any";
  node: ExprNode;
  onChange: (n: ExprNode) => void;
}) {
  const kindId = kindIdOf(node);
  const options = blockKindsFor(shape);
  const current = BLOCK_KINDS.find((k) => k.id === kindId);
  const blockShape = shapeOf(node);
  const children = getChildren(node);

  const changeKind = (id: string) => {
    if (current && id === current.id) return;
    const next = BLOCK_KINDS.find((k) => k.id === id);
    if (next) onChange(next.make());
  };

  // Grouped by category (exprAst.ts's BlockCategory) — the palette more than doubled once the math
  // functions joined the original operators, and a flat 30+ option list stopped being scannable.
  const groups = new Map<string, typeof options>();
  for (const k of options) {
    if (!groups.has(k.category)) groups.set(k.category, []);
    groups.get(k.category)!.push(k);
  }

  return (
    <span className={`expr-block expr-block-${blockShape}`}>
      <select
        className="expr-kind-select"
        value={current?.id ?? ""}
        onChange={(e) => changeKind(e.target.value)}
        aria-label="Block kind"
      >
        {[...groups].map(([category, kinds]) => (
          <optgroup key={category} label={category}>
            {kinds.map((k) => (
              <option key={k.id} value={k.id}>{k.label}</option>
            ))}
          </optgroup>
        ))}
      </select>
      {node.kind === "num" && (
        <input
          type="number"
          className="expr-leaf-input"
          value={node.value}
          onChange={(e) => onChange({ kind: "num", value: e.target.value === "" ? 0 : Number(e.target.value) })}
        />
      )}
      {node.kind === "tag" && (
        <input
          list={KNOWN_TAGS_LIST_ID}
          className="expr-leaf-input"
          value={node.name}
          onChange={(e) => onChange({ kind: "tag", name: e.target.value.replace(/"/g, "") })}
        />
      )}
      {children.length > 0 && current && (
        <span className="expr-block-children">
          {children.map((child, i) => (
            <Block
              key={i}
              shape={current.childShapes[i]}
              node={child}
              onChange={(c) => onChange(withChild(node, i, c))}
            />
          ))}
        </span>
      )}
    </span>
  );
}

export function ExprTreeEditor({ value, onChange, knownTags }: {
  value: string;
  onChange: (expr: string) => void;
  knownTags: string[];
}) {
  const parsed = useMemo(() => parseExpr(value), [value]);
  const [rawMode, setRawMode] = useState(false);

  if (!parsed.ok || rawMode) {
    return (
      <div className="expr-raw">
        {!parsed.ok && value !== "" && <p className="field-error">Can&rsquo;t show as blocks: {parsed.error}</p>}
        <textarea className="expr-raw-input" rows={2} value={value} onChange={(e) => onChange(e.target.value)} />
        <div className="expr-raw-actions">
          {parsed.ok && <Button onClick={() => setRawMode(false)}>Back to blocks</Button>}
          <Button onClick={() => onChange(serializeExpr(defaultNodeFor("boolean")))}>
            Start from a blank expression
          </Button>
        </div>
      </div>
    );
  }

  return (
    <div className="expr-tree">
      <Block shape="any" node={parsed.tree} onChange={(tree) => onChange(serializeExpr(tree))} />
      <datalist id={KNOWN_TAGS_LIST_ID}>
        {knownTags.map((t) => <option key={t} value={t} />)}
      </datalist>
      <div className="expr-tree-actions">
        <Button onClick={() => setRawMode(true)}>Edit as text</Button>
      </div>
    </div>
  );
}
