// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 语法分析
//
// 递归下降；优先级层次与 Python 版完全一致（高 → 低）：
//   postfix → unary → * / % → + - → << >> → & → ^ → | → 比较 → == != → not → and → or
// 位运算高于比较；`not` 低于比较、只高于 and/or。赋值是语句，不是表达式。
#include "parser.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common.hpp"
#include "lexer.hpp"

namespace {

// 与 mtypes.PRIM_NAMES 一致（parser.py 的 PRIMS）；`string` 已改为 core.Str 结构体
const std::unordered_set<std::string>& prims() {
    static const std::unordered_set<std::string> s = {
        "byte",  "char",   "short", "ushort", "int",   "uint",  "long",
        "ulong", "usize", "isize", "float",  "double", "bool",  "void",
    };
    return s;
}

// ast.ASSIGN_OPS
const std::unordered_set<std::string>& assign_ops() {
    static const std::unordered_set<std::string> s = {
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    };
    return s;
}

// 一条声明中的多个声明符共享同一个类型语法；深度复制以各持一份所有权的 TypeSpec。
TypeSpecPtr clone_spec(const TypeSpec& s) {
    auto out = std::make_unique<TypeSpec>();
    out->kind = s.kind;
    out->name = s.name;
    out->elem = s.elem ? clone_spec(*s.elem) : nullptr;
    out->size = s.size;
    out->rank = s.rank;
    out->is_const = s.is_const;
    out->weak = s.weak;
    out->line = s.line;
    out->col = s.col;
    return out;
}

TypeSpecPtr make_named_spec(const std::string& name, int line, int col) {
    auto spec = std::make_unique<TypeSpec>();
    spec->kind = TypeSpec::K::Named;
    spec->name = name;
    spec->line = line;
    spec->col = col;
    return spec;
}

// ---------------------------------------------------------------- 语法分析器

class Parser {
public:
    Parser(const std::string& src, const std::string& filename)
        : filename_(filename), toks_(tokenize(src, filename)) {
        // std.string 提供的字符串类型（库在 sema 前不可见）
        type_names_.insert("string");
        // std.string 总是隐式依赖（字符串字面量需要它的类型）
        module_names_.insert("std.string");
    }

    std::string parse_unit_name() {
        std::string n = expect_ident("unit name").text;
        while (at_op(".")) {
            advance();
            n += ".";
            n += expect_ident("unit name").text;
        }
        return n;
    }

    std::unique_ptr<Unit> parse_unit();

private:
    // ------------------------------------------------------ 基础

    const Token& peek(std::size_t k = 0) const {
        std::size_t j = pos_ + k;
        return j < toks_.size() ? toks_[j] : toks_.back();
    }

    bool at_eof() const { return peek().kind == TokKind::Eof; }

    Token advance() {
        Token t = peek();
        if (t.kind != TokKind::Eof) ++pos_;
        return t;
    }

    ParserError err(const std::string& msg) const { return err_at(msg, peek()); }

    ParserError err_at(const std::string& msg, const Token& t) const {
        return ParserError(msg, t.line, t.col, filename_);
    }

    template <class... Args>
    bool at_op(Args&&... ops) const {
        const Token& t = peek();
        if (t.kind != TokKind::Op) return false;
        return ((t.text == ops) || ...);
    }

    template <class... Args>
    bool at_kw(Args&&... kws) const {
        const Token& t = peek();
        if (t.kind != TokKind::Kw) return false;
        return ((t.text == kws) || ...);
    }

    Token expect_op(const std::string& op) {
        if (!at_op(op)) throw err("expected `" + op + "` but got " + peek().describe());
        return advance();
    }

    Token expect_kw(const std::string& kw) {
        if (!at_kw(kw)) throw err("expected `" + kw + "` but got " + peek().describe());
        return advance();
    }

    Token expect_ident(const std::string& what = "identifier") {
        if (peek().kind != TokKind::Ident) {
            throw err("expected " + what + " but got " + peek().describe());
        }
        return advance();
    }

    void semicolon() { expect_op(";"); }

    // `++`/`--` 没有返回值：出现在任何表达式上下文都是错误
    void forbid_inc_expr() const {
        if (at_op("++", "--")) {
            throw err("`++`/`--` has no value and cannot appear in an expression; it must be a standalone statement");
        }
    }

    // ------------------------------------------------------ unit

    std::unique_ptr<Decl> parse_extern(const Token& tk);
    std::unique_ptr<Decl> parse_struct(bool is_union = false);
    std::unique_ptr<Decl> parse_enum();
    std::unique_ptr<Decl> parse_class();
    std::unique_ptr<Decl> parse_interface();
    std::unique_ptr<FuncDecl> parse_operator_func(TypeSpecPtr ret, const Token& tk);
    std::unique_ptr<MethodDecl> parse_method(TypeSpecPtr ret, const Token& name_tok);
    std::unique_ptr<CtorDecl> parse_ctor(const Token& name_tok);
    std::unique_ptr<DtorDecl> parse_dtor();
    std::vector<std::string> parse_base_list();
    std::string parse_qualified_name();
    std::vector<std::unique_ptr<GlobalVar>> parse_global();
    std::vector<std::unique_ptr<GlobalVar>> parse_global_list(TypeSpecPtr spec, bool is_const);
    std::unique_ptr<FuncDecl> parse_func(std::string name, TypeSpecPtr ret, const Token& tk);
    std::vector<std::unique_ptr<Param>> parse_params(bool* varargs = nullptr);

    // ------------------------------------------------------ 类型

    bool is_type_start(const Token& t) const;
    bool is_module_prefix(const std::string& s) const;
    TypeSpecPtr parse_type();
    TypeSpecPtr parse_type_base();  // 不含数组后缀（供 `new T[n]` 使用）
    TypeSpecPtr type_suffixes(TypeSpecPtr spec, const Token& t);

    // ------------------------------------------------------ 语句

    std::unique_ptr<Block> parse_block();
    std::vector<StmtPtr> parse_stmt();
    bool looks_like_local_decl();
    std::vector<StmtPtr> parse_local(bool is_const, bool is_auto);
    InitPtr parse_init();
    std::unique_ptr<Stmt> parse_if();
    std::unique_ptr<Stmt> parse_while();
    std::unique_ptr<Stmt> parse_for();
    std::unique_ptr<Stmt> parse_switch();

    // ------------------------------------------------------ 表达式

    ExprPtr parse_expr() { return parse_ternary(); }
    ExprPtr parse_ternary();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_not();
    ExprPtr parse_eq();
    ExprPtr parse_rel();
    ExprPtr parse_bitor();
    ExprPtr parse_bitxor();
    ExprPtr parse_bitand();
    ExprPtr parse_shift();
    ExprPtr parse_add();
    ExprPtr parse_mul();
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr parse_postfix_tail(ExprPtr e);
    ExprPtr parse_call(ExprPtr callee);
    ExprPtr parse_primary();

    ExprPtr make_binary(const std::string& op, ExprPtr left, ExprPtr right, const Token& tok);

    std::string filename_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    int group_counter_ = 0;  // 声明语句分组计数（auto 多声明符一致性检查用）
    // 已声明的本地类型名（struct / enum），用于消解 `T x` 与表达式的歧义
    std::unordered_set<std::string> type_names_;
    // `use` 引入的 unit 名，支持 `math.Point` 这种限定类型
    std::unordered_set<std::string> module_names_;
};

// ---------------------------------------------------------------- unit

std::unique_ptr<Unit> Parser::parse_unit() {
    const Token t = peek();
    bool unit_public = false;
    if (at_kw("public", "private")) {
        unit_public = at_kw("public");
        advance();
    }
    if (!at_kw("unit")) {
        throw err("file must start with `unit name;`, `public unit name;` or `private unit name;`");
    }
    advance();
    std::string name = parse_unit_name();
    semicolon();

    auto unit = std::make_unique<Unit>();
    unit->name = std::move(name);
    unit->filename = filename_;
    unit->is_public = unit_public;
    unit->line = t.line;
    unit->col = t.col;

    while (!at_eof()) {
        const Token tk = peek();
        bool is_pub = false;
        // 布局修饰符（packed / align(N)）：与 public/private 同类，任意顺序、可组合
        bool packed = false;
        ExprPtr align_expr;
        bool has_layout = false;
        Token layout_tok = tk;
        for (;;) {
            if (at_kw("public", "private")) {
                is_pub = at_kw("public");
                advance();
                continue;
            }
            if (at_kw("packed")) {
                if (packed) throw err_at("duplicate layout modifier `packed`", peek());
                packed = true;
                has_layout = true;
                layout_tok = peek();
                advance();
                continue;
            }
            // `align` 保持标识符身份，只有 `align(` 形式才是布局修饰符（上下文关键字）
            if (peek().kind == TokKind::Ident && peek().text == "align" && peek(1).is_op("(")) {
                if (align_expr) throw err_at("duplicate layout modifier `align(...)`", peek());
                layout_tok = peek();
                has_layout = true;
                advance();  // align
                advance();  // (
                align_expr = parse_expr();
                expect_op(")");
                continue;
            }
            break;
        }
        if (at_eof()) throw err_at("expected declaration after modifier", tk);

        if (at_kw("use")) {
            if (is_pub) throw err_at("`public` cannot be used with `use`", tk);
            if (has_layout) {
                throw err_at("layout modifier `packed` / `align(N)` can only be used in a `struct` or `union` declaration", layout_tok);
            }
            advance();
            std::string uname = parse_unit_name();
            semicolon();
            auto use = std::make_unique<UseDecl>();
            use->unit_name = std::move(uname);
            use->line = tk.line;
            use->col = tk.col;
            module_names_.insert(use->unit_name);
            unit->uses.push_back(std::move(use));
            continue;
        }

        if (at_kw("extern")) {
            if (is_pub) throw err_at("`extern` declarations are implicitly public; `public` cannot be used", tk);
            if (has_layout) {
                throw err_at("layout modifier `packed` / `align(N)` can only be used in a `struct` or `union` declaration", layout_tok);
            }
            advance();
            unit->decls.push_back(parse_extern(tk));
            continue;
        }

        if (at_kw("struct") || at_kw("union")) {
            const bool is_union = at_kw("union");
            auto d = parse_struct(is_union);
            d->is_public = is_pub;
            if (auto* sd = dynamic_cast<StructDecl*>(d.get())) {
                sd->is_packed = packed;
                sd->is_union = is_union;
                sd->align_expr = std::move(align_expr);
            }
            unit->decls.push_back(std::move(d));
            continue;
        }

        if (at_kw("enum")) {
            if (has_layout) {
                throw err_at("`enum` does not support layout modifiers (`packed` / `align(N)`)", layout_tok);
            }
            auto d = parse_enum();
            d->is_public = is_pub;
            unit->decls.push_back(std::move(d));
            continue;
        }

        if (at_kw("class")) {
            if (has_layout) {
                throw err_at("`class` does not support layout modifiers (`packed` / `align(N)`)", layout_tok);
            }
            auto d = parse_class();
            d->is_public = is_pub;
            unit->decls.push_back(std::move(d));
            continue;
        }

        if (at_kw("interface")) {
            if (has_layout) {
                throw err_at("`interface` does not support layout modifiers (`packed` / `align(N)`)", layout_tok);
            }
            auto d = parse_interface();
            d->is_public = is_pub;
            unit->decls.push_back(std::move(d));
            continue;
        }

        if (has_layout) {
            throw err_at("layout modifier `packed` / `align(N)` can only be used in a `struct` or `union` declaration", layout_tok);
        }

        if (at_kw("static")) {
            throw err_at("`static` has been removed: unit-level variables are private by default; use `public` to share across units", tk);
        }

        if (at_kw("const")) {
            auto globals = parse_global();
            for (auto& g : globals) g->is_public = is_pub;
            for (auto& g : globals) unit->decls.push_back(std::move(g));
            continue;
        }

        if (at_kw("auto")) throw err("`auto` cannot be used for unit-level variables; an explicit type is required");

        // 函数定义或 unit 级变量
        TypeSpecPtr spec = parse_type();
        if (at_kw("operator")) {
            auto f = parse_operator_func(std::move(spec), tk);
            f->is_public = is_pub;
            unit->decls.push_back(std::move(f));
            continue;
        }
        if (peek().kind == TokKind::Ident && peek(1).is_op("(")) {
            const Token name_tok = advance();
            auto f = parse_func(name_tok.text, std::move(spec), name_tok);
            f->is_public = is_pub;
            unit->decls.push_back(std::move(f));
        } else {
            // unit 级变量：默认 private，public 由前缀控制
            auto globals = parse_global_list(std::move(spec), false);
            for (auto& g : globals) g->is_public = is_pub;
            for (auto& g : globals) unit->decls.push_back(std::move(g));
        }
    }

    return unit;
}

std::unique_ptr<Decl> Parser::parse_extern(const Token& tk) {
    TypeSpecPtr spec = parse_type();
    const Token name_tok = expect_ident("external symbol name");
    if (at_op("(")) {
        auto fn = std::make_unique<FuncDecl>();
        fn->name = name_tok.text;
        fn->ret = std::move(spec);
        fn->params = parse_params(&fn->is_varargs);
        fn->is_extern = true;
        fn->is_public = true;  // `extern` 声明隐式公开（parser 已禁止与 public 同写）
        fn->line = tk.line;
        fn->col = tk.col;
        semicolon();
        return fn;
    }
    auto gv = std::make_unique<GlobalVar>();
    gv->name = name_tok.text;
    gv->spec = std::move(spec);
    gv->is_extern = true;
    gv->is_const = false;
    gv->line = tk.line;
    gv->col = tk.col;
    semicolon();
    return gv;
}

std::unique_ptr<Decl> Parser::parse_struct(bool is_union) {
    const char* kw = is_union ? "union" : "struct";
    const Token tk = expect_kw(kw);
    std::string name = expect_ident(std::string(kw) + " name").text;
    type_names_.insert(name);
    expect_op("{");

    auto decl = std::make_unique<StructDecl>();
    decl->name = name;
    decl->line = tk.line;
    decl->col = tk.col;
    decl->is_union = is_union;

    bool cur_pub = true;  // struct 默认 public
    while (!at_op("}")) {
        // 分段标签：`public:` / `private:` 切换后续成员的默认可见性
        if ((at_kw("public") || at_kw("private")) && peek(1).is_op(":")) {
            if (is_union) {
                throw err("union members are always public; `public:` / `private:` sections are not allowed in a union");
            }
            cur_pub = at_kw("public");
            advance();
            advance();
            continue;
        }
        bool member_pub = cur_pub;
        bool explicit_mod = false;
        if (at_kw("public", "private")) {
            if (is_union) {
                throw err("union members are always public; remove `public` / `private`");
            }
            member_pub = at_kw("public");
            advance();
            explicit_mod = true;
        }
        if (at_op("~")) {
            if (explicit_mod) throw err("destructor cannot use `public`/`private`");
            if (is_union) throw err("union `" + name + "` cannot have a destructor");
            if (decl->dtor) throw err("struct `" + name + "` can only have one destructor");
            decl->dtor = parse_dtor();
            continue;
        }
        if (peek().kind == TokKind::Ident && peek().text == name && peek(1).is_op("(")) {
            if (is_union) throw err("union `" + name + "` cannot have a constructor");
            const Token ctor_name = advance();
            decl->ctors.push_back(parse_ctor(ctor_name));
            decl->ctors.back()->is_public = member_pub;
            continue;
        }
        if (at_kw("weak")) {
            throw err("`weak` can only be used on class fields");
        }
        TypeSpecPtr spec = parse_type();
        const Token name_tok = expect_ident("field or method name");
        if (at_op("(")) {
            if (is_union) throw err("union `" + name + "` cannot have methods");
            decl->methods.push_back(parse_method(std::move(spec), name_tok));
            decl->methods.back()->is_public = member_pub;
            continue;
        }
        decl->fields.emplace_back(name_tok.text, clone_spec(*spec));
        decl->field_visibility[name_tok.text] = member_pub;
        for (;;) {
            if (!at_op(",")) break;
            advance();
            std::string fname = expect_ident("field name").text;
            decl->field_visibility[fname] = member_pub;
            decl->fields.emplace_back(std::move(fname), clone_spec(*spec));
        }
        semicolon();
    }
    expect_op("}");
    if (at_op(";")) advance();
    return decl;
}

std::unique_ptr<Decl> Parser::parse_enum() {
    const Token tk = expect_kw("enum");
    std::string name = expect_ident("enum name").text;
    type_names_.insert(name);
    expect_op("{");

    auto decl = std::make_unique<EnumDecl>();
    decl->name = name;
    decl->line = tk.line;
    decl->col = tk.col;

    while (!at_op("}")) {
        std::string member = expect_ident("enum member name").text;
        std::optional<long long> val;
        if (at_op("=")) {
            advance();
            long long sign = 1;
            if (at_op("-")) {
                advance();
                sign = -1;
            }
            const Token t = peek();
            if (t.kind != TokKind::Int) throw err("enum value must be an integer literal");
            advance();
            val = sign * t.ival;
        }
        decl->members.emplace_back(std::move(member), val);
        if (at_op(",")) {
            advance();
        } else {
            break;
        }
    }
    expect_op("}");
    if (at_op(";")) advance();
    return decl;
}

std::string Parser::parse_qualified_name() {
    Token t = expect_ident("type name");
    std::string n = t.text;
    while (at_op(".")) {
        advance();
        n += ".";
        n += expect_ident("name").text;
    }
    return n;
}

std::vector<std::string> Parser::parse_base_list() {
    std::vector<std::string> bases;
    if (!at_op(":")) return bases;
    advance();
    for (;;) {
        bases.push_back(parse_qualified_name());
        if (at_op(",")) {
            advance();
            continue;
        }
        break;
    }
    return bases;
}

std::unique_ptr<MethodDecl> Parser::parse_method(TypeSpecPtr ret, const Token& name_tok) {
    auto m = std::make_unique<MethodDecl>();
    m->name = name_tok.text;
    m->ret = std::move(ret);
    m->line = name_tok.line;
    m->col = name_tok.col;
    m->params = parse_params();
    if (at_kw("override")) {
        advance();
        m->is_override = true;
    }
    if (at_op(";")) {
        advance();
    } else {
        m->body = parse_block();
    }
    return m;
}

std::unique_ptr<CtorDecl> Parser::parse_ctor(const Token& name_tok) {
    auto c = std::make_unique<CtorDecl>();
    c->line = name_tok.line;
    c->col = name_tok.col;
    c->params = parse_params();
    c->body = parse_block();
    return c;
}

std::unique_ptr<DtorDecl> Parser::parse_dtor() {
    const Token tk = expect_op("~");
    expect_ident("class name");
    expect_op("(");
    expect_op(")");
    auto d = std::make_unique<DtorDecl>();
    d->line = tk.line;
    d->col = tk.col;
    d->body = parse_block();
    return d;
}

std::unique_ptr<Decl> Parser::parse_class() {
    const Token tk = expect_kw("class");
    const Token nm = expect_ident("class name");
    type_names_.insert(nm.text);

    auto cd = std::make_unique<ClassDecl>();
    cd->name = nm.text;
    cd->line = tk.line;
    cd->col = tk.col;
    cd->bases = parse_base_list();

    expect_op("{");
    bool cur_pub = false;  // class 默认 private
    while (!at_op("}")) {
        // 分段标签：`public:` / `private:` 切换后续成员的默认可见性
        if ((at_kw("public") || at_kw("private")) && peek(1).is_op(":")) {
            cur_pub = at_kw("public");
            advance();
            advance();
            continue;
        }
        bool member_pub = cur_pub;
        bool explicit_mod = false;
        if (at_kw("public", "private")) {
            member_pub = at_kw("public");
            advance();
            explicit_mod = true;
        }
        if (at_op("~")) {
            if (explicit_mod) throw err("destructor cannot use `public`/`private`");
            if (cd->dtor) throw err("class `" + cd->name + "` can only have one destructor");
            cd->dtor = parse_dtor();
            continue;
        }
        bool prefix_override = false;
        if (at_kw("override")) {
            advance();
            prefix_override = true;
        }
        const Token t = peek();
        if (t.kind == TokKind::Ident && t.text == cd->name && peek(1).is_op("(")) {
            const Token ctor_name = advance();
            cd->ctors.push_back(parse_ctor(ctor_name));
            cd->ctors.back()->is_public = member_pub;
            continue;
        }
        bool prefix_weak = false;
        if (at_kw("weak")) {
            advance();
            prefix_weak = true;
        }
        TypeSpecPtr spec = parse_type();
        if (prefix_weak) spec->weak = true;
        const Token name_tok = expect_ident("member name");
        if (at_op("(")) {
            if (prefix_weak) throw err_at("`weak` can only be used on fields, not on methods", name_tok);
            auto m = parse_method(std::move(spec), name_tok);
            if (prefix_override) m->is_override = true;
            m->is_public = member_pub;
            cd->methods.push_back(std::move(m));
            continue;
        }
        if (prefix_override) throw err_at("`override` can only be used on methods", name_tok);
        cd->fields.emplace_back(name_tok.text, clone_spec(*spec));
        cd->field_visibility[name_tok.text] = member_pub;
        while (at_op(",")) {
            advance();
            const std::string fname = expect_ident("field name").text;
            cd->field_visibility[fname] = member_pub;
            cd->fields.emplace_back(fname, clone_spec(*spec));
        }
        semicolon();
    }
    expect_op("}");
    if (at_op(";")) advance();
    return cd;
}

std::unique_ptr<Decl> Parser::parse_interface() {
    const Token tk = expect_kw("interface");
    const Token nm = expect_ident("interface name");
    type_names_.insert(nm.text);

    auto id = std::make_unique<InterfaceDecl>();
    id->name = nm.text;
    id->line = tk.line;
    id->col = tk.col;
    id->bases = parse_base_list();

    expect_op("{");
    while (!at_op("}")) {
        if (at_kw("public", "private")) {
            throw err("`public`/`private` cannot be used on interface methods (interface methods are always public)");
        }
        TypeSpecPtr spec = parse_type();
        const Token name_tok = expect_ident("method name");
        id->methods.push_back(parse_method(std::move(spec), name_tok));
    }
    expect_op("}");
    if (at_op(";")) advance();
    return id;
}

std::unique_ptr<FuncDecl> Parser::parse_operator_func(TypeSpecPtr ret, const Token& tk) {
    expect_kw("operator");
    std::string sym;
    if (at_op("[")) {
        advance();
        expect_op("]");
        sym = "[]";
        if (at_op("=")) {
            advance();
            sym = "[]=";
        }
    } else if (peek().kind == TokKind::Op) {
        sym = advance().text;
    } else {
        throw err("expected operator after `operator`");
    }

    auto f = std::make_unique<FuncDecl>();
    f->name = "operator" + sym;
    f->ret = std::move(ret);
    f->line = tk.line;
    f->col = tk.col;
    f->is_operator = true;
    f->params = parse_params();
    f->body = parse_block();
    return f;
}

// ---------------------------------------------------------------- 类型

bool Parser::is_type_start(const Token& t) const {
    if (t.kind == TokKind::Kw) {
        if (prims().count(t.text) != 0) return true;
        if (t.text == "const") return true;
        return false;
    }
    if (t.kind == TokKind::Ident) {
        if (type_names_.count(t.text) != 0) return true;
        // 限定类型 `math.Point` / `std.string.string` / `lib.a.b.T`
        if (peek(1).is_op(".") && is_module_prefix(t.text)) return true;
    }
    return false;
}

// 是否是某个已导入 unit 名（或 std.string）的点号前缀。
bool Parser::is_module_prefix(const std::string& s) const {
    for (const auto& m : module_names_) {
        if (m == s) return true;
        if (m.size() > s.size() && m.compare(0, s.size(), s) == 0 && m[s.size()] == '.') {
            return true;
        }
    }
    return false;
}

TypeSpecPtr Parser::parse_type() {
    bool base_const = false;
    if (at_kw("const")) {
        advance();
        base_const = true;
    }
    const Token t = peek();
    TypeSpecPtr spec;
    if (t.kind == TokKind::Kw && prims().count(t.text) != 0) {
        advance();
        spec = make_named_spec(t.text, t.line, t.col);
    } else if (t.kind == TokKind::Ident) {
        advance();
        std::string name = t.text;
        while (at_op(".")) {
            advance();
            name += ".";
            name += expect_ident("type name").text;
        }
        spec = make_named_spec(name, t.line, t.col);
    } else {
        throw err("expected type name but got " + t.describe());
    }
    if (base_const) spec->is_const = true;
    return type_suffixes(std::move(spec), t);
}

TypeSpecPtr Parser::parse_type_base() {
    bool base_const = false;
    if (at_kw("const")) {
        advance();
        base_const = true;
    }
    const Token t = peek();
    TypeSpecPtr spec;
    if (t.kind == TokKind::Kw && prims().count(t.text) != 0) {
        advance();
        spec = make_named_spec(t.text, t.line, t.col);
    } else if (t.kind == TokKind::Ident) {
        advance();
        std::string name = t.text;
        while (at_op(".")) {
            advance();
            name += ".";
            name += expect_ident("type name").text;
        }
        spec = make_named_spec(name, t.line, t.col);
    } else {
        throw err("expected type name but got " + t.describe());
    }
    if (base_const) spec->is_const = true;
    while (at_op("*")) {
        const Token tok = advance();
        auto ptr = std::make_unique<TypeSpec>();
        ptr->kind = TypeSpec::K::Ptr;
        ptr->elem = std::move(spec);
        ptr->line = tok.line;
        ptr->col = tok.col;
        spec = std::move(ptr);
    }
    return spec;
}

TypeSpecPtr Parser::type_suffixes(TypeSpecPtr spec, const Token& t) {
    for (;;) {
        if (at_op("*")) {
            const Token tok = advance();
            auto ptr = std::make_unique<TypeSpec>();
            ptr->kind = TypeSpec::K::Ptr;
            ptr->elem = std::move(spec);
            ptr->line = tok.line;
            ptr->col = tok.col;
            spec = std::move(ptr);
            continue;
        }
        if (at_op("[")) {
            const Token tok = advance();
            if (at_op("]")) {
                advance();
                auto slice = std::make_unique<TypeSpec>();
                slice->kind = TypeSpec::K::Slice;
                slice->elem = std::move(spec);
                slice->rank = 1;
                slice->line = tok.line;
                slice->col = tok.col;
                spec = std::move(slice);
                continue;
            }
            if (at_op(",")) {
                int rank = 1;
                while (at_op(",")) {
                    advance();
                    rank++;
                }
                expect_op("]");
                auto slice = std::make_unique<TypeSpec>();
                slice->kind = TypeSpec::K::Slice;
                slice->elem = std::move(spec);
                slice->rank = rank;
                slice->line = tok.line;
                slice->col = tok.col;
                spec = std::move(slice);
                continue;
            }
            std::vector<size_t> dims;
            for (;;) {
                const Token n = peek();
                if (n.kind != TokKind::Int) throw err("array length only supports integer literals for now");
                advance();
                if (n.ival <= 0) throw err_at("array length must be greater than 0", n);
                dims.push_back(static_cast<size_t>(n.ival));
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
            expect_op("]");
            // 反向包装：int[2,3] -> Array(Array(int,3),2)
            for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
                auto arr = std::make_unique<TypeSpec>();
                arr->kind = TypeSpec::K::Array;
                arr->elem = std::move(spec);
                arr->size = *it;
                arr->line = tok.line;
                arr->col = tok.col;
                spec = std::move(arr);
            }
            continue;
        }
        break;
    }

    return spec;
}

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

// ---------------------------------------------------------------- 表达式

ExprPtr Parser::make_binary(const std::string& op, ExprPtr left, ExprPtr right,
                            const Token& tok) {
    auto node = std::make_unique<Binary>();
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    node->line = tok.line;
    node->col = tok.col;
    return node;
}

ExprPtr Parser::parse_ternary() {
    ExprPtr cond = parse_or();
    if (!at_op("?")) return cond;
    const Token tok = advance();
    auto node = std::make_unique<Ternary>();
    node->cond = std::move(cond);
    node->then_e = parse_ternary();  // 中间分支允许嵌套 `?:`
    if (!at_op(":")) throw err_at("expected `:` after `?`", peek());
    advance();
    node->else_e = parse_ternary();  // 右结合：`a ? b : c ? d : e` = `a ? b : (c ? d : e)`
    node->line = tok.line;
    node->col = tok.col;
    return node;
}

ExprPtr Parser::parse_or() {
    ExprPtr left = parse_and();
    while (at_kw("or")) {
        const Token tok = advance();
        ExprPtr right = parse_and();
        left = make_binary("or", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_and() {
    ExprPtr left = parse_not();
    while (at_kw("and")) {
        const Token tok = advance();
        ExprPtr right = parse_not();
        left = make_binary("and", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_not() {
    if (at_kw("not")) {
        const Token tok = advance();
        ExprPtr operand = parse_not();
        auto node = std::make_unique<NotExpr>();
        node->operand = std::move(operand);
        node->line = tok.line;
        node->col = tok.col;
        return node;
    }
    return parse_eq();
}

ExprPtr Parser::parse_eq() {
    ExprPtr left = parse_rel();
    while (at_op("==", "!=")) {
        const Token tok = advance();
        ExprPtr right = parse_rel();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_rel() {
    ExprPtr left = parse_bitor();
    while (at_op("<", "<=", ">", ">=")) {
        const Token tok = advance();
        ExprPtr right = parse_bitor();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitor() {
    ExprPtr left = parse_bitxor();
    while (at_op("|")) {
        const Token tok = advance();
        ExprPtr right = parse_bitxor();
        left = make_binary("|", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitxor() {
    ExprPtr left = parse_bitand();
    while (at_op("^")) {
        const Token tok = advance();
        ExprPtr right = parse_bitand();
        left = make_binary("^", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitand() {
    ExprPtr left = parse_shift();
    while (at_op("&")) {
        const Token tok = advance();
        ExprPtr right = parse_shift();
        left = make_binary("&", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_shift() {
    ExprPtr left = parse_add();
    while (at_op("<<", ">>")) {
        const Token tok = advance();
        ExprPtr right = parse_add();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_add() {
    ExprPtr left = parse_mul();
    while (at_op("+", "-")) {
        const Token tok = advance();
        ExprPtr right = parse_mul();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_mul() {
    ExprPtr left = parse_unary();
    while (at_op("*", "/", "%")) {
        const Token tok = advance();
        ExprPtr right = parse_unary();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_unary() {
    const Token t = peek();
    if (at_kw("new")) {
        advance();
        auto ne = std::make_unique<NewExpr>();
        ne->type = parse_type_base();
        ne->line = t.line;
        ne->col = t.col;
        if (at_op("[")) {
            const Token lb = advance();
            if (at_op("]")) throw err_at("`new` dynamic array requires a length", lb);
            for (;;) {
                ne->dims.push_back(parse_expr());
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
            expect_op("]");
            // new T[m][]：外层数组的元素是 T[]（空槽，稍后逐行 new）
            while (at_op("[")) {
                if (!peek(1).is_op("]")) {
                    throw err_at("`new T[m][n]` is not jagged array syntax: use `new T[m,n]` for rectangular arrays; for jagged arrays, allocate the outer array first and fill the rows in a loop", peek());
                }
                advance();
                advance();
                ne->jagged_rank++;
            }
            ne->is_array = true;
            if (at_op("(")) throw err_at("dynamic array construction does not support constructor arguments", lb);
            return parse_postfix_tail(std::move(ne));
        }
        if (at_op("(")) {
            advance();
            while (!at_op(")")) {
                if (peek().kind == TokKind::Ident && peek(1).is_op("=")) {
                    std::string nm = advance().text;
                    advance();
                    ne->named.emplace_back(std::move(nm), parse_expr());
                } else {
                    ne->args.push_back(parse_expr());
                    forbid_inc_expr();
                }
                if (at_op(",")) {
                    advance();
                } else {
                    break;
                }
            }
            expect_op(")");
            if (!ne->args.empty() && !ne->named.empty())
                throw err_at("positional and named arguments cannot be mixed", t);
        }
        return parse_postfix_tail(std::move(ne));
    }
    if (at_op("!")) {
        throw err("`!` is no longer used: use `not` for logical negation and `~` for bitwise negation");
    }
    if (t.kind == TokKind::Op && at_op("-", "+", "&", "*", "~")) {
        advance();
        ExprPtr operand = parse_unary();
        auto node = std::make_unique<Unary>();
        node->op = t.text;
        node->operand = std::move(operand);
        node->line = t.line;
        node->col = t.col;
        return parse_postfix_tail(std::move(node));
    }
    if (at_kw("sizeof", "alignof")) {
        std::string which = advance().text;
        expect_op("(");
        TypeSpecPtr spec = parse_type();
        expect_op(")");
        auto node = std::make_unique<SizeofExpr>();
        node->spec = std::move(spec);
        node->which = std::move(which);
        node->line = t.line;
        node->col = t.col;
        return parse_postfix_tail(std::move(node));
    }
    if (at_kw("len")) {
        advance();
        expect_op("(");
        ExprPtr seq = parse_expr();
        ExprPtr dim;
        if (at_op(",")) {
            advance();
            dim = parse_expr();
        }
        expect_op(")");
        auto node = std::make_unique<Len>();
        node->seq = std::move(seq);
        node->dim = std::move(dim);
        node->line = t.line;
        node->col = t.col;
        return parse_postfix_tail(std::move(node));
    }
    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    return parse_postfix_tail(parse_primary());
}

ExprPtr Parser::parse_postfix_tail(ExprPtr e) {
    for (;;) {
        const Token t = peek();
        if (t.is_op("(")) {
            e = parse_call(std::move(e));
            continue;
        }
        if (t.is_op("[")) {
            advance();
            auto node = std::make_unique<Index>();
            for (;;) {
                node->indices.push_back(parse_expr());
                forbid_inc_expr();
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
            expect_op("]");
            node->base = std::move(e);
            node->line = t.line;
            node->col = t.col;
            e = std::move(node);
            continue;
        }
        if (t.is_op(".")) {
            // 多段 unit 名：`lib.a.b.f` 中 `lib.a.b` 是 unit，合并成单个 Name，
            // 避免被拆成三层成员访问。按最长已导入前缀匹配。
            if (auto* n = dynamic_cast<Name*>(e.get());
                n && peek(1).kind == TokKind::Ident) {
                const std::string cand = n->ident + "." + peek(1).text;
                if (module_names_.count(cand)) {
                    advance();  // '.'
                    advance();  // 成员名
                    n->ident = cand;
                    continue;
                }
            }
            advance();
            std::string nm = expect_ident("member name").text;
            auto node = std::make_unique<Member>();
            node->obj = std::move(e);
            node->name = std::move(nm);
            node->line = t.line;
            node->col = t.col;
            e = std::move(node);
            continue;
        }
        if (t.is_kw("as")) {
            advance();
            TypeSpecPtr spec = parse_type();
            auto node = std::make_unique<Cast>();
            node->expr = std::move(e);
            node->spec = std::move(spec);
            node->line = t.line;
            node->col = t.col;
            e = std::move(node);
            continue;
        }
        break;
    }
    return e;
}

ExprPtr Parser::parse_call(ExprPtr callee) {
    const Token t = expect_op("(");
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    while (!at_op(")")) {
        if (peek().kind == TokKind::Ident && peek(1).is_op("=")) {
            std::string nm = advance().text;
            advance();
            named.emplace_back(std::move(nm), parse_expr());
        } else {
            args.push_back(parse_expr());
            forbid_inc_expr();
        }
        if (at_op(",")) {
            advance();
        } else {
            break;
        }
    }
    expect_op(")");
    if (!args.empty() && !named.empty()) throw err_at("positional and named arguments cannot be mixed", t);
    auto node = std::make_unique<Call>();
    node->callee = std::move(callee);
    node->args = std::move(args);
    node->named = std::move(named);
    node->line = t.line;
    node->col = t.col;
    return node;
}

ExprPtr Parser::parse_primary() {
    const Token t = peek();
    if (t.kind == TokKind::Kw && (t.text == "this" || t.text == "super")) {
        advance();
        auto node = std::make_unique<Name>();
        node->ident = t.text;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Int) {
        advance();
        auto node = std::make_unique<IntLit>();
        node->value = t.ival;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Float) {
        advance();
        auto node = std::make_unique<FloatLit>();
        node->value = t.fval;
        node->is_float32 = t.f32;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Char) {
        advance();
        auto node = std::make_unique<CharLit>();
        node->value = static_cast<int>(t.ival);
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Str) {
        advance();
        auto node = std::make_unique<StringLit>();
        node->value = t.bytes;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Kw && (t.text == "true" || t.text == "false")) {
        advance();
        auto node = std::make_unique<BoolLit>();
        node->value = t.text == "true";
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Kw && t.text == "null") {
        advance();
        auto node = std::make_unique<NullLit>();
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Kw && (t.text == "alloc" || t.text == "realloc" || t.text == "free")) {
        advance();
        auto node = std::make_unique<Name>();
        node->ident = t.text;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Ident) {
        advance();
        auto node = std::make_unique<Name>();
        node->ident = t.text;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Op && t.text == "(") {
        advance();
        ExprPtr e = parse_expr();
        if (at_op("++", "--")) {
            throw err("`++`/`--` has no value and cannot appear in an expression (only as a standalone statement)");
        }
        expect_op(")");
        return e;
    }
    throw err("unexpected " + t.describe());
}

}  // namespace

// ---------------------------------------------------------------- 对外接口

std::unique_ptr<Unit> parse_source(const std::string& src, const std::string& filename) {
    Parser parser(src, filename);
    return parser.parse_unit();
}

std::unique_ptr<Unit> parse_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw MuError("cannot read or write file " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_source(ss.str(), path);
}
