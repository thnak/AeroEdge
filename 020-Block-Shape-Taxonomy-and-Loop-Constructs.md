# 020 — Block Shape Taxonomy (Blockly-grounded) and Loop Constructs

## 1. Where this starts from

019's canvas slice shipped real jigsaw shapes (hat/body/body-branch — see
`019-Flow-Graph-Model-and-Studio-Canvas-API.md` §4 and the `studio/jigsaw-node-shapes` PR), but they're
still connected by drawn lines, react-flow-style. The follow-up discussion landed on something more
specific: Blockly/Scratch's actual mechanic isn't just puzzle-shaped art — it's a **connection-type
system**, where the shape *is* the type check, and lines only ever appear for the one case shape can't
express (a jump to something not adjacent — the classic ANSI/ISO-5807 "off-page connector" symbol, used
in pairs, is the closest real precedent for that case).

This doc grounds AeroEdge's block taxonomy in Blockly's actual documented model (not the informal
Scratch-wiki shape names alone), maps every current node type onto it, and — this is the important
part — is explicit about which pieces are **just a shape change** (buildable now, no backend work) vs.
which pieces need **new runtime capability that does not exist today** (loops; nested value
expressions). Conflating those two would misrepresent what's actually available.

Sources: [Blockly — Anatomy of a block](https://docs.blockly.com/guides/create-custom-blocks/define/block-anatomy/),
[Blockly — Connection checks](https://developers.google.com/blockly/guides/create-custom-blocks/inputs/connection-checks),
[Scratch Wiki — Blocks](https://en.scratch-wiki.info/wiki/Blocks).

## 2. The four connection primitives (Blockly's real vocabulary)

| Connection | Shape | Meaning |
|---|---|---|
| **Previous** | a notch on top | "I can follow something" |
| **Next** | a tab on bottom | "Something can follow me" |
| **Output** | a jigsaw nub on the side | "I produce a value" |
| **Input** | a socket | "I accept a value here" |

Compatibility is purely structural: **Previous↔Next** connect (vertical stacking — this is what 019's
jigsaw shapes already are), **Output↔Input** connect (a value nested inside another block's slot —
AeroEdge has **none of this today**, see §5).

## 3. Block shape catalog

| Shape | Connections | Scratch examples | AeroEdge status |
|---|---|---|---|
| **Hat** | Next only (no Previous — nothing can precede it) | "when green flag clicked" | **Shipped** (019 jigsaw slice) — `NodeCategory::Source` |
| **Stack** | Previous + Next | "move () steps", "say () for () seconds" | **Shipped** — `Transform`/`Rule`/`Output` (single-tab body piece) |
| **Cap** | Previous only (no Next — nothing can follow) | "stop all", "delete this clone" | **Not distinguished yet** — see §4.3 |
| **C-block** | Previous + Next, PLUS one or more nested statement-input cavities (each cavity is itself a mini Previous/Next stack) | "repeat () { }", "if () then { }" | **Partially shipped, wrong shape** — see §4.2 |
| **Reporter** | Output only, rounded/oval | "(x position)", "(answer)" | **Not built** — see §5 |
| **Boolean** | Output only, hexagon | "touching ()?", "() > ()" | **Not built** — see §5 |

## 4. Mapping today's catalog onto these shapes

### 4.1 Straightforward (already correct, no change needed)

| type_id | Category | Shape |
|---|---|---|
| `aero.source.decode`, `.json`, `.modbus`, `.modbus_bits`, `.mes_order` | Source | Hat |
| `aero.transform.scale`, `.moving_average`, `.mean`, `.minmax`, `.sum`, `.crc` | Transform | Stack |
| `aero.rule.expr` | Rule | Stack (see note) |
| `aero.output.sum`, `.mes`, `.http` | Output | Stack (see §4.3) |

Note on `aero.rule.expr`: it can `NodeResult::Stop` the flow at runtime on a match, but that's a
**runtime behavior**, not a **connectivity shape** — the compiled step array is unchanged either way,
execution just short-circuits. Shape encodes connectivity only; giving it a Cap shape would be
misleading (a Stop is conditional, a Cap's "nothing follows" is unconditional/structural).

### 4.2 `aero.flow.switch` should be a C-block, not two dangling tabs (proposed, not shipped)

The shipped jigsaw shape (019 follow-up PR) gives switch a body piece with **two bottom tabs**
(true/false), each connected by a drawn line to wherever the branch's first node happens to sit on the
canvas. That's exactly the "near-puzzle-piece, still uses lines" version this doc's discussion started
from.

The better shape, matching Blockly's own `if/else` block almost exactly: switch becomes a **C-block
with two nested cavities** — a true-branch stack and a false-branch stack, nested INSIDE the switch
block's own body, physically snapped, no line at all for the common case where a branch is short and
local. A line (the off-page-connector "stick") is only needed if a branch needs to **rejoin** a shared
downstream node outside the cavity (our merge case — e.g. both branches feeding one `Sum`) — nesting
can't express "and then both paths continue to the same place," so that specific edge stays a drawn
stub, exactly the case flowcharting's off-page connector was invented for.

```
┌─────────────────────────┐
│ Rule: Switch             │
│ expr: raw > 100           │
├─── true ─────────────────┤
│  ┌─────────────────────┐ │
│  │ Transform: Scale x10 │ │   <- nested cavity, snapped, no line
│  └─────────────────────┘ │
├─── false ─────────────────┤
│  ┌─────────────────────┐ │
│  │ Transform: Scale x1  │ │   <- nested cavity, snapped, no line
│  └─────────────────────┘ │
└──────────┬────────────────┘
           ┊  (stub — both branches rejoin here; can't be nesting)
      ┌────▼─────┐
      │ Output:Sum│
      └───────────┘
```

**Status: designed here, not implemented.** This is a real UI/interaction rewrite (the canvas needs
drop-into-cavity detection, not just drop-near-handle), but needs **no backend change** — the existing
`edges[]` + `from_port` model already expresses exactly this (a nested cavity is just an edge with
`from_port` set, rendered differently; the stub is an edge with no `from_port`). Worth doing before the
loop/value work below, since it's schema-compatible with what's already shipped.

### 4.3 Output: Stack vs. Cap is a real distinction we don't track yet

Today every `Output` node is a Stack shape (has a next tab) because `aero.output.mes` legitimately
needs to stay mid-chain (stage a report, then keep going — a non-stopping side effect, 012 §4). But
`aero.output.sum` is, in every real flow so far, actually terminal — giving it a Cap shape (no next
tab) would be more honest AND would double as free validation (can't accidentally chain something
pointless after a terminal sum).

**Gap:** the catalog/`NodeDescriptor` has no "is this node type terminal" flag today — this would be a
small, additive schema change (`config_schema.hpp`, `NodeDescriptor`), not a big one, whenever it's
prioritized.

## 5. Reporter/Boolean value blocks — the free-text-DSL gap

`aero.rule.expr` and `aero.flow.switch` both take a single free-text `expr` config string (e.g.
`"raw > 100"`), parsed by `expr_detail::Program` (`expr_rule_node.hpp`) — a small non-Turing RPN
DSL (compare/boolean/arithmetic over tags). This is **not** a Reporter/Boolean block tree the way
Blockly does it (e.g. dragging a `>` hexagon block with two oval number-reporter blocks plugged into
its two sockets, instead of typing text).

Building real Reporter/Boolean blocks would need, net new:
- An **Output/Input connection type** in the Studio canvas (today only Previous/Next exists — see §2).
- A **block-tree ⇄ DSL-string compiler** in both directions (canvas tree → `expr_detail::Program`
  source text for `toApplication`, and back for loading an existing flow).
- New block shapes for the DSL's actual primitives: tag reference (Reporter), literal number
  (Reporter), comparison/boolean ops (Boolean).

**Status: not scoped, not built.** This is a genuinely separate, sizable project — flagged here so it's
not confused with the shape work in §3/§4, which is comparatively cheap.

## 6. The full operator set (grounded against Scratch's Operators category + what the DSL actually parses)

**Status: implemented** (`expr_rule_node.hpp`) — §6.1 `mod`, §6.2 math functions (`abs`/`floor`/`ceil`/
`round`/`sqrt`/`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`ln`/`log`/`exp`/`pow`), §6.3 `min`/`max`, and
§6.4's NaN diagnostic (`Program::evaluate()`'s `saw_nan` out-param — tracks NaN through every
instruction, not just the final result, so a comparison-shaped expression that "goes quiet" on NaN still
stages the `ExprNaN` diagnostic Event, not just a bare-arithmetic-shaped one that spuriously fires; a
final-result-only check would have missed the first case, since a comparison op always collapses NaN to
a clean 0.0/1.0). Covered by `tests/nodes/expr_rule.cpp`.

Cross-checking `expr_detail::Program`'s real grammar (`expr_rule_node.hpp`) against
[Scratch's Operators category](https://en.scratch-wiki.info/wiki/Operators_Blocks) (18 blocks) to see
what a "complete" operator set means for a NUMERIC, tag-only DSL — not a blind copy, since several
Scratch operators are string/list-typed and don't fit AeroEdge's data model (`Tag{name, value: double}`
— no string value type exists in `ProcessingContext` today).

| Scratch operator | In `expr_detail::Program` today? | Verdict |
|---|---|---|
| `+ - * /` | Yes (`Op::Add/Sub/Mul/Div`) | Have |
| `< > =` (we spell `==`) | Yes (`Op::Lt/Gt/Eq`, plus `<=`/`>=`/`!=` Scratch doesn't even have) | Have, and already a superset |
| `and` / `or` / `not` | Yes (`Op::And/Or/Not`) | Have |
| `() mod ()` | **No** | Easy add — one more binary `Op`, same shape as `Div` |
| `round ()`, `() of ()` (abs/floor/ceiling/sqrt/etc.) | **No** | Real gap for a telemetry DSL (e.g. `abs(delta) > threshold`) — needs a function-call grammar production (`IDENT '(' expr ')'`), a bit more than a new binary op but still non-Turing (pure functions, no state) |
| `pick random () to ()` | **No** | **Deliberately excluded, not just missing** — a rule engine for industrial edge control should be deterministic given the same tags; randomness in a threshold/switch expression would make flows non-reproducible and hard to debug. Skip unless a real use case overrides this. |
| `join`, `letter () of ()`, `length of ()`, `() contains ()?` | **No** | Not applicable — these are string operators; `Tag.value` is `double`, there is no string value type to operate on. Out of scope unless the tag data model itself grows a string type (a much bigger, unrelated change). |

Every added operator here is a candidate **Boolean or Reporter block shape** once §5's visual editor
exists — this section is about the DSL's *vocabulary*, §5 is about *authoring it by dragging shapes
instead of typing*. They're independent: the vocabulary can grow (this section) whether or not the
visual editor (§5) ever gets built, since it's still usable as free text in the meantime (exactly how
`aero.rule.expr`/`aero.flow.switch` work today).

### 6.1 `mod` — one new binary op, zero struct changes

Grammar: add `%` at the same precedence level as `*`/`/` (`parse_mul`'s alternation gets a third
option). Syntax is `%` (not the `mod` keyword) to stay consistent with the DSL's existing C-style
operator spelling (`&&`/`||`/`==`/`!=` are all C-style already, and a `mod` keyword would need special
lexer handling to not collide with the "bare identifier = tag reference" rule — `%` avoids that
entirely, for free).

Semantics: `std::fmod(a, b)`, guarded the same way `Div` already is (`b == 0.0 → 0.0`, "no trap,
E-safe" per that line's own comment). Worth documenting explicitly in the `FieldSpec.help` text once
built: `fmod`'s result takes the sign of `a` (`-5 % 3` = `-2`, not `1` the way Python's `%` would give)
— a real, silent surprise for anyone porting a Python-flavored mental model into this DSL.

New `Op::Mod` slots into the **existing binary-op switch branch** (`default:` case in `evaluate()`,
pop 2/push 1) exactly like `Add`/`Sub`/`Mul`/`Div` already do — no `Instr` struct change, no new field.

### 6.2 Math functions — grounded in Scratch's real `() of ()` block, not guessed

[Scratch's `() of ()` operator](https://en.scratch-wiki.info/wiki/Operators_Blocks) is a single
reporter with a function-name dropdown: **abs, floor, ceiling, sqrt, sin, cos, tan, asin, acos, atan,
ln, log, e^, 10^** — 14 functions, all unary (one operand). That's the real, grounded set; anything
beyond it (see `min`/`max` below) is a separate proposal on top, not a Scratch citation.

**Grammar** — one new production, `IDENT '(' expr ')'`, unified with the parser's existing special
case for `tag("...")` (which is already exactly this shape — an identifier followed by `(`).
`parse_ident` today hardcodes the `tag` name; generalize it to: scan the identifier, peek for `(`, and
dispatch by name — `tag` keeps its own handling (its argument is a quoted string literal, not a nested
expression — genuinely different from every math function here), each math function name recurses into
one nested `expr` and emits its `Op`. An identifier followed by `(` that matches no known name is a
**parse error** ("unknown function 'sqtr'"), not a silent misparse — a real defensive-UX win over
today (nothing currently distinguishes a typo'd function name from anything else).

**Naming choices** (matching the most common convention across C/Python/JS math libraries over
Scratch's UI-dropdown label where they diverge — the DSL is typed text, not a dropdown, so familiarity
to whoever's typing wins):
- `ceil`, not Scratch's UI label "ceiling" — every mainstream math library spells it `ceil`.
- `ln` (natural log) and `log` (base-10) — kept as Scratch spells them; this is worth keeping exactly
  as-is specifically BECAUSE "log" is genuinely ambiguous across languages (natural log in some
  contexts, base-10 in others) — inheriting Scratch's already-disambiguated two names sidesteps
  re-litigating that ambiguity ourselves.
- `round` already exists as a bare Scratch block (not inside `() of ()`) — folds into the same
  function-call production here, no special casing needed.

**`e^(x)` / `10^(x)` — decided: general `pow(base, exponent)` + `exp(x)`.** One binary `pow` subsumes
both Scratch cases (`pow(10, x)` for the "10^" block; `exp(x)` kept as its own convenience function
for eˣ specifically, matching `std::exp`, since e^x is common enough on its own — e.g. exponential
decay/growth) and is fully general beyond them (`pow(tag("a"), tag("b"))`). This is the more idiomatic
design for a typed DSL over mirroring Scratch's two fixed-base blocks literally, and it's the design
that requires comma-argument-list parsing (`IDENT '(' expr ',' expr ')'`) — which `min`/`max` (§6.3)
reuse, so they're close to free on top of this decision.

### 6.3 `min`/`max` — decided: keep, on the strength of §6.2's `pow` decision

Flagging plainly (unchanged from the previous round): Scratch's operators do **not** include `min`/`max`
at all (checked directly against the real block list in §6.2 — no min/max block exists in Scratch).
These were **my own addition**, not grounded in the same primary source the rest of this section
cites — motivated by a plausible telemetry need (clamping — "cap this tag at X"). Since `pow` (§6.2)
already needs comma-separated argument-list parsing, `min(a, b)`/`max(a, b)` reuse that same grammar
machinery essentially for free, which is why they're kept rather than deferred.

### 6.4 Domain guards — decided: let NaN propagate (IEEE754 default), a deliberate split from `Div`

`sqrt` of a negative number, `ln`/`log` of a non-positive number, `asin`/`acos` outside `[-1, 1]` will
produce `NaN` via `<cmath>`'s default behavior, and **that's the chosen behavior, not an oversight** —
overruling this doc's earlier recommendation to guard-to-`0.0` like `Div`.

**Correction from adversarial review — the consequence described below was wrong, and materially
understated the real risk.** The original text here said a NaN result makes a rule "go quiet rather
than erroring loudly." That's only true when the NaN feeds an *ordered comparison* (`>`, `<`, `<=`,
`>=`, `==`, all `false` for NaN per IEEE754). But `ExprRuleNode`/`SwitchNode` don't fire on a
comparison result specifically — they fire on **`v != 0.0`** (`expr_rule_node.hpp`'s `process()`,
mirrored by `switch_node.hpp`). `NaN != 0.0` is **`true`** (`!=` is the one comparison IEEE754 makes
true for NaN, since it's the negation of `==`). So the real behavior has two opposite failure modes
depending on incidental expression shape, not one:
- A **comparison-shaped** top-level expression (`sqrt(tag("x")) > 5` — the pattern every example in
  this doc uses) → out-of-domain input makes the inner comparison `0.0` → the rule silently **doesn't
  fire**. This is the case the original text described.
- A **bare-arithmetic-shaped** top-level expression (`sqrt(tag("x")) - offset` — an equally legal,
  unremarkable way to author `aero.rule.expr`) → the same NaN makes `v != 0.0` **true** → the rule
  **spuriously fires**, raising a real alarm/routing a real branch from garbage input.

In an alarm/routing engine, "an alarm that should fire silently doesn't" and "an alarm that shouldn't
fire spuriously does" are both bad, but they're not the same risk, and an author reasoning from the
original "goes quiet" framing would only be defending against the first one. **This needs to be stated
plainly wherever this decision is documented going forward** (this paragraph is the fix), and the
"any test/debug tooling can specifically check for it" claim two paragraphs below was asserted, not
designed — a concrete, cheap answer is: add a `std::isnan(v)` check inside `process()` (already touches
`v`, 0-alloc) that stages a diagnostic `Event` when a rule/switch actually evaluates NaN at runtime,
so there's at least an operational signal for "this fired/didn't-fire because of NaN," not silence
either way. This wasn't in the original design and should be treated as a real requirement, not a
nice-to-have, before this decision ships.

This means the DSL now has **two different philosophies for two different classes of edge case**, and
that split is intentional, not inconsistent-by-accident:
- **`Div`'s divide-by-zero stays guarded to `0.0`** — dividing by zero is a routine, expected edge case
  in threshold-style rules (e.g. a rate computed over an elapsed-time tag that can legitimately be
  zero), and "no trap" was chosen there deliberately (that line's own comment: "no trap (E-safe)").
- **The new domain-restricted functions propagate `NaN`** — an out-of-domain input to `sqrt`/`ln`/
  `asin`/`acos` is closer to a genuine configuration mistake than a routine runtime edge case, and
  surfacing it (even silently-via-NaN, which any test/debug tooling can specifically check for) is more
  useful than papering over it with a guard that would make a broken expression *look* like it's
  working.

If this split ever proves confusing in practice, the fix is narrow (guard specific functions
individually) rather than a reason to revisit `Div`'s own established behavior.

### 6.5 What doesn't change

`Instr { Op op; double k; std::uint32_t tag; }` needs **zero new fields** for any of §6.1/6.2/6.3 — every
new unary function slots into the existing unary switch branch (`Neg`/`Not`'s pattern: pop 1, push 1,
`stack[sp-1] = f(stack[sp-1])`), every new binary op slots into the existing binary branch (`Add`
et al.'s pattern: pop 2, push 1). `max_depth`-based stack sizing, the 0-alloc eval loop, and the
parse-once/eval-many split (N1/N3) are all unaffected — this is purely more `Op` enum values plus more
`case`s in one `switch`, the same shape of change the codebase already made when `aero.flow.switch` was
added on top of the same `Program` (reused wholesale, zero duplication).

## 7. Variables/Data category — `set` (buildable now, no new runtime capability)

**Status: implemented** (`set_node.hpp`, registered as `aero.transform.set`) — §7.1 overwrite-in-place/
self-reference, §7.2 deploy-time expr validation, §7.5's `validate_tag_writers` compiler pass (rejects
non-mutually-exclusive same-tag writers; allows exactly the same-switch-opposite-label case, and a
single node's own self-reference since that's one writer, not two). §7.3's `change-by` Tier-2 UI helper
is NOT built (Studio-side, out of scope for this slice — the DSL already supports the pattern as plain
text today). Covered by `tests/core/flow_graph.cpp` (tag-writer collision/accept cases + SetNode
overwrite/self-reference) and `tests/runtime/catalog.cpp`.

This is the one addition in this whole document that's cheap in every dimension — unlike §5 (needs a
new connection type + compiler) and §6-below/loops (needs a new execution model), a `set` node needs
**neither**. It reuses machinery that already exists and already shipped.

Scratch's [Variables category](https://en.scratch-wiki.info/wiki/Variables_Blocks) has `set [var] to
()` and `change [var] by ()`, both Stack-shaped with one Reporter-typed input socket. AeroEdge's
`ctx.tags` (`ProcessingContext::tags`, `sdk/processing_context.hpp`) is exactly Scratch's flat
named-variable store — a `Tag{name, value}` list — so the mapping is direct:

- **New node, `aero.transform.set`** (`NodeCategory::Transform`): config `{tag: string, expr: string}`.
  `process()` calls `expr_detail::Program::evaluate(ctx)` (the *exact* evaluator
  `aero.rule.expr`/`aero.flow.switch` already call — `Program::evaluate` was deliberately made public
  on `Program` itself, not private to `ExprRuleNode`, precisely so a third caller could reuse it without
  duplicating the interpreter, per that file's own comment) and writes the result under `tag`. Shape:
  **Stack** (Previous+Next, one generic body tab) — it's an action, not a value producer, matching
  Scratch's `set` being Stack-shaped too, not Reporter-shaped.

### 7.1 Overwrite-in-place, not append — a real correctness decision, not a style choice

`ctx.tags` is a flat `std::vector<Tag>`, and every lookup (`Program::tag_value`) does a **linear scan,
first match wins**. If `set` just `push_back`'d a fresh `Tag{tag, result}` every call, and a tag of that
name already existed earlier in the vector, every subsequent `tag("X")` read would keep returning the
*old* value forever — the new one would be pushed to the end, past where the scan already stopped. So
`process()` must search first:

```cpp
NodeResult process(ProcessingContext& ctx) noexcept override {
    if (!prog_.ok) return NodeResult::Error;
    const double v = prog_.evaluate(ctx);          // reads the OLD value if self-referential
    for (auto& t : ctx.tags) {
        if (t.name == tag_) { t.value = v; return NodeResult::Continue; }
    }
    ctx.tags.push_back(Tag{tag_, v});               // first write for this name
    return NodeResult::Continue;
}
```

This linear scan is `O(tags.size())`, not `O(1)` — a deliberate acceptance of the *same* tradeoff
`tag_value()` itself already makes (comment there: "missing tag reads as 0.0", same first-match linear
search), not a new performance concern this design introduces. It stays fully 0-alloc (N1) either way —
`ctx.tags` is pre-`reserve()`d and never reallocates on the steady path regardless of whether this call
hits the search branch or the (rarer, first-time) push_back branch.

**Self-reference "just works" from evaluation order, not special-casing:** `evaluate(ctx)` runs
*before* the search-and-overwrite loop, so `expr = "tag(\"x\") * 1.05"` with `tag = "x"` correctly
reads the pre-update value — this is exactly the read-old-write-new semantics `change by` (below)
depends on, and it falls out of the code shape above for free, not from any extra logic.

`tag_` itself is a `std::string_view` into the node's own `std::string tag` config field — same
pattern `StagedHttpRequest`'s `url`/`method` already use ("views into the node's own config storage,
stable, 0-alloc to stage" per that struct's comment) — safe because the node is pinned in the
`CompiledPlan` for the whole flow's lifetime (I6).

### 7.2 Deploy-time validation parity — don't let this be the one node that skips it

`aero.rule.expr` and `aero.flow.switch` both compile their `expr` at **deploy time**
(`flow_compiler.hpp`'s `validate_node_config()`), so a malformed expression is rejected before deploy,
never discovered as a runtime `NodeResult::Error`. `aero.transform.set`'s `expr` needs the exact same
`validate_node_config()` case — easy to build correctly by copying the switch-node case, easy to
forget entirely (there's no structural reason the compiler would catch the omission; it would just
silently behave differently from its two DSL-consuming siblings). Flagging explicitly so whoever
implements this doesn't have to rediscover it.

### 7.3 `change [tag] by (expr)` — UI sugar over the same node, with a concrete Tier-2 shape

Not a second node type: the Studio can offer a friendlier config-form affordance that writes
`expr = "tag(\"<tag>\") + <delta>"` into the same `aero.transform.set` node, since the DSL already
supports referencing the tag being written. Concretely, this fits the **existing `tier2_hint` field
convention** (`FieldSpec.tier2_hint` on the C++ side, serialized as `tier2` in the JSON the Studio
reads) — a new `tier2: "set-expr-helper"` micro-frontend that offers a plain "increment by (number)"
input alongside the raw `expr` text box, writing into the same underlying string either way.

**Correction from adversarial review — the "already branches on 3 values" claim was wrong.**
`opcua-security` and `http-headers` exist as `tier2_hint` values on the C++ side and show up in Studio
test fixtures, but `ConfigForm.tsx` today has exactly **one** real `tier2` branch
(`modbus-register-map`) — the other two currently fall through to a plain generic input, no dedicated
UI built yet. So a `set-expr-helper` panel would be the **second** real `tier2` UI, not a fourth —
still the right mechanism to reuse (the convention exists, the wiring pattern is proven once), just a
smaller existing footprint than originally claimed.

### 7.4 Where this lands in the codebase

New file `include/aero/nodes/set_node.hpp`, structurally a near-copy of `switch_node.hpp` (which
already establishes the "reuse `ExprRuleNode::Program` wholesale" pattern this depends on): a
`SetNode final : public INode` with `using Program = ExprRuleNode::Program;`, a `static Program
compile(std::string_view)` forwarding to it, `kFields` (`tag`/`expr`, both `FieldType::String`,
required), `kDesc{NodeCategory::Transform, "aero.transform.set", kFields}`, and registration in
`runtime.hpp`'s `register_builtins()` — the exact same five touch-points every prior node addition in
this codebase has used (kDesc/kFields → factory → `register_type` call → catalog test entry → runtime
registration), nothing novel about the wiring itself.
- **Ships with a plain-text `expr` field TODAY**, same Tier-1 `ConfigForm` pattern every existing node
  config field already uses — the fancy Reporter/Boolean drag-and-drop editor (§5) is a *strictly
  optional* later upgrade to how you author the same string, not a prerequisite for shipping `set`
  itself.
- **Lists are explicitly out of scope.** Scratch's Variables category also has list blocks (add/delete/
  insert/replace/length/contains — see the Variables Blocks source above). `ProcessingContext` has no
  list/array working-set type today (only scalar `Tag.value: double`); adding one would be a genuinely
  new data-model extension with its own design questions (bounded size for zero-alloc? per-Command
  lifetime? how does a list round-trip through the wire schema?) — not something to fold into this
  `set` proposal. Revisit only if a concrete use case needs it (today's fixed-size register-block
  decode nodes, e.g. `ModbusDecodeNode`, cover the "array of readings" case without a general list type).

### 7.5 Real gap found by adversarial review: `set` tag-name collisions have no deploy-time check

`ctx.tags` has zero namespacing, and both the read path (`Program::tag_value`) and `set`'s write path
(§7.1) resolve by first-match linear scan. Because `set` overwrites-in-place rather than appending,
whichever node — the original owner or a `set` node choosing the same name — writes a given tag name
**last in execution order** silently wins, with nothing in the schema expressing ownership of a tag
name at all. `validate_node_config` (the real deploy-time gate, `flow_compiler.hpp`) checks per-type
config shape (does `expr` parse, is `factor` present) but has and will have no cross-node check that a
`set` node's `tag` doesn't collide with a name some other node already produces — §7.2 only proposed
validating that `expr` *parses*, not that `tag` is collision-free.

Concretely who hits this: the whole premise of a low-code block editor is composing a flow from a
catalog without reading every upstream node's internals. Someone drags in `set`, picks a plausible
generic name (`value`, `result`, `rate`), and has no way to know a Source/Transform node earlier — or
later, since reordering the canvas can silently flip which write wins — already produces that name.
Nothing errors; the value is just quietly wrong downstream, and debugging it means manually walking the
whole node list checking who writes what name in what order.

**Resolved with the strong version, not a soft warning.** A warning that nobody is forced to look at is
barely better than silence — this needs a real deploy-time gate that actually understands the flow's
structure, not a name-scan that either over-rejects legitimate patterns or under-catches real bugs. A
blanket "reject any two nodes writing the same tag" is provably too strict on its own: two nodes on
**mutually exclusive switch branches** legitimately writing the same conceptual tag (e.g. both branches
of "if fast-path then `set result` else `set result`" converging into one downstream reader) is a real,
useful, correct pattern — only one of the two writers ever runs in a given Command, so there's no
actual collision at runtime. The bug this section is really about is **non-exclusive** collisions: two
writers that CAN both run in the same Command's execution, in an order the author doesn't control or
necessarily expect.

**New, dedicated compiler pass** (own function, own single responsibility — not folded into
`validate_node_config`'s per-node shape checks, not folded into `order_flow_graph`'s topology/cycle
logic; a third, focused pass, matching the "one object, one job" principle applied everywhere else in
this round of fixes):

```
validate_tag_writers(app, node_labels)   // node_labels: the required_label §3 already resolves per
                                          // node — this pass is free, it doesn't need new graph data
```

For every tag name, collect every node that writes it (Source nodes' fixed literal tags, Transform
nodes' fixed outputs, `set`'s configurable `tag`). For any two DIFFERENT writer nodes of the same tag
name, they're only legal together if their `required_label`s are **mutually exclusive** — both
non-empty, both attached to the *same* branch-producing node (§8.7's `branch_tracker`), and *different*
labels (`"true"` vs `"false"`). Any other combination — both unconditional, both on unrelated/unrelated-
label paths, or one unconditional and one conditional — is a **deploy-time rejection**, not a warning:
"nodes '<a>' and '<b>' both write tag '<name>' and are not mutually exclusive — this makes the tag's
value order-dependent." Self-reference (one node reading and writing its own tag — `change-by`) isn't
even a two-writer case under this model, so it needs no special-casing at all; it just never appears in
the "collect every writer" step as a *second* writer.

## 8. Loop blocks ("repeat () do", "count with X from () to () do") — needs new RUNTIME capability, not just a new shape

This is the one that needs the loudest flag. Blockly's `controls_for` (the "count with X from Y to Z
do" block) has: Previous + Next (stacks like anything else) + three value inputs (start/end/step,
Reporter-typed) + one nested statement cavity (the loop body, itself a Stack). **The shape is easy** —
it's just a C-block with extra value sockets, same primitives as §3/§5.

**The runtime cannot execute it.** AeroEdge's compile-once, zero-alloc execution invariant (I3, 004 §2)
compiles a flow into a **flat array**, walked in a **single pass** per Command
(`CompiledFlow::execute()`). There is no loop-iteration, no jump-back, no bounded-repeat semantics
anywhere in the runtime today — this is the exact same reason `validateGraph`'s cycle check rejects
*any* cycle a user draws (019 §4 follow-up: the `SelfConnectingEdge` component lets a user draw a
loop-back edge and see it, specifically so it's visible enough to hit that rejection cleanly instead of
silently drawing nothing).

Two real design directions, radically different in cost:
1. **Compile-time unroll** — a `repeat(N)` where `N` is a config-time constant (not a runtime tag
   value) could be unrolled into N copies of the body's steps at compile time, staying inside I3
   entirely (still a flat array, still zero-alloc, still one pass). Doesn't cover `count X from Y to Z`
   where Y/Z are tag values known only at runtime.
2. **Real interpreter loop** — a bounded loop with a runtime-computed trip count needs actual
   loop-back/jump semantics in `CompiledFlow::execute()`, which is a genuine architecture change to
   the execution model I3 currently guarantees (flat array, single pass) — this is not a small
   addition, it's a different execution model, and would need its own ADR-weight design pass (cost,
   zero-alloc-under-iteration story, `Stop`-inside-a-loop semantics, max-iteration safety bound, etc.)
   before any Studio-side block work on top of it means anything.

**Scope decided (§10 Q1): direction 2 is the actual target.** Runtime-computed bounds
(`count X from tag("start") to tag("end") do`) are a real requirement, not a nice-to-have — a
compile-time-only unroll would leave out exactly the "count from Y to Z" case this section opened
with, which is the whole reason `controls_for` was cited from Blockly in the first place.

**Design pass: done, below (§8.1–§8.8).** What follows used to be "the next real step whenever loops
are prioritized" — it's been walked through in this same session: the `ProcessingContext` side-channel
mechanism, the two new nodes, compile-time jump-target wiring, the max-iteration safety bound and its
exact failure path, `Stop`-inside-a-loop semantics, the zero-alloc story (and the new test it needs),
and the compiler-side cycle-detection exemption. Every question direction 2's cost estimate flagged
above (zero-alloc-under-iteration, `Stop` semantics, the safety bound) has a concrete, decided answer
in §8.1–§8.8, not just a placeholder acknowledging they matter.

**The original recommendation ("don't build the block shape until the design pass happens") is now
satisfied, not overridden** — the design pass happened; building the actual C++ (new `ProcessingContext`
fields, the two nodes, the `execute()` change, the compiler special case, the new zero-alloc test) and
the Studio-side loop block shape on top of it are still real, unstarted implementation work, sequenced
last in §9 for the reasons given there — but "don't build a shape for a capability that doesn't exist
yet" no longer applies verbatim, since the capability is now fully specified, just not yet coded.

### 8.1 The core mechanism — a side-channel jump signal, not a `NodeResult`/`INode` change

The single biggest design constraint: **do not touch the `INode`/`NodeResult` contract.** Every
existing node type implements `NodeResult process(ProcessingContext&) noexcept` today; extending
`NodeResult` to carry "jump to index J" would mean either breaking that signature for every node in the
codebase (and every future one) or bolting a second return channel onto it — both disproportionate to
what's needed, and 019 already established the right idiom for exactly this shape of problem:
`aero.flow.switch` doesn't extend `NodeResult` to communicate its routing decision either — it writes
`ctx.active_branch` (a side-channel scalar on `ProcessingContext`) and `CompiledFlow::execute()` reads
it. Loops get the same treatment:

```cpp
// sdk/processing_context.hpp — TWO new fields (loop_continue here; loop_iterations_remaining from
// §8.2 is the other), both cleared in reset(), same shape as active_branch. CORRECTED (§8.3):
// loop_continue is a plain bool, not an index — the index lives on Step, the node never sees it.
bool loop_continue = false;
std::size_t loop_iterations_remaining = 0;

// reset() — BOTH new fields shown explicitly this time. An earlier draft of this section showed only
// one of the two loop fields being cleared here, which is exactly the class of omission §8.1's own
// "ProcessingContext obstacle?" answer warned about (the active_branch/reset() bug) — worth being
// deliberately over-explicit about, not just stating the rule and trusting it gets applied.
// mes_reports.clear(); http_requests.clear(); active_branch = {};
loop_continue = false;
loop_iterations_remaining = 0;
```

`CompiledFlow::execute()`'s change is the whole runtime cost of this feature — the `for` loop becomes a
`while` so `i` can be redirected, and one check is added after the existing Stop/Error handling. This
version reflects §8.3's correction (the target comes from `step.loop_back_target`, not from `ctx`):

```cpp
void execute(ProcessingContext& ctx) const noexcept {
    std::size_t i = 0;
    while (i < steps_.size()) {
        const Step& step = steps_[i];
        if (!step.required_label.empty() && step.required_label != ctx.active_branch) { ++i; continue; }
        const NodeResult r = step.node->process(ctx);
        if (r == NodeResult::Stop) break;
        if (r == NodeResult::Error) { ctx.failed = true; ctx.failed_step = i; break; }
        if (ctx.loop_continue && step.loop_back_target) {   // NEW: this step asked to redirect
            i = *step.loop_back_target;
            ctx.loop_continue = false;
            continue;                                       // skip the ++i below — we already moved
        }
        ++i;
    }
}
```

Everything else about `execute()` — branch-skip, Stop, Error, the step array itself — is byte-for-byte
unchanged. `std::optional<std::size_t>` is a stack value, no heap; setting/checking/resetting it is 0-
alloc, same N1 guarantee every other `ctx` field already has.

**Does `ProcessingContext` obstruct this, and is it worth replacing? No, on both counts — this is
precisely the growth pattern it was designed for.** Its own header comment already says so: "Phase-1
shape: fields grow as node categories need them, but the lifetime and ownership rules are stable." It
has grown this exact way three times already (`mes_reports`, `http_requests`, `active_branch`), each a
plain additive scalar/vector field, no structural rework. Three concrete reasons there's no obstacle:
- **Not size-constrained.** The `kMaxFramePayload = 128` budget documented at the top of the file is
  about `Frame`/`ReceiveFrame` fitting inside Quark's 192-byte mailbox message pool — `Frame` travels
  through the actor mailbox, `ProcessingContext` never does ("Created once per Command, reused across
  Commands on an actor... Never copied, never serialized, never escapes the flow," per its own comment).
  Adding scalar fields here has zero relationship to that budget.
- **`reset()` is the one place that actually needs discipline, and this codebase has already been
  burned by forgetting it once.** The `/code-review` pass earlier in this session's own history caught
  exactly this bug: `active_branch` was added to the struct but *not* to `reset()`, so a stale branch
  value could leak into the next Command. Every new loop field needs the same one-line addition to
  `reset()` — noted here explicitly so it isn't the second time this specific mistake happens.
  `mes_reports.clear(); http_requests.clear(); active_branch = {};` already sets the pattern to copy.
- **No cheaper alternative exists that doesn't reinvent this struct's own job.** The only other place
  loop state could live is on the node instances themselves (see §8.2's refinement below for where that
  *is* the right call) — but the "one active thing per flow, Command-scoped, reset between Commands"
  shape is exactly what `ProcessingContext` exists to hold; recreating that outside it would just be a
  second, redundant per-Command scratch space next to the one that already works.

### 8.2 Two new nodes, both reusing `expr_detail::Program` — the third reuse of the same evaluator

Following the exact pattern §7 already established for `set` (which itself followed `switch`'s
precedent) — `NodeCategory::Rule` (control flow, same bucket as `aero.flow.switch`, not a new category
for one feature — same reasoning as §10 Q5 for `set`):

- **`aero.flow.loop_start`** — config `{counter_tag: string, start_expr: string, max_iterations: int,
  required, no default — see §8.8}`. `process()`: evaluate `start_expr`, write the result into
  `counter_tag` (literally `aero.transform.set`'s own write logic — §7.1's overwrite-in-place
  search-or-append — reused a third time, not reimplemented), AND initialize the fresh-per-Command
  iteration budget: `ctx.loop_iterations_remaining = max_iterations_` (a second new `ProcessingContext`
  field, same additive shape as `loop_jump_to` — `loop_start` is the natural place to initialize it
  since it always runs exactly once before the loop's first iteration, whereas `loop_back` runs once
  *per iteration* and has no single moment to reset a budget to).
- **`aero.flow.loop_back`** — config `{counter_tag: string, step_expr: string (default "1"),
  end_expr: string}` — **no `max_iterations` here**, moved to `loop_start` above (a budget needs a
  single init point, not two nodes each guessing whether they own it). `process()`: evaluate
  `tag(counter_tag) + step_expr`, write back (same overwrite-in-place logic again), decrement
  `ctx.loop_iterations_remaining`; if it hits zero, **`return NodeResult::Error`** — reusing
  `CompiledFlow::execute()`'s *already-existing* error path (`ctx.failed = true; ctx.failed_step = i;
  break;`) verbatim, no new failure-signaling mechanism needed at all (this resolves §8.8's first
  question — see below). Otherwise, evaluate the continue-condition (`step_expr`'s sign decides
  ascending-vs-descending comparison against `end_expr`) and set **`ctx.loop_continue = true`** — a
  plain `bool`, not an index (see §8.3's correction: the node no longer knows or stores any index at
  all).

Both nodes are small, and both are the exact same shape of thing `SwitchNode`/`SetNode` already are:
thin wrappers around `expr_detail::Program::evaluate()` plus one write into `ctx.tags`. Nothing here is
a new interpreter — the "interpreter" part is entirely the one `CompiledFlow::execute()` change in
§8.1; these two nodes are ordinary `INode`s that happen to set well-known side-channel fields.

### 8.3 Compile-time wiring — CORRECTED after adversarial review found the original design impossible

**The original version of this section was wrong, not just underspecified — a real, blocking bug an
independent review caught before any code was written on top of it.** It claimed `loop_back_index_`
could be "constructor state on the `LoopBackNode` instance, resolved once by the Flow Compiler while
it's building the `CompiledFlow` array." Checking the actual `compile_flow()` (`flow_compiler.hpp`)
sequencing: **every node is constructed via `registry.create(type_id, config)` FIRST, and only
afterward does `order_flow_graph()` compute topological order** (it needs the already-constructed nodes
to query `descriptor().category` for the root check). There is no point at which a node's constructor
could receive a topological index — the index doesn't exist yet when construction happens. The doc's
own analogy to `required_label` was actually evidence AGAINST its own design, not for it:
`required_label` is proof this exact pattern already lives on `Step`, not the node — `CompiledFlow::add`
takes it as a parameter precisely *because* it's only known post-construction, during `wire()`.

**Corrected design: the jump target lives on `Step`, exactly like `required_label` does, and the node
carries no index at all.**

```cpp
// compiled_flow.hpp — Step gains one optional field, resolved by the compiler during wire(), same
// timing as required_label — never baked into any node.
struct Step {
    INode* node;
    std::string_view required_label;
    std::optional<std::size_t> loop_back_target;   // set only on the step that IS a loop_back node
};
```

`aero.flow.loop_back`'s `process()` no longer computes or stores any position — it just sets
`ctx.loop_continue = true/false` (§8.2's correction above). `CompiledFlow::execute()` (§8.1's corrected
sketch below) reads the target off the **step**, not off `ctx` and not off the node — the compiler
resolves `loop_back_target` at the exact same moment and by the exact same mechanism it already
resolves `required_label`, no new timing problem introduced.

### 8.4 Max-iteration safety bound — decided: a hard cap, enforced via the existing `Error` path

A malformed or adversarial runtime bound (`count X from 0 to tag("attacker_controlled")`) must not be
able to hang a Command — this runs inline on an edge actor's hot path, not off in some cancellable
background job. `ctx.loop_iterations_remaining` (§8.2) is initialized by `loop_start` and decremented
by `loop_back` every pass; hitting zero makes `loop_back` return `NodeResult::Error`, which
`CompiledFlow::execute()` already turns into `ctx.failed = true` — **a genuine flow failure, not a
silent truncation that looks like the loop finished normally.** This settles §8.8's first open question
outright: no new `Event` type, no separate signal, just the error path every other failing node already
uses. Same "no trap, but never silently wrong either" posture `expr_detail::Program`'s own `kMaxStack`
bound already models (its `evaluate()` has a defensive `if (sp >= kMaxStack) break;`) — this is the
loop-level equivalent of that same belt-and-suspenders guard.

**Real bug found by adversarial review, fixed here: `max_iterations = 0` silently defeats the entire
cap.** `loop_iterations_remaining` is unsigned; `loop_back` **decrements before checking**, so a
config of `0` underflows on the very first pass (`0 - 1` on a `std::size_t` wraps to `SIZE_MAX`) and the
`Error` path never fires — exactly the hang §8.4 exists to prevent, reachable through the one value
the original design forgot to exclude. Fix: `loop_start`'s deploy-time validation
(`validate_node_config`, mirroring the **real, verified** `moving_average` precedent —
`flow_compiler.hpp` already rejects `window < 1` the same way) must reject `max_iterations < 1`, not
just check that the field is *present*. "Required, no default" (§8.8 Q3) was never sufficient on its
own; it needed a minimum too.

### 8.5 `Stop`-inside-a-loop — matches G7's existing precedent, no new scoping invented

If a node inside the loop body returns `NodeResult::Stop`, `execute()`'s existing behavior already
applies unchanged: it breaks out of the **entire** remaining array, loop included — not "exit the loop
but keep running what comes after it." This is the same choice 019's G7 already made for `Stop` under
fan-out — **verbatim, per adversarial review's fact-check** (the earlier draft here paraphrased it in
quotes as if quoting directly; the real G7 text is): *"`Stop` aborts the whole remaining step array
regardless of fan-out (§3) — a fired switch branch or a parallel sibling does not get special-cased;
refining this is deferred, not silently assumed away."* Loops get the identical, consistent answer
rather than a bespoke third scoping rule. A narrower "break-just-the-loop" semantics is a distinct,
future signal (not today's `Stop`) if ever needed — exactly the kind of thing 019 already deferred once
and shouldn't be quietly reintroduced here.

### 8.6 Zero-alloc — inherited for free, but needs its own test to actually prove it

The loop mechanism itself allocates nothing (§8.1/§8.3, corrected — a `bool` on `ctx` plus an
`optional<size_t>` on `Step`, both stack scalars); a loop's total 0-alloc-ness then depends entirely on
every node inside its body already being 0-alloc, which N1 already requires of every node
unconditionally — **except it doesn't, in practice, for every node that exists today; see §8.9 finding
5 below, which found a real counter-example** (`HttpOutputNode` builds a `nlohmann::json` body per
call, an already-documented N1 exception). So this "just works by composition" claim needs narrowing:
it holds for nodes that are actually 0-alloc, and the loop design needs to say what happens when a body
contains one that isn't, not assume the premise always holds. Separately, `flow_zero_alloc.cpp`'s
existing gate has never exercised the "same steps called N times in one Command" shape at all — new
test needed regardless: compile a small loop, run it for, say, 1000 iterations in one Command, assert
zero allocations across the whole run.

### 8.7 Compiler-side work — CORRECTED: this is genuinely new compiler logic, not a reuse

**The original framing here overstated its own precedent, per adversarial review's fact-check.** It
claimed exempting a `loop_back` edge from cycle detection is "the same shape of special-casing
`order_flow_graph` already does for branch labels today." Checked against the real code: it is not.
Today, `"true"`/`"false"`-labeled edges participate in Kahn's-algorithm cycle detection **identically**
to unlabeled edges (they go into the same adjacency/in-degree construction); labels are only consulted
*afterward*, for the `required_label`/conflicting-label/single-branch-source checks. Nothing today is
ever exempted from the topological sort itself — because a switch's branches are, structurally, always
forward edges. A `loop_back` edge is a backward edge *by construction* — that is the entire point of it
— so it needs to be excluded from the adjacency/in-degree construction itself before Kahn's algorithm
runs, which is new logic inside `order_flow_graph`, not an extension of an existing special case.

Design, corrected to be honest about the scope: reuse the **existing** `EdgeSpec.from_port` field with
a new recognized value, `from_port: "loop_back"` — still no new schema field, that part holds — but the
compiler must **filter any edge carrying that label out of the graph handed to Kahn's algorithm
entirely**, then separately validate it as its own category: does it point from the loop body's last
step back to `loop_start`'s immediate successor? is `counter_tag` consistent between the `loop_start`/
`loop_back` pair?

**Decided: a second, fully independent counter — never extend or share the branch-source check.**
`order_flow_graph`'s existing `have_branch_source`/`branch_source` pair (§10 Q5's real precedent)
tracks exactly one concern: at most one branch-producing node. Bolting "and also track at most one
loop" onto that same pair — reusing the variables, or worse, reusing one boolean for "have we seen a
branch-or-loop-source yet" — is precisely the "one object, two abilities" shape that tends to grow new
bugs later (e.g. a flow with one switch AND one loop would trip a shared counter as if it were "two
branch sources," a false rejection of a perfectly valid flow; or a fix that special-cases around the
sharing becomes exactly the kind of accreted-complexity code review keeps finding here). Instead:

```cpp
// order_flow_graph() — two structurally identical but fully separate trackers, not one shared pair.
// Deliberately not merged into one struct/counter even though the shape is the same — see rationale
// above. A small shared UTILITY is fine (it's a pure "at most one of X" check, reused as a function,
// not as shared mutable state); two SEPARATE instances of it are not the same as one shared instance.
struct SingleSourceTracker {
    bool seen = false;
    std::size_t source_index = 0;
    // returns an error string on a second distinct source, std::nullopt otherwise
    std::optional<std::string> see(std::size_t idx, std::string_view what, const Application& app);
};

SingleSourceTracker branch_tracker;   // "at most one branch-producing node" — existing rule, unchanged
SingleSourceTracker loop_tracker;     // "at most one loop" — new rule, own instance, own state
```

Each edge with a non-empty `from_port` still needs to route to the *right* tracker based on which
`from_port` value it carries: `"true"`/`"false"` → `branch_tracker`; `"loop_back"` → `loop_tracker`
(and, structurally, its own `from`/`to` shape check — a loop-back edge is validated differently than a
branch edge, not just counted differently). This is the direct, load-bearing consequence of keeping
them separate: a flow is free to have one switch *and* one loop at the same time as long as the loop
isn't nested inside a branch (the v1 rule below still forbids that specific combination, for a
different, independent reason) — e.g. `Source -> Switch -> (both branches merge) -> Loop -> Output` is
a perfectly legal flow with one of each, sequential rather than nested. A shared/merged counter would
have silently forbidden that combination too, as an accidental side effect of implementation
convenience rather than because anyone decided sequential switch+loop should be illegal.

**Real gap found by adversarial review, needs an explicit v1 rule, not silence: a loop nested inside a
switch branch breaks `loop_start`'s "always runs exactly once before `loop_back`" assumption.** Branch
labels don't propagate transitively — confirmed against `tests/core/flow_graph.cpp`'s own switch test,
where only the node **directly** reached by a `"true"`/`"false"` edge carries that label; everything
after it (even one hop later) is unlabeled and runs unconditionally. So if `loop_start` sits right after
a switch's `"true"` edge, and the switch evaluates `"false"` for a given Command, `loop_start` correctly
gets skipped — but `loop_back` and the rest of the body, being unlabeled, run anyway, decrementing and
comparing against whatever `loop_iterations_remaining`/`counter_tag` state happens to be sitting there
(zero, or stale leftovers) rather than what `loop_start` would have initialized. **v1 fix: reject this
at deploy time** — a `loop_start` reached only via a labeled edge is a compile error ("a loop must be
reachable unconditionally from the flow's root; loops nested inside a branch are not supported yet"),
the same "refuse cleanly rather than silently misbehave" posture every other unsupported combination in
019/this doc already gets (cycles, two-branch-sources, conflicting labels). Not a full fix — a loop
inside a branch might be a real future need — but it closes the gap honestly instead of leaving it
unaddressed.

### 8.8 The three questions this deep pass surfaced — all now resolved

1. **§8.4's failure signal — resolved: hard fail, via the existing `Error` path.** Settled in §8.4's
   rewrite above: `loop_back` returns `NodeResult::Error` when the budget is exhausted, reusing
   `CompiledFlow::execute()`'s pre-existing failure handling verbatim. No new `Event` type. Consistent
   with the reasoning §6.4 already used to justify letting `NaN` propagate instead of guarding it: a
   runaway loop is closer to a configuration mistake than a routine edge case, and papering over it
   with a soft-continue would be exactly the "looks like it worked, silently didn't" failure mode that
   reasoning was built to avoid.

2. **One loop per flow, v1 — resolved: yes, confirmed.** Mirrors the existing "one active switch"
   constraint (019 §10) for the identical structural reason: `ctx.loop_continue`/
   `ctx.loop_iterations_remaining` (§8.3's corrected field set) are single fields, not stacks, so
   nested/sibling loops aren't representable without materially more state. **Enforced via its own
   independent `SingleSourceTracker` instance in `order_flow_graph` (§8.7's corrected design) — not by
   sharing or extending the branch-source counter.** Same shape of rule, deliberately not the same
   object doing double duty.

3. **`max_iterations`'s default value — resolved: no default, required — AND, per §8.4's later fix, a
   minimum of 1.** Rather than guess a number with no real flow-timing data behind it (the same honesty
   this doc applied to "does unrolled `repeat(N)` cover enough cases" in §10 Q1 — that one genuinely
   needed your input, this one is avoidable by *not* needing a number at all), make `max_iterations` a
   **required** `loop_start` config field with no default, forcing every loop's author to consciously
   pick a bound. **Correction from adversarial review: the precedent originally cited here for
   "required-with-no-default" was wrong** — `aero.output.mes`'s `line` field is required, but it DOES
   have a default (`default_string = "line-1"`, `mes_nodes.hpp`), so it wasn't actually an example of
   this pattern. The decision itself still stands on its own reasoning (force a deliberate bound,
   and §8.4's underflow bug is exactly the kind of thing an unexamined default could hide) — it just
   needed a real justification instead of a precedent that turned out not to exist.

### 8.9 Further gaps found by adversarial review — resolved with the strong design, not the cheap one

Three findings §8.8's three questions didn't cover. None require reversing a decision already made.
Resolved here by building real protection rather than restricting the feature or trusting hope —
consistent with §8.7/§7.5's "separate, dedicated mechanism per concern, not a shared shortcut" pattern.

1. **Iteration count alone doesn't bound wall-clock time — resolved: a second, independent deadline
   check, its own field, never reusing the iteration counter.** §8.4 motivates `max_iterations` with
   "this runs inline on an edge actor's hot path" — that argument demands a time bound as much as a
   count bound, since a capped-but-expensive body (many HTTP/MES stages, expensive math) can still stall
   well before hitting even a modest iteration count. Design, mirroring §8.4's existing shape exactly:

   ```cpp
   // sdk/processing_context.hpp — a THIRD new field, deliberately separate from loop_continue and
   // loop_iterations_remaining (own concern, own field — not a second responsibility bolted onto the
   // iteration counter).
   std::optional<std::chrono::steady_clock::time_point> loop_deadline;
   ```

   `aero.flow.loop_start` gains a second **required, no-default** field, `max_duration_ms` — same
   "force a deliberate choice" reasoning §8.8 Q3 already settled for `max_iterations`, applied a second
   time rather than assumed to only matter once. `process()` sets
   `ctx.loop_deadline = steady_clock::now() + milliseconds(max_duration_ms_)` alongside its existing
   work. `aero.flow.loop_back::process()` checks **both** conditions before continuing — iteration
   budget exhausted *or* `steady_clock::now() >= *ctx.loop_deadline` — either one returns
   `NodeResult::Error`, reusing the exact same failure path §8.4 already established, just with a
   second independent trigger. `steady_clock::now()` is a register/syscall read, not an allocation —
   fully compatible with N1.

2. **The "one loop mirrors one active switch" analogy — resolved: framing corrected, already reflected
   in §8.8 Q2's rewrite above.** Kept the "one loop, v1" constraint on its own merits (enforced by its
   own `SingleSourceTracker`, §8.7); dropped the implication that a loop is switch-equivalent risk — it
   isn't, and §8.4–§8.6/§8.9's own extra machinery (safety bounds, a new zero-alloc test, this deadline
   check) is the evidence for why it needed more scrutiny, not less.

3. **A loop body containing an Output-category node can burst downstream buffers — resolved: fix the
   real bug underneath it, then build a dedicated, centrally-enforced budget; don't just restrict the
   feature.** Two parts, both real engineering, not workarounds:

   - **`ProcessingContext::reserve()` is missing two buffers, independent of loops — fix it regardless.**
     It pre-reserves `tags`/`output`/`events` but not `mes_reports`/`http_requests`, so these vectors
     reallocate under growth today even without a loop involved; loops just make the gap painful enough
     to notice. Add both to `reserve()`'s parameter list and every call site.
   - **A dedicated per-Command staging budget, enforced centrally, not restricted to loop bodies and
     not trusted to every Output node remembering to self-limit.** Two more new fields — separate from
     each other, per §8.7/§7.5's "own concern, own object" pattern, since MES durability and HTTP
     egress latency are different real-world costs with no reason to share one counter:
     ```cpp
     std::size_t mes_report_budget_remaining;   // set from actor/deployment config, not per-node
     std::size_t http_request_budget_remaining; // config, e.g. Runtime::configure_mes/configure_http_egress
     ```
     Enforcement is centralized so no individual node's author has to remember a check (the exact class
     of bug `reset()`-discipline already bit this design twice, §8.1/§8.6): `ProcessingContext` gains
     `bool stage_mes_report(StagedMesReport)`/`bool stage_http_request(StagedHttpRequest)` methods that
     do the `push_back` **and** the budget decrement/check internally, returning `false` on exhaustion.
     `HttpOutputNode`/MES output nodes call these instead of `ctx.http_requests.push_back(...)` directly
     — a real, small change to existing node code (not just new code), and return `NodeResult::Error`
     when the helper returns `false`. This protects **any** flow that over-stages, loop-driven or not
     (e.g. runaway fan-out into many Output nodes has the identical risk today, loops just make it easy
     to trigger) — a strictly more capable fix than banning Output nodes from loop bodies, which would
     have solved only the loop-shaped instance of a problem that was never loop-specific to begin with.

## 9. Suggested incremental order (not committed, for discussion)

Ranked by cost, cheapest first. §8 (loops) has moved up conceptually — its design pass is now actually
*done* (§8.1–§8.8), not just "gated behind a future pass" — but it still sits last by build cost: it's
the only item that touches `CompiledFlow::execute()` itself, the hottest, most performance-sensitive,
most-tested path in the runtime (`flow_zero_alloc.cpp`'s gate lives right there), and it's the only item
needing a brand-new test *shape* (§8.6 — repeated `process()` calls in one Command, nothing today
exercises that). Being fully designed lowers the RISK of building it, not the COST:

1. §7 — `aero.transform.set` (+ the DSL `mod`/math-function additions from §6, if wanted alongside it).
   Backend-only, reuses `expr_detail::Program` verbatim, ships with today's plain-text `ConfigForm`
   field. No Studio canvas change required at all.
2. §4.2 — switch as a real C-block with nested branch cavities. Schema-compatible with what's already
   shipped; pure Studio-side interaction rewrite.
3. §4.3 — terminal-vs-continuing Output distinction (Cap vs Stack). Small additive schema change.
4. §5 — Reporter/Boolean value blocks, replacing free-text `expr` (now also used by `set`'s `expr`
   field). Sizable, self-contained; doesn't block or depend on loops.
5. §8 — loops. Design is done (this round); implementation is still the biggest single change in this
   whole document — two new `ProcessingContext` fields, a control-flow change to `execute()` itself,
   two new nodes, a new compiler special case (§8.7), and a new zero-alloc test shape (§8.6). Worth
   doing last specifically *because* it's designed now, not despite it — a careful, well-scoped change
   to the hottest path in the runtime, not a rushed one.

## 10. Open questions — worked through, all six now resolved or explicitly scoped

1. **Does an unrolled `repeat(constant N)` cover enough real use cases on its own?** — **Resolved: no,
   runtime-computed bounds are a real requirement** (`count X from tag("start") to tag("end") do`), not
   just fixed-N repeats. This confirms §8 direction 2 (the real interpreter-loop rewrite) is the actual
   target, not the cheaper compile-time-unroll-only path — see §8's updated scope note. Still not
   designed in detail; the target is now decided, the design pass isn't done.

2. **Where do Reporter/Boolean blocks live spatially?** — **Resolved: a separate "expression editor"
   panel opened from `ConfigForm`** (agreed), not inline on the main canvas. AeroEdge's cards are
   small, uniform jigsaw shapes laid out in a vertical topological layout (§3/019 §4); embedding a full
   value-block tree inline would make cards balloon unpredictably and break the uniform-card visual
   language the auto-layout depends on. Reuses the `tier2` mechanism — one real implementation exists
   today (`ModbusRegisterMap`), `opcua-security`/`http-headers` are declared hints with no UI built yet,
   and §7.3 proposes a `set-expr-helper` as the second real one (corrected from an earlier, wrong "3
   existing panels" claim — see §7.3) — a Reporter/Boolean tree editor extends the same convention, not
   a new UI paradigm, even though fewer panels currently prove it out than originally stated.

3. **Should `aero.rule.expr`'s Stop-on-match get its own visual signal?** — **Resolved: yes** (agreed).
   Reuse the exact mechanism the jigsaw slice already built for category accent colors — add one
   boolean-ish flag to `NodeDescriptor` (e.g. `can_stop`) and render a small icon/badge accent on any
   node type that sets it, distinguishing `aero.rule.expr` (can Stop) from `aero.flow.switch` (never
   stops, only routes) even though both are `Rule`-category and currently look identical besides
   their config fields. Cheap because it rides the same badge/color infrastructure already shipped.

4. **Should §6's grammar additions and §7's `set` land as one PR or split?** — **Recommendation: split.**
   They touch a shared file (`expr_rule_node.hpp`) but are logically independent (a parser/grammar
   extension vs. a brand-new node type) — matches this session's own established pattern of one
   coherent slice per PR (the jigsaw-shapes PR was kept separate from the graph-model PR that preceded
   it, `set` PR wouldn't gate on the grammar PR landing first, but reusing `Op::Mod`/math functions in
   `set`'s own expressions works either order).

5. **Does `set` belong in `NodeCategory::Transform`?** — **Resolved: yes, stays Transform.** One node
   doesn't justify a new category; revisit only if `change-by` or future Variables-style nodes
   accumulate enough that a distinct category would actually mean something structurally (e.g. a
   different default shape or validation rule), not just a label.

6. **Does deploy-time validation need to flag `NaN`-capable expressions** (following §6.4's decision to
   let `NaN` propagate)? — **Resolved: no.** Detecting "this expression *might* produce NaN for some
   input" in general needs real domain/interval analysis over the expression tree — disproportionate
   effort for what §6.4 already settled as an accepted, documented author-responsibility tradeoff, not
   a gap that quietly needs patching. Revisit only if NaN-related confusion turns out to be a recurring
   real support problem, not preemptively.
