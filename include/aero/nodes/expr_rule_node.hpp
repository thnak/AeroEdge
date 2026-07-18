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
//   mul     := unary  ( ('*'|'/') unary )*
//   unary   := ('!'|'-') unary | primary
//   primary := number | tagref | '(' expr ')'
//   tagref  := 'tag' '(' '"' NAME '"' ')' | IDENT          // `tag("name")` or a bare `raw`
// Values are doubles; booleans are 1.0 (true) / 0.0 (false). A missing tag reads as 0.0.
//
// PARSE-ONCE / 0-ALLOC EVAL (N1/N3): compile() parses the text ONCE (at configure/deploy) into a
// flat RPN `Program`. process() walks that vector with a fixed-size value stack — no heap, no
// parsing, no recursion on the hot path (see tests/expr_rule.cpp's alloc gate).
//
// ROUTING (008 §6 "if expr holds, emit AlarmRaised and Stop"): if the expression evaluates non-zero
// the node stages an Event (type from config, default "AlarmRaised") and returns Stop to
// short-circuit the flow; otherwise it returns Continue and the pipeline proceeds.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aero/sdk/node.hpp"

namespace aero::nodes {

namespace expr_detail {

enum class Op : std::uint8_t {
    Const, Tag,                       // push a literal / a tag value
    Neg, Not,                         // unary
    Add, Sub, Mul, Div,               // arithmetic
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
        // A bare identifier IS a tag reference (e.g. `raw`).
        emit_tag(p, std::string(ident));
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
        if (op != Op::Neg && op != Op::Not) --depth_;  // binary ops pop 2 push 1 (net -1)
    }
    void push_depth() { ++depth_; if (depth_ > max_depth_) max_depth_ = depth_; }

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
        const double v = eval(ctx);
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

    static constexpr NodeDescriptor kDesc{NodeCategory::Rule, "aero.rule.expr"};

private:
    // 0-alloc RPN eval over a fixed value stack (N1). Bounded by the program's compiled depth.
    static constexpr std::size_t kMaxStack = 128;

    double eval(const ProcessingContext& ctx) const noexcept {
        double stack[kMaxStack];
        std::size_t sp = 0;
        for (const auto& in : prog_.code) {
            using expr_detail::Op;
            switch (in.op) {
                case Op::Const: stack[sp++] = in.k; break;
                case Op::Tag:   stack[sp++] = tag_value(ctx, in.tag); break;
                case Op::Neg:   stack[sp - 1] = -stack[sp - 1]; break;
                case Op::Not:   stack[sp - 1] = (stack[sp - 1] == 0.0) ? 1.0 : 0.0; break;
                default: {
                    const double b = stack[--sp];
                    const double a = stack[--sp];
                    double r = 0.0;
                    switch (in.op) {
                        case Op::Add: r = a + b; break;
                        case Op::Sub: r = a - b; break;
                        case Op::Mul: r = a * b; break;
                        case Op::Div: r = (b == 0.0) ? 0.0 : a / b; break;  // guarded — no trap (E-safe)
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
            if (sp >= kMaxStack) break;  // parse bounds depth < kMaxStack; belt-and-suspenders
        }
        return sp > 0 ? stack[sp - 1] : 0.0;
    }

    double tag_value(const ProcessingContext& ctx, std::uint32_t idx) const noexcept {
        const std::string& nm = prog_.names[idx];
        for (const auto& t : ctx.tags) {
            if (t.name == nm) return t.value;
        }
        return 0.0;  // missing tag reads as 0.0 (008 §6 open question: could be a config error later)
    }

    Program prog_;
    std::string alarm_;
};

}  // namespace aero::nodes
