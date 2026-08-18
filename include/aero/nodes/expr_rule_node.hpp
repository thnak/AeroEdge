// AeroEdge built-in Rule node — the low-code expression DSL (spec 008 §6, resolving 005 §8).
//
// 008 §6 places "low-code rules (thresholds, switches, simple expressions authored in a visual
// designer)" on the SAME INode seam as everything else, implemented NOT as a sandboxed VM but as a
// built-in `ExprRuleNode` interpreting a small, NON-Turing-complete expression over the working-set
// tags. No loops, no calls, no state → it cannot hang or escape; a threshold does not need a WASM
// sandbox. Pro-code / untrusted logic is the WASM seam's job (wasm_runtime.hpp), not this one.
//
// GRAMMAR (recursive-descent, precedence-climbing; `|` = alternation, `*` = repetition):
//   expr    := or
//   or      := and    ( '||' and )*
//   and     := equ    ( '&&' equ )*
//   equ     := cmp    ( ('=='|'!=') cmp )*
//   cmp     := add    ( ('<'|'>'|'<='|'>=') add )*
//   add     := mul    ( ('+'|'-') mul )*
//   mul     := unary  ( ('*'|'/'|'%') unary )*             // '%' = mod, 020 §6.1
//   unary   := ('!'|'-') unary | primary
//   primary := number | tagref | funccall | '(' expr ')'
//   tagref  := 'tag' '(' '"' NAME '"' ')' | IDENT           // `tag("name")` or a bare `raw`
//   funccall:= IDENT '(' expr (',' expr)? ')'               // 020 §6.2/§6.3 math functions
// Values are doubles; booleans are 1.0 (true) / 0.0 (false). A missing tag reads as 0.0.
//
// FUNCTIONS (020 §6.2/§6.3, grounded in Scratch's `() of ()` operator's 14-function set, minus its
// e^/10^ pair which is generalized here as pow()/exp()): unary — abs, floor, ceil, round, sqrt, sin,
// cos, tan, asin, acos, atan, ln, log, exp; binary — pow(base, exp), min(a, b), max(a, b). An IDENT
// immediately followed by '(' that isn't `tag` or a known function name is a parse error (unknown
// function), not a silent misparse.
//
// PARSE-ONCE / 0-ALLOC EVAL (N1/N3): compile() parses the text ONCE (at configure/deploy) into a
// flat RPN `Program`. process() walks that vector with a fixed-size value stack — no heap, no
// parsing, no recursion on the hot path (see tests/expr_rule.cpp's alloc gate).
//
// ROUTING (008 §6 "if expr holds, emit AlarmRaised and Stop"): if the expression evaluates non-zero
// the node stages an Event (type from config, default "AlarmRaised") and returns Stop to
// short-circuit the flow; otherwise it returns Continue and the pipeline proceeds.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aero/sdk/node.hpp"

namespace aero::nodes {

namespace expr_detail {

enum class Op : std::uint8_t {
    Const, Tag,                       // push a literal / a tag value
    Neg, Not,                         // unary
    Abs, Floor, Ceil, Round, Sqrt,     // unary math (020 §6.2)
    Sin, Cos, Tan, Asin, Acos, Atan,   // unary math (020 §6.2)
    Ln, Log, Exp,                      // unary math (020 §6.2) — ln = natural log, log = base-10
    Add, Sub, Mul, Div, Mod,          // arithmetic (Mod: 020 §6.1)
    Pow, Min, Max,                    // binary math (020 §6.2/§6.3)
    Lt, Gt, Le, Ge, Eq, Ne,           // comparison
    And, Or,                          // boolean
};

struct Instr {
    Op op;
    double k = 0.0;             // Const operand
    std::uint32_t tag = 0;      // Tag: index into Program::names
};

// The compiled form: a flat RPN program + the referenced tag names. `ok`/`error` carry the parse
// outcome so a malformed expression is a value (rejected at configure), never a throw or a crash.
struct Program {
    std::vector<Instr> code;
    std::vector<std::string> names;   // tag names, indexed by Instr::tag
    std::size_t max_depth = 0;        // peak value-stack depth (bounds the eval stack)
    bool ok = false;
    std::string error;

    // 0-alloc RPN eval over a fixed value stack (N1), bounded by `max_depth`. A public method on
    // `Program` itself (not a private helper on whichever node owns one) so any node compiling an
    // expression — `ExprRuleNode`, `aero.flow.switch` (switch_node.hpp) — shares one evaluator instead
    // of duplicating the interpreter loop.
    //
    // `saw_nan` (020 §6.4, optional, 0-cost when null): set to true if ANY instruction produces a NaN
    // during this evaluation — not just a NaN that survives to the final result. A comparison op (Lt/
    // Gt/...) collapses a NaN operand to a clean 0.0/1.0 (IEEE754: every ordered comparison against NaN
    // is false), so checking only the returned value would miss exactly the "goes quiet" case §6.4
    // documents — a comparison-shaped expression like `sqrt(tag("x")) > 5` never returns NaN itself even
    // though it evaluated one internally. Checking after every instruction catches it upstream of that
    // collapse, giving the "operational signal... this fired/didn't-fire because of NaN" both shapes
    // need, not just the bare-arithmetic-shaped one where NaN happens to survive to the top.
    [[nodiscard]] double evaluate(const aero::ProcessingContext& ctx,
                                   bool* saw_nan = nullptr) const noexcept {
        static constexpr std::size_t kMaxStack = 128;
        double stack[kMaxStack];
        std::size_t sp = 0;
        for (const auto& in : code) {
            switch (in.op) {
                case Op::Const: stack[sp++] = in.k; break;
                case Op::Tag:   stack[sp++] = tag_value(ctx, in.tag); break;
                case Op::Neg:   stack[sp - 1] = -stack[sp - 1]; break;
                case Op::Not:   stack[sp - 1] = (stack[sp - 1] == 0.0) ? 1.0 : 0.0; break;
                // Unary math functions (020 §6.2). Domain-restricted ones (Sqrt/Asin/Acos/Ln/Log) let
                // NaN propagate on out-of-domain input — a deliberate split from Div's guard-to-0.0
                // below (020 §6.4): an out-of-domain input here is closer to a config mistake than a
                // routine edge case, and papering over it with a guard would make a broken expression
                // *look* like it's working.
                case Op::Abs:   stack[sp - 1] = std::fabs(stack[sp - 1]); break;
                case Op::Floor: stack[sp - 1] = std::floor(stack[sp - 1]); break;
                case Op::Ceil:  stack[sp - 1] = std::ceil(stack[sp - 1]); break;
                case Op::Round: stack[sp - 1] = std::round(stack[sp - 1]); break;
                case Op::Sqrt:  stack[sp - 1] = std::sqrt(stack[sp - 1]); break;
                case Op::Sin:   stack[sp - 1] = std::sin(stack[sp - 1]); break;
                case Op::Cos:   stack[sp - 1] = std::cos(stack[sp - 1]); break;
                case Op::Tan:   stack[sp - 1] = std::tan(stack[sp - 1]); break;
                case Op::Asin:  stack[sp - 1] = std::asin(stack[sp - 1]); break;
                case Op::Acos:  stack[sp - 1] = std::acos(stack[sp - 1]); break;
                case Op::Atan:  stack[sp - 1] = std::atan(stack[sp - 1]); break;
                case Op::Ln:    stack[sp - 1] = std::log(stack[sp - 1]); break;
                case Op::Log:   stack[sp - 1] = std::log10(stack[sp - 1]); break;
                case Op::Exp:   stack[sp - 1] = std::exp(stack[sp - 1]); break;
                default: {
                    const double b = stack[--sp];
                    const double a = stack[--sp];
                    double r = 0.0;
                    switch (in.op) {
                        case Op::Add: r = a + b; break;
                        case Op::Sub: r = a - b; break;
                        case Op::Mul: r = a * b; break;
                        case Op::Div: r = (b == 0.0) ? 0.0 : a / b; break;  // guarded — no trap (E-safe)
                        case Op::Mod: r = (b == 0.0) ? 0.0 : std::fmod(a, b); break;  // guarded like Div
                        case Op::Pow: r = std::pow(a, b); break;
                        case Op::Min: r = std::min(a, b); break;
                        case Op::Max: r = std::max(a, b); break;
                        case Op::Lt:  r = (a <  b) ? 1.0 : 0.0; break;
                        case Op::Gt:  r = (a >  b) ? 1.0 : 0.0; break;
                        case Op::Le:  r = (a <= b) ? 1.0 : 0.0; break;
                        case Op::Ge:  r = (a >= b) ? 1.0 : 0.0; break;
                        case Op::Eq:  r = (a == b) ? 1.0 : 0.0; break;
                        case Op::Ne:  r = (a != b) ? 1.0 : 0.0; break;
                        case Op::And: r = (a != 0.0 && b != 0.0) ? 1.0 : 0.0; break;
                        case Op::Or:  r = (a != 0.0 || b != 0.0) ? 1.0 : 0.0; break;
                        default: break;
                    }
                    stack[sp++] = r;
                }
            }
            if (saw_nan != nullptr && sp > 0 && std::isnan(stack[sp - 1])) *saw_nan = true;
            if (sp >= kMaxStack) break;  // parse bounds depth < kMaxStack; belt-and-suspenders
        }
        return sp > 0 ? stack[sp - 1] : 0.0;
    }

private:
    double tag_value(const aero::ProcessingContext& ctx, std::uint32_t idx) const noexcept {
        const std::string& nm = names[idx];
        for (const auto& t : ctx.tags) {
            if (t.name == nm) return t.value;
        }
        return 0.0;  // missing tag reads as 0.0 (008 §6 open question: could be a config error later)
    }
};

// A tiny recursive-descent / precedence-climbing parser. It emits RPN as it recurses, so the output
// is naturally postfix and eval needs no tree. Non-Turing-complete by construction: it has no
// production that loops or calls, so a parsed program always terminates in O(code.size()).
class Parser {
public:
    explicit Parser(std::string_view src) noexcept : src_(src) {}

    Program parse() {
        Program p;
        parse_or(p);
        skip_ws();
        if (!failed_ && pos_ != src_.size()) {
            fail("unexpected trailing input at position " + std::to_string(pos_));
        }
        if (failed_) {
            p.code.clear();
            p.names.clear();
            p.ok = false;
            p.error = error_;
            return p;
        }
        p.ok = true;
        p.max_depth = max_depth_;
        return p;
    }

private:
    void parse_or(Program& p) {
        parse_and(p);
        for (;;) {
            skip_ws();
            if (match2('|', '|')) { parse_and(p); emit(p, Op::Or); } else break;
        }
    }
    void parse_and(Program& p) {
        parse_equ(p);
        for (;;) {
            skip_ws();
            if (match2('&', '&')) { parse_equ(p); emit(p, Op::And); } else break;
        }
    }
    void parse_equ(Program& p) {
        parse_cmp(p);
        for (;;) {
            skip_ws();
            if (match2('=', '=')) { parse_cmp(p); emit(p, Op::Eq); }
            else if (match2('!', '=')) { parse_cmp(p); emit(p, Op::Ne); }
            else break;
        }
    }
    void parse_cmp(Program& p) {
        parse_add(p);
        for (;;) {
            skip_ws();
            if (match2('<', '=')) { parse_add(p); emit(p, Op::Le); }
            else if (match2('>', '=')) { parse_add(p); emit(p, Op::Ge); }
            else if (match1('<')) { parse_add(p); emit(p, Op::Lt); }
            else if (match1('>')) { parse_add(p); emit(p, Op::Gt); }
            else break;
        }
    }
    void parse_add(Program& p) {
        parse_mul(p);
        for (;;) {
            skip_ws();
            if (match1('+')) { parse_mul(p); emit(p, Op::Add); }
            else if (match1('-')) { parse_mul(p); emit(p, Op::Sub); }
            else break;
        }
    }
    void parse_mul(Program& p) {
        parse_unary(p);
        for (;;) {
            skip_ws();
            if (match1('*')) { parse_unary(p); emit(p, Op::Mul); }
            else if (match1('/')) { parse_unary(p); emit(p, Op::Div); }
            else if (match1('%')) { parse_unary(p); emit(p, Op::Mod); }  // 020 §6.1
            else break;
        }
    }
    void parse_unary(Program& p) {
        skip_ws();
        if (match1('!')) { parse_unary(p); emit(p, Op::Not); return; }
        if (match1('-')) { parse_unary(p); emit(p, Op::Neg); return; }
        parse_primary(p);
    }
    void parse_primary(Program& p) {
        skip_ws();
        if (failed_) return;
        const char c = peek();
        if (c == '(') {
            ++pos_;
            parse_or(p);
            skip_ws();
            if (!match1(')')) fail("expected ')'");
            return;
        }
        if (c == '"') { parse_string_tag(p); return; }
        if (is_digit(c) || c == '.') { parse_number(p); return; }
        if (is_ident_start(c)) { parse_ident(p); return; }
        fail(std::string("unexpected character '") + (c ? c : '?') + "' at position " +
             std::to_string(pos_));
    }

    void parse_number(Program& p) {
        const std::size_t start = pos_;
        while (pos_ < src_.size() && (is_digit(src_[pos_]) || src_[pos_] == '.' ||
               src_[pos_] == 'e' || src_[pos_] == 'E' ||
               ((src_[pos_] == '+' || src_[pos_] == '-') && pos_ > start &&
                (src_[pos_ - 1] == 'e' || src_[pos_ - 1] == 'E')))) {
            ++pos_;
        }
        const std::string num(src_.substr(start, pos_ - start));
        try {
            std::size_t consumed = 0;
            const double v = std::stod(num, &consumed);
            if (consumed != num.size()) { fail("malformed number '" + num + "'"); return; }
            emit_const(p, v);
        } catch (...) {
            fail("malformed number '" + num + "'");
        }
    }

    void parse_ident(Program& p) {
        const std::size_t start = pos_;
        while (pos_ < src_.size() && is_ident_char(src_[pos_])) ++pos_;
        const std::string_view ident = src_.substr(start, pos_ - start);
        if (ident == "tag") {
            skip_ws();
            if (!match1('(')) { fail("expected '(' after 'tag'"); return; }
            skip_ws();
            if (peek() != '"') { fail("expected a quoted tag name in tag(\"...\")"); return; }
            parse_string_body(p);
            skip_ws();
            if (!match1(')')) fail("expected ')' closing tag(...)");
            return;
        }
        skip_ws();
        if (peek() == '(') { parse_function_call(p, ident); return; }
        // A bare identifier IS a tag reference (e.g. `raw`).
        emit_tag(p, std::string(ident));
    }

    // 020 §6.2/§6.3: IDENT '(' expr (',' expr)? ')' — unary math functions take one argument,
    // pow/min/max take two. `ident` reached here is anything but `tag`; a name matching neither table
    // below is a parse error ("unknown function"), never a silent misparse.
    void parse_function_call(Program& p, std::string_view name) {
        static constexpr std::pair<std::string_view, Op> kUnary[] = {
            {"abs", Op::Abs}, {"floor", Op::Floor}, {"ceil", Op::Ceil}, {"round", Op::Round},
            {"sqrt", Op::Sqrt}, {"sin", Op::Sin}, {"cos", Op::Cos}, {"tan", Op::Tan},
            {"asin", Op::Asin}, {"acos", Op::Acos}, {"atan", Op::Atan},
            {"ln", Op::Ln}, {"log", Op::Log}, {"exp", Op::Exp},
        };
        static constexpr std::pair<std::string_view, Op> kBinary[] = {
            {"pow", Op::Pow}, {"min", Op::Min}, {"max", Op::Max},
        };

        Op fn_op{};
        bool is_unary = false, is_binary = false;
        for (const auto& [n, op] : kUnary) { if (n == name) { fn_op = op; is_unary = true; break; } }
        if (!is_unary) {
            for (const auto& [n, op] : kBinary) { if (n == name) { fn_op = op; is_binary = true; break; } }
        }
        if (!is_unary && !is_binary) { fail("unknown function '" + std::string(name) + "'"); return; }

        if (!match1('(')) { fail("expected '(' after '" + std::string(name) + "'"); return; }
        parse_or(p);  // first argument
        if (is_binary) {
            skip_ws();
            if (!match1(',')) { fail("function '" + std::string(name) + "' requires 2 arguments"); return; }
            parse_or(p);  // second argument
        }
        skip_ws();
        if (!match1(')')) { fail("expected ')' closing '" + std::string(name) + "(...)'"); return; }
        emit(p, fn_op);
    }

    void parse_string_tag(Program& p) { parse_string_body(p); }

    // Parse a "..."-quoted tag name and emit a Tag reference.
    void parse_string_body(Program& p) {
        if (!match1('"')) { fail("expected '\"'"); return; }
        const std::size_t start = pos_;
        while (pos_ < src_.size() && src_[pos_] != '"') ++pos_;
        if (pos_ >= src_.size()) { fail("unterminated string literal"); return; }
        const std::string name(src_.substr(start, pos_ - start));
        ++pos_;  // closing quote
        if (name.empty()) { fail("empty tag name"); return; }
        emit_tag(p, name);
    }

    // --- emit helpers (also track value-stack depth for the eval bound) ---
    void emit_const(Program& p, double v) { p.code.push_back({Op::Const, v, 0}); push_depth(); }
    void emit_tag(Program& p, std::string name) {
        std::uint32_t idx = 0;
        bool found = false;
        for (std::size_t i = 0; i < p.names.size(); ++i) {
            if (p.names[i] == name) { idx = static_cast<std::uint32_t>(i); found = true; break; }
        }
        if (!found) { idx = static_cast<std::uint32_t>(p.names.size()); p.names.push_back(std::move(name)); }
        p.code.push_back({Op::Tag, 0.0, idx});
        push_depth();
    }
    void emit(Program& p, Op op) {
        if (failed_) return;
        p.code.push_back({op, 0.0, 0});
        if (!is_unary_op(op)) --depth_;  // binary ops pop 2 push 1 (net -1); unary ops net 0
    }
    void push_depth() { ++depth_; if (depth_ > max_depth_) max_depth_ = depth_; }

    // Unary ops pop 1 push 1 (net-0 depth change) — everything else emitted via `emit()` is binary
    // (pop 2 push 1). 020 §6.2's math functions extend the original Neg/Not-only set.
    static bool is_unary_op(Op op) noexcept {
        switch (op) {
            case Op::Neg: case Op::Not:
            case Op::Abs: case Op::Floor: case Op::Ceil: case Op::Round: case Op::Sqrt:
            case Op::Sin: case Op::Cos: case Op::Tan: case Op::Asin: case Op::Acos: case Op::Atan:
            case Op::Ln: case Op::Log: case Op::Exp:
                return true;
            default:
                return false;
        }
    }

    // --- lexing primitives ---
    char peek() const noexcept { return pos_ < src_.size() ? src_[pos_] : '\0'; }
    void skip_ws() { while (pos_ < src_.size() && is_space(src_[pos_])) ++pos_; }
    bool match1(char c) { skip_ws(); if (peek() == c) { ++pos_; return true; } return false; }
    bool match2(char a, char b) {
        skip_ws();
        if (pos_ + 1 < src_.size() && src_[pos_] == a && src_[pos_ + 1] == b) { pos_ += 2; return true; }
        return false;
    }
    void fail(std::string msg) { if (!failed_) { failed_ = true; error_ = std::move(msg); } }

    static bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
    static bool is_ident_start(char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool is_ident_char(char c) noexcept { return is_ident_start(c) || is_digit(c) || c == '.'; }

    std::string_view src_;
    std::size_t pos_ = 0;
    bool failed_ = false;
    std::string error_;
    std::size_t depth_ = 0;
    std::size_t max_depth_ = 0;
};

// 020 §6.4: a NaN result is allowed to propagate (a deliberate split from Div's guard-to-0.0) — but it
// must never be entirely silent either. `NaN != 0.0` is true, so a NaN can spuriously FIRE a
// comparison-shaped rule instead of just suppressing one — both ExprRuleNode and SwitchNode stage this
// diagnostic Event whenever they evaluate a NaN, so there's at least an operational signal for "this
// fired/didn't-fire because of NaN," never silence either way.
inline constexpr std::string_view kNaNEventType = "ExprNaN";

}  // namespace expr_detail

class ExprRuleNode final : public INode {
public:
    using Program = expr_detail::Program;

    // Parse-once entry (008 §6). Returns a Program whose `.ok` says whether the text was valid — a
    // malformed expression is a value the deploy path rejects (flow_compiler), never a throw (N3).
    [[nodiscard]] static Program compile(std::string_view expr) {
        return expr_detail::Parser(expr).parse();
    }

    ExprRuleNode(Program prog, std::string alarm) noexcept
        : prog_(std::move(prog)), alarm_(std::move(alarm)) {}

    NodeResult process(ProcessingContext& ctx) noexcept override {
        if (!prog_.ok) return NodeResult::Error;  // defensive: deploy validation rejects bad exprs
        bool saw_nan = false;
        const double v = prog_.evaluate(ctx, &saw_nan);
        if (saw_nan) ctx.events.push_back(Event{expr_detail::kNaNEventType, v});  // 020 §6.4
        if (v != 0.0) {
            // Rule fired: raise the alarm and short-circuit (008 §6). `alarm_` outlives the flow (the
            // node is pinned in the CompiledPlan), so a borrowing Event::type view is safe.
            ctx.events.push_back(Event{std::string_view{alarm_}, v});
            return NodeResult::Stop;
        }
        return NodeResult::Continue;
    }

    const NodeDescriptor& descriptor() const noexcept override { return kDesc; }

    [[nodiscard]] bool valid() const noexcept { return prog_.ok; }
    [[nodiscard]] const std::string& error() const noexcept { return prog_.error; }

    static constexpr std::array<FieldSpec, 2> kFields{{
        {.key = "expr", .label = "Expression", .type = FieldType::String, .required = true,
         .help = "Non-Turing DSL: compare / boolean / arithmetic over tags. On match: alarm + stop."},
        {.key = "alarm", .label = "Alarm event", .type = FieldType::String,
         .default_string = "AlarmRaised"},
    }};
    static constexpr NodeDescriptor kDesc{NodeCategory::Rule, "aero.rule.expr", kFields};

private:
    Program prog_;
    std::string alarm_;
};

}  // namespace aero::nodes
