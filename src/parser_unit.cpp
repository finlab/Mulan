// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 语法分析 —— unit 与声明
#include "parser_impl.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace muc_detail;

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
        if (spec->is_const) {
            // 类型级编译期常量：`const T NAME = <const-expr>;`
            if (is_union) {
                throw err_at("union `" + name + "` does not support const members (union members are always public fields)",
                             name_tok);
            }
            auto cm = std::make_unique<ConstMember>();
            cm->name = name_tok.text;
            spec->is_const = false;  // `const` 是声明标记，不是类型限定
            cm->spec = std::move(spec);
            cm->is_public = member_pub;
            cm->line = name_tok.line;
            cm->col = name_tok.col;
            if (!at_op("=")) {
                throw err_at("const member `" + cm->name + "` must be initialized", name_tok);
            }
            advance();
            cm->init = parse_expr();
            if (at_op(",")) {
                throw err("const member `" + cm->name +
                          "` must be declared one per statement (write a separate `const` declaration for each)");
            }
            semicolon();
            decl->const_members.push_back(std::move(cm));
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
        if (spec->is_const) {
            // 类型级编译期常量：`const T NAME = <const-expr>;`
            if (prefix_weak) throw err_at("`weak` can only be used on fields", name_tok);
            auto cm = std::make_unique<ConstMember>();
            cm->name = name_tok.text;
            spec->is_const = false;  // `const` 是声明标记，不是类型限定
            cm->spec = std::move(spec);
            cm->is_public = member_pub;
            cm->line = name_tok.line;
            cm->col = name_tok.col;
            if (!at_op("=")) {
                throw err_at("const member `" + cm->name + "` must be initialized", name_tok);
            }
            advance();
            cm->init = parse_expr();
            if (at_op(",")) {
                throw err("const member `" + cm->name +
                          "` must be declared one per statement (write a separate `const` declaration for each)");
            }
            semicolon();
            cd->const_members.push_back(std::move(cm));
            continue;
        }
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
        if (at_kw("const")) {
            throw err("interface `" + id->name + "` does not support const members");
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
