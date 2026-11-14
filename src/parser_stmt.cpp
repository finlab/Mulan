// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing — statements and function declarations
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

std::vector<std::unique_ptr<Param>> Parser::parse_params(bool* varargs, bool allow_ref) {
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
            const Token ptok = peek();
            // each parameter must be written as `T name` on its own; C-style shared types (`int a, b`) are invalid.
            // Otherwise `b` is taken as a type name by parse_type and only errors at `)` with "expected parameter name", so diagnose up front.
            if (ptok.kind == TokKind::Ident && (peek(1).is_op(",") || peek(1).is_op(")"))) {
                throw err_at(
                    "parameter must be written as `type name`; each parameter declares its own type, shared types like `int a, b` are not supported",
                    ptok);
            }
            // `ref` parameter (borrow): `ref T p` / `ref const T p`. `ref` is a binding mode, not a type.
            bool by_ref = false;
            if (at_kw("ref")) {
                if (!allow_ref) {
                    throw err_at("`ref` parameters are only supported on free functions (methods and constructors are not supported yet)",
                                 peek());
                }
                advance();
                by_ref = true;
            }
            TypeSpecPtr spec = parse_type();
            // `const` is the read-only marker of the binding and does not enter the parameter type (same as local declarations):
            // `const Cat c` / `const int x` make the parameter binding read-only; in `const int* p` the const sits on the pointee, so the binding can still be rebound.
            const bool is_const = spec->kind == TypeSpec::K::Named && spec->is_const;
            if (is_const) spec->is_const = false;
            std::string pname = expect_ident("parameter name").text;
            auto p = std::make_unique<Param>();
            p->name = std::move(pname);
            p->spec = std::move(spec);
            p->is_const = is_const;
            p->by_ref = by_ref;
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

std::unique_ptr<Block> Parser::parse_block() {
    const Token tk = expect_op("{");
    auto block = std::make_unique<Block>();
    block->line = tk.line;
    block->col = tk.col;
    while (!at_op("}")) {
        if (at_eof()) throw err_at("unterminated block", tk);
        for (auto& s : parse_stmt()) block->stmts.push_back(std::move(s));
    }
    expect_op("}");
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
        std::vector<StmtPtr> out;
        out.push_back(parse_switch());
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
    bool paren = false;
    if (at_op("(")) {
        advance();
        paren = true;
    }
    bool by_ref = false;
    if (at_kw("ref")) {
        advance();
        by_ref = true;
    }
    if (peek().kind != TokKind::Ident) throw err("for-in requires a loop variable name");
    std::string first = advance().text;
    bool has_idx = false;
    std::string idx_name;
    if (at_op(",")) {
        if (by_ref) throw err("`for ref i, x` is invalid: write the index form as `for i, ref x in seq` (`ref` must directly precede the element variable)");
        advance();  // ,
        if (at_kw("ref")) {
            advance();
            by_ref = true;
        }
        if (peek().kind != TokKind::Ident) throw err("for-in requires a loop variable name");
        idx_name = std::move(first);
        first = advance().text;
        has_idx = true;
    }
    std::string var = std::move(first);
    expect_kw("in");
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

std::unique_ptr<Stmt> Parser::parse_switch() {
    const Token tk = expect_kw("switch");
    ExprPtr scrut = parse_expr();
    forbid_inc_expr();
    expect_op("{");
    auto node = std::make_unique<SwitchStmt>();
    node->scrut = std::move(scrut);
    node->line = tk.line;
    node->col = tk.col;
    bool saw_else = false;
    while (!at_op("}")) {
        if (at_eof()) throw err_at("unterminated switch", tk);
        const Token at = peek();
        auto arm = std::make_unique<SwitchArm>();
        arm->line = at.line;
        arm->col = at.col;
        if (at_kw("else")) {
            advance();
            if (saw_else) throw err_at("switch can only have one `else` branch", at);
            arm->is_else = true;
            saw_else = true;
        } else {
            if (saw_else) throw err_at("`else:` must be the last branch of the switch", at);
            for (;;) {
                SwitchPat pat;
                pat.lo = parse_expr();
                if (at_op("..") || at_op("..=")) {
                    pat.is_range = true;
                    pat.inclusive = at_op("..=");
                    advance();
                    pat.hi = parse_expr();
                }
                if (peek().kind == TokKind::Ident && peek().text == "step") {
                    throw err_at("`step` is not allowed in a switch pattern: list the values or use a range", peek());
                }
                arm->pats.push_back(std::move(pat));
                if (!at_op(",")) break;
                advance();
            }
        }
        if (!at_op(":")) {
            throw err_at("switch branch must end with `:` (write `:` after the pattern list, the branch body must be `{ }`)",
                         peek());
        }
        advance();  // :
        arm->body = parse_block();
        node->arms.push_back(std::move(arm));
    }
    expect_op("}");
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
