// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing — statements and function declarations
//
// owns:      statements and function/method declarations (bodies, control flow, local declarations)
// not:       type-level declarations (parser_unit.cpp), expressions and type syntax (parser_expr.cpp)
// pass:      parsing
// concept:   parser-statements
#include "parser_impl.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace muc_detail;

namespace muc_detail {

// ast.ASSIGN_OPS
static const std::unordered_set<std::string>& assign_ops() {
    static const std::unordered_set<std::string> s = {
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    };
    return s;
}

}  // namespace muc_detail

// ---------------------------------------------------------------- functions

std::vector<std::unique_ptr<Param>> Parser::parse_params(bool* varargs, bool allow_ref, bool allow_move) {
    expect_op("(");
    std::vector<std::unique_ptr<Param>> params;
    bool saw_varargs = false;
    if (!at_op(")")) {
        for (;;) {
            if (at_op("...")) {
                const Token dots = advance();
                if (!varargs) throw err_at("only `extern` functions can be variadic", dots);
                if (params.empty()) {
                    throw err_at("variadic function needs at least one named parameter before `...`", dots);
                }
                saw_varargs = true;
                if (!at_op(")")) {
                    throw err_at("`...` must be the last parameter", peek());
                }
                break;
            }
            // C says 127 parameters, C++ 256 ([implimits] §1.11): far above real code, and it keeps the
            // call-site matching (which tries every parameter) bounded. The check sits after `...`,
            // which is not a parameter: `extern void f(<256 params>, ...)` is legal (#101).
            if (params.size() >= kMaxParams) {
                throw err_at("a function cannot declare more than " + std::to_string(kMaxParams) +
                                 " parameters; take a struct instead",
                             peek());
            }
            const Token ptok = peek();
            // each parameter must be written as `T name` on its own; C-style shared types (`int a, b`) are invalid.
            // Otherwise `b` is taken as a type name by parse_type and only errors at `)` with "expected parameter name", so diagnose up front.
            if (ptok.kind == TokKind::Ident && (peek(1).is_op(",") || peek(1).is_op(")"))) {
                throw err_at(
                    "parameter must be written as `type name`; each parameter declares its own type, shared types like `int a, b` are not supported",
                    ptok);
            }
            // `const ref T p`: a read-only borrow. `const ref` is the preferred spelling (it reads like
            // C++'s `const&`); the older `ref const T p` spelling stays accepted.
            bool by_ref = false;
            bool const_before_ref = false;
            if (at_kw("const") && peek(1).is_kw("ref")) {
                advance();  // const
                const_before_ref = true;
            }
            if (at_kw("ref")) {
                if (!allow_ref) {
                    throw err_at("`ref` parameters are only supported on free functions (methods and constructors are not supported yet)",
                                 peek());
                }
                advance();
                by_ref = true;
            }
            // `move T p`: the parameter takes over the caller's value. Only a constructor can declare one,
            // and only as `T(move T other)` (agents/value-semantics.md §8.1).
            bool by_move = false;
            if (at_kw("move")) {
                if (!allow_move) {
                    throw err_at("`move` parameters are only supported on a constructor, as `T(move T other)`", peek());
                }
                advance();
                by_move = true;
            }
            TypeSpecPtr spec = parse_type();
            // `const` is the read-only marker of the binding and does not enter the parameter type (same as local declarations):
            // `const Cat c` / `const int x` make the parameter binding read-only; in `const int* p` the const sits on the pointee, so the binding can still be rebound.
            const bool is_const = const_before_ref ||
                                  (spec->kind == TypeSpec::K::Named && spec->is_const);
            // cv stays in the type specification (decided 2026-11-17). A borrowed parameter carries its
            // read-only flag in `pty_cref`, so there the const is recorded only on the binding.
            if (is_const && by_ref) spec->is_const = false;
            std::string pname = expect_ident("parameter name").text;
            auto p = std::make_unique<Param>();
            p->name = std::move(pname);
            p->spec = std::move(spec);
            p->is_const = is_const;
            p->by_ref = by_ref;
            p->by_move = by_move;
            p->line = ptok.line;
            p->col = ptok.col;
            params.push_back(std::move(p));
            if (!at_op(",")) break;
            advance();
        }
    }
    expect_op(")");
    if (saw_varargs) *varargs = true;
    return params;
}

std::unique_ptr<FuncDecl> Parser::parse_func(std::string name, TypeSpecPtr ret, bool ret_ref, const Token& tk) {
    auto fn = std::make_unique<FuncDecl>();
    fn->name = std::move(name);
    fn->ret = std::move(ret);
    fn->ret_ref = ret_ref;
    fn->params = parse_params(nullptr, /*allow_ref=*/true);
    fn->body = parse_block();
    fn->is_extern = false;
    fn->line = tk.line;
    fn->col = tk.col;
    return fn;
}

std::vector<std::unique_ptr<GlobalVar>> Parser::parse_global_list(TypeSpecPtr spec, bool is_const) {
    std::vector<std::unique_ptr<GlobalVar>> out;
    for (;;) {
        const Token name_tok = expect_ident("variable name");
        InitPtr init;
        if (at_op("=")) {
            advance();
            init = parse_init();
        }
        auto gv = std::make_unique<GlobalVar>();
        gv->name = name_tok.text;
        gv->spec = clone_spec(*spec);
        gv->init = std::move(init);
        gv->is_extern = false;
        gv->is_const = is_const;
        gv->line = name_tok.line;
        gv->col = name_tok.col;
        out.push_back(std::move(gv));
        if (!at_op(",")) break;
        advance();
    }
    semicolon();
    return out;
}

std::vector<std::unique_ptr<GlobalVar>> Parser::parse_global() {
    if (at_kw("static")) {
        throw err("`static` has been removed: unit-level variables are private by default; use `public` to share across units");
    }
    TypeSpecPtr spec = parse_type();
    // C model: a bare `const T` makes the variable binding const; in `const T*` the const belongs to the pointee
    const bool is_const = spec->kind == TypeSpec::K::Named && spec->is_const;
    return parse_global_list(std::move(spec), is_const);
}

// ---------------------------------------------------------------- statements

void Parser::push_stmt(StmtPtr s) {
    if (!cur_stmts_) throw MuError("internal: statement outside a statement list");
    cur_stmts_->push_back(std::move(s));
}

std::unique_ptr<Block> Parser::parse_block() {
    NestGuard guard(*this, nest_block_, kMaxBlockNesting, kMaxBlockNesting, "block");
    // A block is a statement list; a `#if` here collects each arm into its own container.
    auto* saved_decls = cur_decls_;
    auto* saved_stmts = cur_stmts_;
    cur_decls_ = nullptr;
    const Token tk = expect_op("{");
    auto block = std::make_unique<Block>();
    cur_stmts_ = &block->stmts;
    block->line = tk.line;
    block->col = tk.col;
    const size_t g0 = groups_.size();
    while (true) {
        if (at_eof()) throw err_at("unterminated block", tk);
        if (at_op("}")) {
            if (groups_.size() > g0) { directive_arm_end(); continue; }
            break;
        }
        if (at_directive()) { directive_header(); continue; }
        for (auto& s : parse_stmt()) push_stmt(std::move(s));
    }
    expect_op("}");
    if (groups_.size() != g0) throw err_at("unterminated `#if` block: missing `}`", tk);
    cur_stmts_ = saved_stmts;
    cur_decls_ = saved_decls;
    return block;
}

std::unique_ptr<Stmt> Parser::parse_para() {
    const Token tk = peek();
    advance();  // `para`
    if (at_kw("for")) {
        auto pf = std::make_unique<ParaForStmt>();
        pf->line = tk.line;
        pf->col = tk.col;
        auto st = parse_for();
        pf->loop.reset(static_cast<ForInStmt*>(st.release()));
        return pf;
    }
    auto node = std::make_unique<ParaStmt>();
    node->line = tk.line;
    node->col = tk.col;
    expect_op("{");
    while (!at_op("}")) {
        if (at_eof()) throw err_at("unterminated `para` block", tk);
        if (!at_op("{")) throw err_at("a `para` section must be a block: write `{ ... }`", peek());
        node->sections.push_back(parse_block());
    }
    expect_op("}");
    if (node->sections.empty()) throw err_at("`para` needs at least one section", tk);
    return node;
}

std::vector<StmtPtr> Parser::parse_stmt() {
    const Token t = peek();
    if (t.kind == TokKind::Op && t.text == "{") {
        std::vector<StmtPtr> out;
        out.push_back(parse_block());
        return out;
    }
    if (t.kind == TokKind::Op && t.text == ";") throw err("empty statement `;` is not supported");
    if (at_kw("ref")) {
        throw err("`ref` is a binding mode, not a type: it cannot be used to declare a variable (only parameters and return types)");
    }
    if (at_kw("if")) {
        std::vector<StmtPtr> out;
        out.push_back(parse_if());
        return out;
    }
    if (at_kw("while")) {
        std::vector<StmtPtr> out;
        out.push_back(parse_while());
        return out;
    }
    if (at_kw("for")) {
        std::vector<StmtPtr> out;
        out.push_back(parse_for());
        return out;
    }
    if (at_kw("para")) {
        std::vector<StmtPtr> out;
        out.push_back(parse_para());
        return out;
    }
    if (at_kw("switch")) {
        // `switch` was folded into `match` (SPEC 10.1.2): one dispatch construct. The keyword stays
        // reserved so old code gets a migration message instead of a parse error about an identifier.
        throw err_at("`switch` was folded into `match`: write `match value { pattern: { ... } else: { ... } }`",
                     peek());
    }
    if (at_kw("match")) {
        std::vector<StmtPtr> out;
        out.push_back(parse_match_stmt());
        return out;
    }
    if (at_kw("break")) {
        advance();
        semicolon();
        auto s = std::make_unique<BreakStmt>();
        s->line = t.line;
        s->col = t.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    if (at_kw("continue")) {
        advance();
        semicolon();
        auto s = std::make_unique<ContinueStmt>();
        s->line = t.line;
        s->col = t.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    if (at_kw("return")) {
        advance();
        ExprPtr value;
        if (!at_op(";")) {
            value = parse_expr();
            forbid_inc_expr();
        }
        semicolon();
        auto s = std::make_unique<ReturnStmt>();
        s->value = std::move(value);
        s->line = t.line;
        s->col = t.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    if (at_kw("throw")) {
        const Token tt = advance();
        ExprPtr value = parse_expr();
        forbid_inc_expr();
        semicolon();
        auto s = std::make_unique<ThrowStmt>();
        s->value = std::move(value);
        s->line = tt.line;
        s->col = tt.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    if (at_kw("try")) {
        std::vector<StmtPtr> out;
        out.push_back(parse_try());
        return out;
    }
    if (at_kw("catch") || at_kw("finally")) {
        throw err("`catch`/`finally` must follow a `try` block");
    }
    if (at_kw("del")) {
        const Token dt = advance();
        throw err_at("`del` has been removed: write `x = null;` to release a reference (same semantics)", dt);
    }
    if (at_kw("static")) {
        throw err("`static` local variables are not supported in phase 1");
    }
    // prefix ++/--: only as a standalone statement (produces no value)
    if (at_op("++", "--")) {
        const Token op = advance();
        ExprPtr target = parse_unary();
        semicolon();
        auto s = std::make_unique<AssignStmt>();
        s->target = std::move(target);
        s->op = op.text == "++" ? "+=" : "-=";
        auto one = std::make_unique<IntLit>();
        one->value = 1;
        one->line = op.line;
        one->col = op.col;
        s->value = std::move(one);
        s->line = op.line;
        s->col = op.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    if (at_kw("const") && peek(1).kind == TokKind::Kw && peek(1).text == "auto") {
        return parse_local(true, true);
    }
    if (at_kw("auto")) {
        return parse_local(false, true);
    }
    // `static` has been removed from the language, but keep an explicit hint (especially for the old form inside functions)
    if (t.kind == TokKind::Ident && t.text == "static" &&
        (peek(1).kind == TokKind::Ident || peek(1).kind == TokKind::Kw)) {
        throw err_at("`static` has been removed: local variables do not need it; unit-level variables are private by default, use `public` to share across units", t);
    }
    // a statement starting with a type keyword is definitely a declaration: parse it as one so type errors surface as-is
    if (t.kind == TokKind::Kw && is_type_start(t)) {
        return parse_local(false, false);
    }
    if (looks_like_local_decl()) {
        return parse_local(false, false);
    }

    // `Foo bar;` where Foo is not a known type: give a clearer diagnostic (an expression statement cannot start with two identifiers)
    if (t.kind == TokKind::Ident && peek(1).kind == TokKind::Ident) {
        throw err_at("`" + t.text + "` is not a known type (types must be declared before use)", t);
    }

    ExprPtr expr = parse_expr();
    // postfix ++/--: only as a standalone statement (produces no value)
    if (at_op("++", "--")) {
        const Token op = advance();
        semicolon();
        auto s = std::make_unique<AssignStmt>();
        s->target = std::move(expr);
        s->op = op.text == "++" ? "+=" : "-=";
        auto one = std::make_unique<IntLit>();
        one->value = 1;
        one->line = op.line;
        one->col = op.col;
        s->value = std::move(one);
        s->line = t.line;
        s->col = t.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    if (peek().kind == TokKind::Op && assign_ops().count(peek().text) != 0) {
        std::string op = advance().text;
        ExprPtr value = parse_expr();
        forbid_inc_expr();
        semicolon();
        auto s = std::make_unique<AssignStmt>();
        s->target = std::move(expr);
        s->op = std::move(op);
        s->value = std::move(value);
        s->line = t.line;
        s->col = t.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    semicolon();
    auto s = std::make_unique<ExprStmt>();
    s->expr = std::move(expr);
    s->line = t.line;
    s->col = t.col;
    std::vector<StmtPtr> out;
    out.push_back(std::move(s));
    return out;
}

bool Parser::looks_like_local_decl() {
    const Token t = peek();
    // A type name may come from another unit (directly visible after `use`), which the parser does not know in advance,
    // so probe the declaration shape `Type name ...` for any Ident.
    // An expression statement cannot start with `Ident(.Ident)* Ident`, so erring in this direction is safe.
    if (t.kind != TokKind::Ident && !(t.kind == TokKind::Kw && is_type_start(t))) {
        return false;
    }
    const std::size_t save = pos_;
    bool ok = false;
    try {
        parse_type();
        ok = peek().kind == TokKind::Ident;
    } catch (const MuError&) {
        ok = false;
    }
    pos_ = save;
    return ok;
}

std::vector<StmtPtr> Parser::parse_local(bool is_const, bool is_auto) {
    if (at_kw("const") && peek(1).kind == TokKind::Kw && peek(1).text == "auto") {
        advance();
        is_const = true;
    }
    if (at_kw("auto")) {
        advance();
        is_auto = true;
    }

    TypeSpecPtr spec;
    if (!is_auto && at_kw("ref")) {
        throw err_at("`ref` is a binding mode, not a type: a variable cannot be declared as `ref T`", peek());
    }
    if (!is_auto) spec = parse_type();
    // C model: a bare `const T` makes the variable binding const; in `const T*` the const belongs to the pointee
    if (spec && spec->kind == TypeSpec::K::Named && spec->is_const) is_const = true;
    const int gid = ++group_counter_;

    std::vector<StmtPtr> out;
    for (;;) {
        const Token name_tok = expect_ident("variable name");
        InitPtr init;
        if (at_op("=")) {
            advance();
            init = parse_init();
        }
        if (is_auto && !init) throw err_at("`auto` declaration requires an initializer", name_tok);
        auto lv = std::make_unique<LocalVar>();
        lv->name = name_tok.text;
        lv->spec = spec ? clone_spec(*spec) : nullptr;
        lv->init = std::move(init);
        lv->is_const = is_const;
        lv->is_auto = is_auto;
        lv->group = gid;
        lv->line = name_tok.line;
        lv->col = name_tok.col;
        out.push_back(std::move(lv));
        if (!at_op(",")) break;
        advance();
    }
    semicolon();
    return out;
}

InitPtr Parser::parse_init() {
    NestGuard guard(*this, nest_block_, kMaxBlockNesting, kMaxBlockNesting, "initializer");
    const Token t = peek();
    if (at_op("{")) {
        advance();
        auto agg = std::make_unique<InitAggregate>();
        agg->line = t.line;
        agg->col = t.col;
        while (!at_op("}")) {
            if (peek().kind == TokKind::Ident && peek(1).is_op("=")) {
                std::string fname = advance().text;
                advance();  // '='
                agg->items.emplace_back(std::move(fname), parse_init());
            } else {
                agg->items.emplace_back(std::string(), parse_init());
            }
            if (at_op(",")) {
                advance();
            } else {
                break;
            }
        }
        expect_op("}");
        return agg;
    }
    auto ie = std::make_unique<InitExpr>();
    ie->expr = parse_expr();
    forbid_inc_expr();
    if (at_op("..") || at_op("..=")) {
        if (at_op("..=")) {
            throw err_at("`..=` is not supported in brace initializers; use a half-open `a..b` range", peek());
        }
        advance();  // ..
        auto rg = std::make_unique<InitRange>();
        rg->lo = std::move(ie->expr);
        rg->hi = parse_expr();
        forbid_inc_expr();
        rg->line = t.line;
        rg->col = t.col;
        if (peek().kind == TokKind::Ident && peek().text == "step") {
            throw err_at("`step` is not supported in brace initializers: use consecutive ranges instead", peek());
        }
        return rg;
    }
    ie->line = t.line;
    ie->col = t.col;
    return ie;
}

std::unique_ptr<Stmt> Parser::parse_if() {
    NestGuard guard(*this, nest_block_, kMaxBlockNesting, kMaxBlockNesting, "statement");
    const Token tk = expect_kw("if");
    ExprPtr cond = parse_expr();
    forbid_inc_expr();
    std::unique_ptr<Block> then = parse_block();
    StmtPtr els;
    if (at_kw("elif")) {
        advance();
        // reuse the if parser: rewrite the just-consumed `elif` as `if`
        --pos_;
        Token if_tok;
        if_tok.kind = TokKind::Kw;
        if_tok.text = "if";
        if_tok.line = tk.line;
        if_tok.col = tk.col;
        toks_[pos_] = std::move(if_tok);
        els = parse_if();
    } else if (at_kw("else")) {
        advance();
        if (at_kw("if")) {
            els = parse_if();
        } else {
            els = parse_block();
        }
    }
    auto node = std::make_unique<IfStmt>();
    node->cond = std::move(cond);
    node->then = std::move(then);
    node->els = std::move(els);
    node->line = tk.line;
    node->col = tk.col;
    return node;
}

std::unique_ptr<Stmt> Parser::parse_while() {
    const Token tk = expect_kw("while");
    ExprPtr cond = parse_expr();
    forbid_inc_expr();
    std::unique_ptr<Block> body = parse_block();
    auto node = std::make_unique<WhileStmt>();
    node->cond = std::move(cond);
    node->body = std::move(body);
    node->line = tk.line;
    node->col = tk.col;
    return node;
}

std::unique_ptr<Stmt> Parser::parse_for() {
    const Token tk = expect_kw("for");
    if (at_op("(") && for_head_is_c_style()) return parse_c_for(tk);
    bool paren = false;
    if (at_op("(")) {
        advance();
        paren = true;
    }
    bool by_ref = false;
    bool ref_const = false;
    // `const ref x`: the read-only binding. `const ref` is the preferred spelling; the older
    // `ref const x` spelling stays accepted.
    if (at_kw("const") && peek(1).is_kw("ref")) {
        advance();  // const
        ref_const = true;
    }
    if (at_kw("ref")) {
        advance();
        by_ref = true;
        if (at_kw("const")) {
            advance();
            ref_const = true;
        }
    }
    auto for_head_error = [&](const Token& at) -> ParserError {
        return err_at("`for` needs either a for-in head (`for name in seq`, parentheses optional) or a "
                      "C-style head in parentheses (`for (init; cond; step)`); the C-style form requires "
                      "the parentheses",
                      at);
    };
    if (peek().kind != TokKind::Ident) throw for_head_error(peek());
    std::string first = advance().text;
    bool has_idx = false;
    std::string idx_name;
    if (at_op(",")) {
        if (by_ref) throw err("`for ref i, x` is invalid: write the index form as `for i, ref x in seq` (`ref` must directly precede the element variable)");
        advance();  // ,
        if (at_kw("const") && peek(1).is_kw("ref")) {
            advance();  // const
            ref_const = true;
        }
        if (at_kw("ref")) {
            advance();
            by_ref = true;
            if (at_kw("const")) {
                advance();
                ref_const = true;
            }
        }
        if (peek().kind != TokKind::Ident) throw err("for-in requires a loop variable name");
        idx_name = std::move(first);
        first = advance().text;
        has_idx = true;
    }
    std::string var = std::move(first);
    if (!at_kw("in")) throw for_head_error(peek());
    advance();  // in
    ExprPtr seq = parse_expr();
    if (at_op("..") || at_op("..=")) {
        const bool inclusive = at_op("..=");
        advance();
        ExprPtr hi = parse_expr();
        auto range = std::make_unique<RangeExpr>();
        range->lo = std::move(seq);
        range->hi = std::move(hi);
        range->inclusive = inclusive;
        range->line = tk.line;
        range->col = tk.col;
        seq = std::move(range);
    }
    ExprPtr step;
    if (peek().kind == TokKind::Ident && peek().text == "step") {
        if (!dynamic_cast<RangeExpr*>(seq.get())) {
            throw err_at("`step` is only allowed on integer ranges (`for i in a..b step n`)", peek());
        }
        advance();  // step
        step = parse_expr();
        if (peek().kind == TokKind::Ident && peek().text == "step") {
            throw err_at("only one `step` is allowed per loop", peek());
        }
    }
    if (paren) expect_op(")");
    std::unique_ptr<Block> body = parse_block();
    auto node = std::make_unique<ForInStmt>();
    node->by_ref = by_ref;
    node->ref_const = ref_const;
    node->var = std::move(var);
    node->has_idx = has_idx;
    node->idx_name = std::move(idx_name);
    node->seq = std::move(seq);
    node->step = std::move(step);
    node->body = std::move(body);
    node->line = tk.line;
    node->col = tk.col;
    return node;
}


// Whether `for (` opens a C-style loop. The for-in form inside parentheses always starts with a loop
// variable: `(` [const] [ref] name [`,` [const] [ref] name] `in`. Everything else — an empty clause, a
// declaration, or an expression such as `i = 0` / `i++` — is the C form.
bool Parser::for_head_is_c_style() {
    const Token& n = peek(1);  // the token right after `(`
    if (n.is_op(";")) return true;                    // `for (;;)`
    if (n.is_kw("ref")) return false;                 // `for (ref x in s)`
    if (n.is_kw("const")) return !peek(2).is_kw("ref");  // `const ref x in` is for-in; `const int i = 0` is not
    if (n.is_kw("auto")) return true;
    if (n.kind == TokKind::Kw) return true;           // a type keyword starts a declaration
    if (n.kind == TokKind::Ident) {
        const Token& m = peek(2);
        if (m.is_kw("in")) return false;              // `for (x in s)`
        if (m.is_op(",")) return false;               // `for (i, ref x in s)`
        return true;                                  // `x = 0`, `x++`, `T v = ...`
    }
    return true;  // anything else cannot start a for-in head, so the C form gives the better diagnostic
}

// `for (init; cond; step) body`. Each clause is optional; a declaration in `init` consumes its own `;`.
std::unique_ptr<Stmt> Parser::parse_c_for(const Token& tk) {
    expect_op("(");
    auto node = std::make_unique<ForStmt>();
    if (at_op(";")) {
        advance();  // empty init clause
    } else if (at_kw("const") && peek(1).kind == TokKind::Kw && peek(1).text == "auto") {
        node->init = parse_local(true, true);  // consumes the `;`
    } else if (at_kw("auto")) {
        node->init = parse_local(false, true);
    } else if (peek().kind == TokKind::Kw && is_type_start(peek())) {
        node->init = parse_local(false, false);
    } else if (looks_like_local_decl()) {
        node->init = parse_local(false, false);
    } else if (at_op("++", "--")) {
        // prefix `++i` / `--i` in the init clause
        const Token op = advance();
        ExprPtr target = parse_unary();
        auto s = std::make_unique<AssignStmt>();
        s->target = std::move(target);
        s->op = op.text == "++" ? "+=" : "-=";
        auto one = std::make_unique<IntLit>();
        one->value = 1;
        one->line = op.line;
        one->col = op.col;
        s->value = std::move(one);
        s->line = op.line;
        s->col = op.col;
        semicolon();
        node->init.push_back(std::move(s));
    } else {
        const Token at = peek();
        ExprPtr e = parse_expr();
        auto step_assign = [&](const std::string& op, ExprPtr target, int line, int col) {
            auto s = std::make_unique<AssignStmt>();
            s->target = std::move(target);
            s->op = op;
            auto one = std::make_unique<IntLit>();
            one->value = 1;
            one->line = line;
            one->col = col;
            s->value = std::move(one);
            s->line = line;
            s->col = col;
            return s;
        };
        if (at_op("++", "--")) {
            const Token op = advance();
            auto s = step_assign(op.text == "++" ? "+=" : "-=", std::move(e), at.line, at.col);
            semicolon();
            node->init.push_back(std::move(s));
        } else if (peek().kind == TokKind::Op && assign_ops().count(peek().text) != 0) {
            std::string op = advance().text;
            ExprPtr value = parse_expr();
            forbid_inc_expr();
            auto s = std::make_unique<AssignStmt>();
            s->target = std::move(e);
            s->op = std::move(op);
            s->value = std::move(value);
            s->line = at.line;
            s->col = at.col;
            semicolon();
            node->init.push_back(std::move(s));
        } else {
            forbid_inc_expr();
            auto s = std::make_unique<ExprStmt>();
            s->expr = std::move(e);
            s->line = at.line;
            s->col = at.col;
            semicolon();
            node->init.push_back(std::move(s));
        }
    }
    if (!at_op(";")) node->cond = parse_expr();
    expect_op(";");
    if (!at_op(")")) {
        // The step clause ends at `)`, so it cannot go through the statement parser (which expects `;`).
        // It is always a single expression statement: `i++`, `i = i + 1`, `f()`.
        if (at_op("++", "--")) {
            const Token op = advance();
            ExprPtr target = parse_unary();
            auto s = std::make_unique<AssignStmt>();
            s->target = std::move(target);
            s->op = op.text == "++" ? "+=" : "-=";
            auto one = std::make_unique<IntLit>();
            one->value = 1;
            one->line = op.line;
            one->col = op.col;
            s->value = std::move(one);
            s->line = op.line;
            s->col = op.col;
            node->step = std::move(s);
        } else {
            const Token at = peek();
            ExprPtr e = parse_expr();
            if (at_op("++", "--")) {
                // postfix `i++` / `i--` in the step clause
                const Token op = advance();
                auto s = std::make_unique<AssignStmt>();
                s->target = std::move(e);
                s->op = op.text == "++" ? "+=" : "-=";
                auto one = std::make_unique<IntLit>();
                one->value = 1;
                one->line = op.line;
                one->col = op.col;
                s->value = std::move(one);
                s->line = at.line;
                s->col = at.col;
                node->step = std::move(s);
            } else if (peek().kind == TokKind::Op && assign_ops().count(peek().text) != 0) {
                std::string op = advance().text;
                ExprPtr value = parse_expr();
                forbid_inc_expr();
                auto s = std::make_unique<AssignStmt>();
                s->target = std::move(e);
                s->op = std::move(op);
                s->value = std::move(value);
                s->line = at.line;
                s->col = at.col;
                node->step = std::move(s);
            } else {
                forbid_inc_expr();
                auto s = std::make_unique<ExprStmt>();
                s->expr = std::move(e);
                s->line = at.line;
                s->col = at.col;
                node->step = std::move(s);
            }
        }
    }
    expect_op(")");
    node->body = parse_block();
    node->line = tk.line;
    node->col = tk.col;
    return node;
}


ExprPtr Parser::parse_match_scrut() {
    if (at_kw("move")) return parse_not();
    return parse_expr();
}

void Parser::parse_match_arms(std::vector<std::unique_ptr<MatchArm>>& arms, bool& has_else,
                              bool expression_form) {
    const Token open = expect_op("{");
    bool saw_else = false;
    while (!at_op("}")) {
        if (at_eof()) throw err_at("unterminated match", open);
        auto arm = std::make_unique<MatchArm>();
        const Token at = peek();
        arm->line = at.line;
        arm->col = at.col;
        if (at_kw("else")) {
            advance();
            if (saw_else) throw err_at("match can only have one `else` arm", at);
            saw_else = true;
            has_else = true;
            arm->is_else = true;
        } else {
            if (saw_else) throw err_at("`else:` must be the last arm of the match", at);
            // `Alt(...)` binds a variant payload; anything else is a list of constant patterns. Which
            // one the arm means is settled in sema, from the scrutinee's type: `None` parses as a bare
            // name pattern here and is reinterpreted as the alternative when the scrutinee is a variant.
            if (peek().kind == TokKind::Ident && peek(1).is_op("(")) {
                for (;;) {  // `A(x)` or an or-pattern `A(x) | B(y)`
                    MatchAlt a;
                    const Token name_tok = advance();
                    a.name = name_tok.text;
                    expect_op("(");
                    if (!at_op(")")) {
                        for (;;) {
                            MatchBind b;
                            b.line = peek().line;
                            b.col = peek().col;
                            if (at_kw("const") && peek(1).is_kw("ref")) {
                                advance();
                                advance();
                                b.mode = 2;  // `const ref`
                            } else if (at_kw("ref")) {
                                advance();
                                b.mode = 1;
                            }
                            if (peek().kind != TokKind::Ident || peek(1).is_op("(")) {
                                // A nested pattern (`Some(Some(x))`) or a literal in a payload position.
                                throw err_at("a match payload binds a name, not a nested pattern "
                                             "(`Alt(x)`); nested patterns are not supported yet",
                                             peek());
                            }
                            b.name = advance().text;
                            a.binds.push_back(std::move(b));
                            if (at_op(",")) {
                                advance();
                                continue;
                            }
                            break;
                        }
                    }
                    expect_op(")");
                    arm->alts.push_back(std::move(a));
                    if (!at_op("|")) break;
                    advance();
                    if (!(peek().kind == TokKind::Ident && peek(1).is_op("("))) {
                        throw err_at("an or-pattern alternative must name an alternative with its "
                                     "payload (`Alt` or `Alt(...)`); write a bare alternative as a "
                                     "separate arm",
                                     peek());
                    }
                }
            } else {
                for (;;) {
                    MatchPat pat;
                    pat.lo = parse_expr();
                    if (at_op("..") || at_op("..=")) {
                        pat.is_range = true;
                        pat.inclusive = at_op("..=");
                        advance();
                        pat.hi = parse_expr();
                    }
                    if (peek().kind == TokKind::Ident && peek().text == "step") {
                        throw err_at("`step` is not allowed in a match pattern: list the values or use a range",
                                     peek());
                    }
                    arm->pats.push_back(std::move(pat));
                    if (!at_op(",")) break;
                    advance();
                }
            }
            if (at_kw("if")) {
                advance();
                arm->guard = parse_expr();
            }
        }
        expect_op(":");
        if (expression_form) {
            if (at_op("{")) {
                throw err_at("an expression match uses expressions for its arms; write the statement "
                             "form (`match v { Alt(x): { ... } }`) or an expression arm",
                             peek());
            }
            arm->value = parse_expr();
            expect_op(";");
        } else {
            if (!at_op("{")) {
                throw err_at("a statement match uses a `{ ... }` block for every arm", peek());
            }
            arm->body = parse_block();
            if (at_op(";")) advance();  // an optional `;` keeps the arm separator uniform
        }
        arms.push_back(std::move(arm));
    }
    expect_op("}");
}

std::unique_ptr<Stmt> Parser::parse_match_stmt() {
    const Token tk = expect_kw("match");
    auto node = std::make_unique<MatchStmt>();
    node->line = tk.line;
    node->col = tk.col;
    node->scrut = parse_match_scrut();
    parse_match_arms(node->arms, node->has_else, /*expression_form=*/false);
    return node;
}

ExprPtr Parser::parse_match_expr() {
    const Token tk = expect_kw("match");
    auto node = std::make_unique<MatchExpr>();
    node->line = tk.line;
    node->col = tk.col;
    node->scrut = parse_match_scrut();
    parse_match_arms(node->arms, node->has_else, /*expression_form=*/true);
    return node;
}

std::unique_ptr<Stmt> Parser::parse_try() {
    const Token tk = expect_kw("try");
    auto node = std::make_unique<TryStmt>();
    node->line = tk.line;
    node->col = tk.col;
    node->body = parse_block();
    while (at_kw("catch")) {
        const Token ck = advance();
        auto c = std::make_unique<CatchClause>();
        c->line = ck.line;
        c->col = ck.col;
        expect_op("(");
        c->name = expect_ident("catch variable name").text;
        expect_op(":");
        c->spec = parse_type();
        expect_op(")");
        c->body = parse_block();
        node->catches.push_back(std::move(c));
    }
    if (at_kw("finally")) {
        advance();
        node->finally_body = parse_block();
    }
    if (node->catches.empty() && !node->finally_body) {
        throw err_at("`try` requires at least one `catch` or `finally`", tk);
    }
    return node;
}
