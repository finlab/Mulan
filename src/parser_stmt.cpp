// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 语法分析 —— 语句与函数声明
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

// ---------------------------------------------------------------- 函数

std::vector<std::unique_ptr<Param>> Parser::parse_params(bool* varargs) {
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
            // 每个参数必须独立写 `T name`；C 式共享类型（`int a, b`）不合法。
            // `b` 会被 parse_type 当成类型名、再在 `)` 处报「期望参数名」，因此提前给出明确诊断。
            if (ptok.kind == TokKind::Ident && (peek(1).is_op(",") || peek(1).is_op(")"))) {
                throw err_at(
                    "parameter must be written as `type name`; each parameter declares its own type, shared types like `int a, b` are not supported",
                    ptok);
            }
            TypeSpecPtr spec = parse_type();
            std::string pname = expect_ident("parameter name").text;
            auto p = std::make_unique<Param>();
            p->name = std::move(pname);
            p->spec = std::move(spec);
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

std::unique_ptr<FuncDecl> Parser::parse_func(std::string name, TypeSpecPtr ret, const Token& tk) {
    auto fn = std::make_unique<FuncDecl>();
    fn->name = std::move(name);
    fn->ret = std::move(ret);
    fn->params = parse_params();
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
    // C 模型：无后缀的 `const T` 是变量绑定 const；`const T*` 的 const 属于 pointee
    const bool is_const = spec->kind == TypeSpec::K::Named && spec->is_const;
    return parse_global_list(std::move(spec), is_const);
}

// ---------------------------------------------------------------- 语句

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

std::vector<StmtPtr> Parser::parse_stmt() {
    const Token t = peek();
    if (t.kind == TokKind::Op && t.text == "{") {
        std::vector<StmtPtr> out;
        out.push_back(parse_block());
        return out;
    }
    if (t.kind == TokKind::Op && t.text == ";") throw err("empty statement `;` is not supported");
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
    if (at_kw("del")) {
        const Token dt = advance();
        throw err_at("`del` has been removed: write `x = null;` to release a reference (same semantics)", dt);
    }
    if (at_kw("static")) {
        throw err("`static` local variables are not supported in phase 1");
    }
    // 前缀 ++/--：只能作为独立语句（不产生值）
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
    // `static` 已从语言移除，但保留明确提示（尤其是函数内旧写法）
    if (t.kind == TokKind::Ident && t.text == "static" &&
        (peek(1).kind == TokKind::Ident || peek(1).kind == TokKind::Kw)) {
        throw err_at("`static` has been removed: local variables do not need it; unit-level variables are private by default, use `public` to share across units", t);
    }
    // 以类型关键字开头的语句一定是声明：直接按声明解析，让类型错误原样暴露
    if (t.kind == TokKind::Kw && is_type_start(t)) {
        return parse_local(false, false);
    }
    if (looks_like_local_decl()) {
        return parse_local(false, false);
    }

    // `Foo bar;` 但 Foo 不是已知类型：给出更明确的诊断（表达式语句不可能以两个标识符开头）
    if (t.kind == TokKind::Ident && peek(1).kind == TokKind::Ident) {
        throw err_at("`" + t.text + "` is not a known type (types must be declared before use)", t);
    }

    ExprPtr expr = parse_expr();
    // 后缀 ++/--：只能作为独立语句（不产生值）
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
    // 类型名可能来自其他 unit（`use` 后直接可见），parser 不预先知道，
    // 因此对任意 Ident 都做一次声明形状试探：`Type name ...`。
    // 表达式语句不可能以「Ident(.Ident)* Ident」开头，误判方向是安全的。
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
    if (!is_auto) spec = parse_type();
    // C 模型：无后缀的 `const T` 是变量绑定 const；`const T*` 的 const 属于 pointee
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
        // 复用 if 的解析：把刚消费的 `elif` 改写成 `if`
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
    if (at_op("..")) {
        advance();
        ExprPtr hi = parse_expr();
        auto range = std::make_unique<RangeExpr>();
        range->lo = std::move(seq);
        range->hi = std::move(hi);
        range->line = tk.line;
        range->col = tk.col;
        seq = std::move(range);
    }
    if (paren) expect_op(")");
    std::unique_ptr<Block> body = parse_block();
    auto node = std::make_unique<ForInStmt>();
    node->by_ref = by_ref;
    node->var = std::move(var);
    node->has_idx = has_idx;
    node->idx_name = std::move(idx_name);
    node->seq = std::move(seq);
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
                if (at_op("..")) {
                    advance();
                    pat.is_range = true;
                    pat.hi = parse_expr();
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
