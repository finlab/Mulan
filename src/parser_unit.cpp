// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing — units and declarations
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
        // layout modifiers (packed / align(N)): same category as public/private, any order, composable
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
            // `align` keeps its identifier identity; only the `align(` form is a layout modifier (contextual keyword)
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

        // contextual keyword `type`: `type Ident = <type>;` (transparent type alias, exactly one definition per declaration)
        if (peek().kind == TokKind::Ident && peek().text == "type" && peek(1).kind == TokKind::Ident &&
            peek(2).is_op("=")) {
            if (has_layout) {
                throw err_at("layout modifier `packed` / `align(N)` can only be used in a `struct` or `union` declaration",
                             layout_tok);
            }
            advance();  // type
            auto d = std::make_unique<TypeAliasDecl>();
            const Token name_tok = expect_ident("type alias name");
            d->name = name_tok.text;
            d->line = name_tok.line;
            d->col = name_tok.col;
            expect_op("=");
            d->spec = parse_type();
            if (at_op(",")) {
                throw err_at("`type` declares exactly one alias; write a separate `type` declaration for each", name_tok);
            }
            if (at_op("{")) {
                throw err_at("`type` defines an alias, not a type: write `type " + d->name + " = <type>;`", name_tok);
            }
            semicolon();
            d->is_public = is_pub;
            unit->decls.push_back(std::move(d));
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

        // function definition or unit-level variable
        bool ret_ref = false;
        TypeSpecPtr spec = parse_return_type(ret_ref);
        if (at_kw("operator")) {
            auto f = parse_operator_func(std::move(spec), ret_ref, tk);
            f->is_public = is_pub;
            unit->decls.push_back(std::move(f));
            continue;
        }
        if (peek().kind == TokKind::Ident && (peek(1).is_op("(") || peek(1).is_op("<"))) {
            const Token name_tok = advance();
            std::vector<TypeParam> tps = parse_type_params();
            const std::unordered_set<std::string> saved_type_names = type_names_;
            for (const auto& p : tps) type_names_.insert(p.name);
            auto f = parse_func(name_tok.text, std::move(spec), ret_ref, name_tok);
            f->type_params = std::move(tps);
            type_names_ = saved_type_names;
            f->is_public = is_pub;
            unit->decls.push_back(std::move(f));
        } else {
            if (ret_ref) {
                throw err_at("`ref` can only modify a return type (`ref T f(...)`); a variable cannot have a reference type",
                             tk);
            }
            // unit-level variable: private by default; `public` comes from the prefix
            auto globals = parse_global_list(std::move(spec), false);
            for (auto& g : globals) g->is_public = is_pub;
            for (auto& g : globals) unit->decls.push_back(std::move(g));
        }
    }

    return unit;
}

std::unique_ptr<Decl> Parser::parse_extern(const Token& tk) {
    bool ret_ref = false;
    TypeSpecPtr spec = parse_return_type(ret_ref);
    const Token name_tok = expect_ident("external symbol name");
    if (at_op("(")) {
        if (ret_ref) throw err_at("`extern` functions cannot return a reference (`ref` borrows are a language mechanism)", tk);
        auto fn = std::make_unique<FuncDecl>();
        fn->name = name_tok.text;
        fn->ret = std::move(spec);
        fn->params = parse_params(&fn->is_varargs, /*allow_ref=*/true);
        fn->is_extern = true;
        fn->is_public = true;  // `extern` declarations are implicitly public (the parser already rejects combining them with `public`)
        fn->line = tk.line;
        fn->col = tk.col;
        semicolon();
        return fn;
    }
    auto gv = std::make_unique<GlobalVar>();
    if (ret_ref) throw err_at("`ref` can only modify a return type; an extern variable cannot have a reference type", tk);
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
    std::vector<TypeParam> tps = parse_type_params();
    const std::unordered_set<std::string> saved_type_names = type_names_;
    for (const auto& p : tps) type_names_.insert(p.name);
    if (is_union && at_op(":")) throw err("union cannot implement interfaces");
    std::vector<TypeSpecPtr> base_specs;
    std::vector<std::string> bases = parse_base_list(&base_specs);
    expect_op("{");

    auto decl = std::make_unique<StructDecl>();
    decl->name = name;
    decl->bases = std::move(bases);
    decl->base_specs = std::move(base_specs);
    decl->type_params = std::move(tps);
    decl->line = tk.line;
    decl->col = tk.col;
    decl->is_union = is_union;

    bool cur_pub = true;  // struct defaults to public
    while (!at_op("}")) {
        // section label: `public:` / `private:` switches the default visibility of following members
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
        // `type` is not an alias declaration in this position (aliases can only be written at unit level); give a clear diagnostic
        if (peek().kind == TokKind::Ident && peek().text == "type" && peek(1).kind == TokKind::Ident &&
            peek(2).is_op("=")) {
            throw err_at("type aliases must be declared at unit scope (outside `" + std::string(kw) + " " + name + "`)",
                         peek());
        }
        bool prefix_override = false;
        if (at_kw("override")) {
            advance();
            prefix_override = true;
        }
        bool ret_ref = false;
        TypeSpecPtr spec = parse_return_type(ret_ref);
        if (at_kw("operator")) {
            if (is_union) throw err("union `" + name + "` cannot have methods");
            decl->methods.push_back(parse_member_operator(std::move(spec), ret_ref));
            decl->methods.back()->is_public = member_pub;
            continue;
        }
        const Token name_tok = expect_ident("field or method name");
        if (at_op("(")) {
            if (is_union) throw err("union `" + name + "` cannot have methods");
            decl->methods.push_back(parse_method(std::move(spec), ret_ref, name_tok));
            decl->methods.back()->is_public = member_pub;
            if (prefix_override) decl->methods.back()->is_override = true;
            continue;
        }
        if (prefix_override) {
            throw err_at("`override` can only be used on a method", name_tok);
        }
        if (ret_ref) {
            throw err_at("`ref` can only modify a method return type; a field cannot have a reference type", name_tok);
        }
        if (at_op(":")) {
            throw err_at("bit-fields are not supported: use explicit shifts/masks or a `byte[N]` field with offset constants",
                         name_tok);
        }
        if (spec->is_const) {
            // type-level compile-time constant: `const T NAME = <const-expr>;`
            if (is_union) {
                throw err_at("union `" + name + "` does not support const members (union members are always public fields)",
                             name_tok);
            }
            auto cm = std::make_unique<ConstMember>();
            cm->name = name_tok.text;
            spec->is_const = false;  // `const` is a declaration marker, not a type qualifier
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
            const Token extra = expect_ident("field name");
            if (at_op(":")) {
                throw err_at(
                    "bit-fields are not supported: use explicit shifts/masks or a `byte[N]` field with offset constants",
                    extra);
            }
            std::string fname = extra.text;
            decl->field_visibility[fname] = member_pub;
            decl->fields.emplace_back(std::move(fname), clone_spec(*spec));
        }
        semicolon();
    }
    expect_op("}");
    if (at_op(";")) advance();
    type_names_ = saved_type_names;
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

// type syntax → display string (only for placeholder names of generic base classes in the M1 stage)
static std::string spec_display(const TypeSpec& s) {
    switch (s.kind) {
        case TypeSpec::K::Named: {
            std::string n = s.name;
            if (!s.args.empty()) {
                n += "<";
                for (size_t i = 0; i < s.args.size(); ++i) {
                    if (i) n += ", ";
                    n += spec_display(*s.args[i]);
                }
                n += ">";
            }
            return n;
        }
        case TypeSpec::K::Ptr:
            return spec_display(*s.elem) + "*";
        case TypeSpec::K::Slice:
            return spec_display(*s.elem) + "[]";
        case TypeSpec::K::Array:
            return spec_display(*s.elem) + "[" + std::to_string(s.size) + "]";
        case TypeSpec::K::Func: {
            std::string n = "(";
            for (size_t i = 0; i < s.params.size(); ++i) {
                if (i) n += ", ";
                n += spec_display(*s.params[i]);
            }
            n += ") => " + spec_display(*s.ret);
            return n;
        }
    }
    return s.name;
}

// `Self` inside an interface body is marked by `TypeSpec::is_self`; sema resolves it to the interface type and substitutes it during constraint checking.

std::vector<std::string> Parser::parse_base_list(std::vector<TypeSpecPtr>* specs) {
    std::vector<std::string> bases;
    if (!at_op(":")) return bases;
    advance();
    for (;;) {
        const Token nt = peek();
        std::string n = parse_qualified_name();
        auto bs = std::make_unique<TypeSpec>();
        bs->kind = TypeSpec::K::Named;
        bs->name = n;
        bs->line = nt.line;
        bs->col = nt.col;
        if (at_op("<")) {
            // generic base class (`class Box<T> : Base<T>`): the string falls back to concatenated display names,
            // structured arguments go into base_specs (type parameters are substituted at instantiation).
            std::vector<std::unique_ptr<TypeSpec>> args = parse_type_args();
            n += "<";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) n += ", ";
                n += spec_display(*args[i]);
                bs->args.push_back(std::move(args[i]));
            }
            n += ">";
        }
        bases.push_back(std::move(n));
        if (specs) specs->push_back(std::move(bs));
        if (at_op(",")) {
            advance();
            continue;
        }
        break;
    }
    return bases;
}

std::unique_ptr<MethodDecl> Parser::parse_method(TypeSpecPtr ret, bool ret_ref, const Token& name_tok) {
    auto m = std::make_unique<MethodDecl>();
    m->name = name_tok.text;
    m->ret = std::move(ret);
    m->ret_ref = ret_ref;
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
    std::vector<TypeParam> tps = parse_type_params();
    const std::unordered_set<std::string> saved_type_names = type_names_;
    for (const auto& p : tps) type_names_.insert(p.name);

    auto cd = std::make_unique<ClassDecl>();
    cd->name = nm.text;
    cd->type_params = std::move(tps);
    cd->line = tk.line;
    cd->col = tk.col;
    std::vector<TypeSpecPtr> base_specs;
    cd->bases = parse_base_list(&base_specs);
    cd->base_specs = std::move(base_specs);

    expect_op("{");
    bool cur_pub = false;  // class defaults to private
    while (!at_op("}")) {
        // section label: `public:` / `private:` switches the default visibility of following members
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
        bool ret_ref = false;
        TypeSpecPtr spec = parse_return_type(ret_ref);
        if (prefix_weak) spec->weak = true;
        if (at_kw("operator")) {
            throw err("member operators are only allowed in a `struct`: operators are static dispatch and a class "
                      "operator would be virtual (write a free function `T operator+(C a, C b)` instead)");
        }
        const Token name_tok = expect_ident("member name");
        if (at_op("(")) {
            if (prefix_weak) throw err_at("`weak` can only be used on fields, not on methods", name_tok);
            auto m = parse_method(std::move(spec), ret_ref, name_tok);
            if (prefix_override) m->is_override = true;
            m->is_public = member_pub;
            cd->methods.push_back(std::move(m));
            continue;
        }
        if (ret_ref) {
            throw err_at("`ref` can only modify a method return type; a field cannot have a reference type", name_tok);
        }
        if (prefix_override) throw err_at("`override` can only be used on methods", name_tok);
        if (spec->is_const) {
            // type-level compile-time constant: `const T NAME = <const-expr>;`
            if (prefix_weak) throw err_at("`weak` can only be used on fields", name_tok);
            auto cm = std::make_unique<ConstMember>();
            cm->name = name_tok.text;
            spec->is_const = false;  // `const` is a declaration marker, not a type qualifier
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
    type_names_ = saved_type_names;
    return cd;
}

std::unique_ptr<Decl> Parser::parse_interface() {
    const Token tk = expect_kw("interface");
    const Token nm = expect_ident("interface name");
    type_names_.insert(nm.text);
    std::vector<TypeParam> tps = parse_type_params();
    const std::unordered_set<std::string> saved_type_names = type_names_;
    for (const auto& p : tps) type_names_.insert(p.name);

    auto id = std::make_unique<InterfaceDecl>();
    id->name = nm.text;
    id->type_params = std::move(tps);
    id->line = tk.line;
    id->col = tk.col;
    std::vector<TypeSpecPtr> base_specs;
    id->bases = parse_base_list(&base_specs);
    id->base_specs = std::move(base_specs);

    expect_op("{");
    while (!at_op("}")) {
        if (at_kw("public", "private")) {
            throw err("`public`/`private` cannot be used on interface methods (interface methods are always public)");
        }
        if (at_kw("const")) {
            throw err("interface `" + id->name + "` does not support const members");
        }
        bool ret_ref = false;
        TypeSpecPtr spec = parse_return_type(ret_ref);
        if (at_kw("operator")) {
            id->methods.push_back(parse_interface_operator(std::move(spec), ret_ref));
            continue;
        }
        const Token name_tok = expect_ident("method name");
        id->methods.push_back(parse_method(std::move(spec), ret_ref, name_tok));
    }
    expect_op("}");
    if (at_op(";")) advance();
    type_names_ = saved_type_names;
    return id;
}

TypeSpecPtr Parser::parse_return_type(bool& ret_ref) {
    ret_ref = false;
    if (at_kw("ref")) {
        advance();
        ret_ref = true;
    }
    return parse_type();
}

std::unique_ptr<MethodDecl> Parser::parse_interface_operator(TypeSpecPtr ret, bool ret_ref) {
    const Token tk = expect_kw("operator");
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
        throw err("expected operator symbol after `operator`");
    }
    auto m = std::make_unique<MethodDecl>();
    m->name = "operator" + sym;
    m->ret = std::move(ret);
    m->ret_ref = ret_ref;
    m->is_operator = true;
    m->line = tk.line;
    m->col = tk.col;
    m->params = parse_params();
    semicolon();
    return m;
}

// Member operator inside a struct body (3.11): `T operator+(B other) { ... }`.
// The receiver is the implicit `this` (a borrowed pointer), so `s[i]` / `s == t` cost no copy and no RC.
std::unique_ptr<MethodDecl> Parser::parse_member_operator(TypeSpecPtr ret, bool ret_ref) {
    const Token tk = expect_kw("operator");
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
        throw err("expected operator symbol after `operator`");
    }
    auto m = std::make_unique<MethodDecl>();
    m->name = "operator" + sym;
    m->ret = std::move(ret);
    m->ret_ref = ret_ref;
    m->is_operator = true;
    m->line = tk.line;
    m->col = tk.col;
    m->params = parse_params();
    if (at_op(";")) {
        advance();
    } else {
        m->body = parse_block();
    }
    return m;
}

std::unique_ptr<FuncDecl> Parser::parse_operator_func(TypeSpecPtr ret, bool ret_ref, const Token& tk) {
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
    f->ret_ref = ret_ref;
    f->line = tk.line;
    f->col = tk.col;
    f->is_operator = true;
    f->params = parse_params(nullptr, /*allow_ref=*/true);
    f->body = parse_block();
    return f;
}
