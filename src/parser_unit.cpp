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

void Parser::push_decl(std::unique_ptr<Decl> d) {
    if (!cur_decls_) throw MuError("internal: declaration outside a declaration list");
    cur_decls_->push_back(std::move(d));
}

namespace {

// `const struct` / `const class`: every field and method declared in the body is implicitly const,
// including the members of a conditional arm (the arm is selected later, by the semantic pass).
void mark_body_const(MemberGroup& m) {
    for (auto& f : m.fields) {
        if (f.second) f.second->is_const = true;
    }
    for (auto& mm : m.methods) mm->is_const = true;
    for (auto& g : m.groups) {
        for (auto& arm : g.dir->arms) mark_body_const(arm.members);
    }
}

}  // namespace

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

    const size_t g0 = groups_.size();
    cur_decls_ = &unit->decls;
    while (!at_eof()) {
        if (at_op("}") && groups_.size() > g0) { directive_arm_end(); continue; }
        if (at_directive()) { directive_header(); continue; }
        const Token tk = peek();
        bool is_pub = false;
        bool is_export = false;
        bool saw_private = false;
        Token export_tok = tk;
        // layout modifiers (packed / align(N)): same category as public/private, any order, composable
        bool packed = false;
        bool nocopy = false;
        Token nocopy_tok = tk;
        bool is_const_decl = false;
        Token const_tok = tk;
        bool has_builtin_decl = false;
        Token builtin_tok = tk;
        ExprPtr align_expr;
        bool has_layout = false;
        Token layout_tok = tk;
        for (;;) {
            if (at_kw("public", "private")) {
                is_pub = at_kw("public");
                if (!is_pub) saw_private = true;
                advance();
                continue;
            }
            if (at_kw("export")) {
                if (is_export) throw err_at("duplicate modifier `export`", peek());
                is_export = true;
                export_tok = peek();
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
            // `nocopy` is not a layout modifier: it only changes the copy semantics, so it is rejected
            // everywhere a value type cannot appear rather than lumped in with `packed` / `align(N)`.
            if (at_kw("nocopy")) {
                if (nocopy) throw err_at("duplicate modifier `nocopy`", peek());
                nocopy = true;
                nocopy_tok = peek();
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
            // `const struct S` / `const class C` is a declaration modifier (decided 2026-11-17): every
            // field and method is implicitly const. It is distinguished from a `const T` variable
            // declaration by what follows, which may be other modifiers (`const nocopy struct S`).
            if (at_kw("const") && type_decl_after_modifiers(1)) {
                if (is_const_decl) throw err_at("duplicate modifier `const`", peek());
                is_const_decl = true;
                const_tok = peek();
                advance();
                continue;
            }
            // `builtin const T name;` / `builtin T f(...);` (9.3)
            if (at_kw("builtin")) {
                if (has_builtin_decl) throw err_at("duplicate modifier `builtin`", peek());
                has_builtin_decl = true;
                builtin_tok = peek();
                advance();
                continue;
            }
            break;
        }
        if (at_eof()) throw err_at("expected declaration after modifier", tk);
        if (is_export && saw_private) {
            throw err_at("`export` implies `public`; `private` cannot be combined with `export`", export_tok);
        }
        if (is_export) is_pub = true;
        // `nocopy` only makes sense on a value type: a class is a heap handle (copying it shares), an
        // interface has no storage, a union is copied by bits, and the rest are not type declarations.
        if (nocopy && !(at_kw("struct") || at_kw("union"))) {
            throw err_at("`nocopy` can only modify a `struct` declaration", nocopy_tok);
        }

        if (has_builtin_decl) {
            if (has_layout) {
                throw err_at("`builtin` cannot be combined with a layout modifier", builtin_tok);
            }
            auto d = parse_builtin(builtin_tok);
            d->is_public = is_pub;
            d->is_export = is_export;
            push_decl(std::move(d));
            continue;
        }

        if (at_kw("use")) {
            if (is_pub) throw err_at("`public` cannot be used with `use`", tk);
            if (is_export) throw err_at("`export` cannot be used with `use`", export_tok);
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
            if (!groups_.empty() && groups_.back().decl_mode && cur_decls_) {
                // The arm may not be selected: the semantic pass decides and then adds the `use`
                // (P3.3). The dependency is still discovered by the loader's candidate scan.
                module_names_.insert(use->unit_name);
                push_decl(std::move(use));
            } else {
                module_names_.insert(use->unit_name);
                unit->uses.push_back(std::move(use));
            }
            continue;
        }
        if (at_kw("extern")) {
            if (is_export) throw err_at("`extern` imports a C symbol; `export` cannot be used with it", export_tok);
            if (is_pub) throw err_at("`extern` declarations are implicitly public; `public` cannot be used", tk);
            if (has_layout) {
                throw err_at("layout modifier `packed` / `align(N)` can only be used in a `struct` or `union` declaration", layout_tok);
            }
            advance();
            push_decl(parse_extern(tk));
            continue;
        }

        // contextual keyword `type`: `type Ident = <type>;` (transparent type alias) or
        // `type Ident = Alt | Alt(T);` (sum type), optionally with type parameters for a sum.
        if (peek().kind == TokKind::Ident && peek().text == "type" && peek(1).kind == TokKind::Ident &&
            (peek(2).is_op("=") || peek(2).is_op("<"))) {
            if (has_layout) {
                throw err_at("layout modifier `packed` / `align(N)` can only be used in a `struct` or `union` declaration",
                             layout_tok);
            }
            advance();  // type
            const Token name_tok = expect_ident("type alias name");
            std::vector<TypeParam> type_params;
            if (at_op("<")) type_params = parse_type_params();
            expect_op("=");
            // `type X = A | B(T) | C;` declares a **sum type** (SPEC 10.1); everything else stays a
            // transparent alias.
            bool is_sum = false;
            {
                const std::size_t save = pos_;
                if (peek().kind == TokKind::Ident) {
                    advance();
                    if (at_op("(")) {  // skip the payload list
                        int depth = 0;
                        while (pos_ < toks_.size()) {
                            if (toks_[pos_].kind == TokKind::Op && toks_[pos_].text == "(") depth++;
                            if (toks_[pos_].kind == TokKind::Op && toks_[pos_].text == ")") {
                                depth--;
                                if (depth == 0) { advance(); break; }
                            }
                            if (toks_[pos_].is_op(";")) break;
                            advance();
                        }
                    }
                    is_sum = at_op("|");
                }
                pos_ = save;
            }
            if (is_sum) {
                auto sd = std::make_unique<SumDecl>();
                sd->name = name_tok.text;
                sd->line = name_tok.line;
                sd->col = name_tok.col;
                for (;;) {
                    SumAlt alt;
                    const Token at = expect_ident("alternative name");
                    alt.name = at.text;
                    alt.line = at.line;
                    alt.col = at.col;
                    if (at_op("(")) {
                        advance();
                        if (!at_op(")")) {
                            for (;;) {
                                alt.fields.push_back(parse_type());
                                if (at_op(",")) {
                                    advance();
                                    continue;
                                }
                                break;
                            }
                        }
                        expect_op(")");
                    }
                    sd->alts.push_back(std::move(alt));
                    if (at_op("|")) {
                        advance();
                        continue;
                    }
                    break;
                }
                semicolon();
                sd->is_public = is_pub;
                sd->is_export = is_export;
                sd->type_params = std::move(type_params);
                type_names_.insert(sd->name);
                push_decl(std::move(sd));
                continue;
            }
            if (!type_params.empty()) {
                throw err_at("generic type aliases are not supported; only sum types take type "
                             "parameters (`type Option<T> = Some(T) | None;`)",
                             name_tok);
            }
            auto d = std::make_unique<TypeAliasDecl>();
            d->name = name_tok.text;
            d->line = name_tok.line;
            d->col = name_tok.col;
            d->spec = parse_type();
            if (at_op(",")) {
                throw err_at("`type` declares exactly one alias; write a separate `type` declaration for each", name_tok);
            }
            if (at_op("{")) {
                throw err_at("`type` defines an alias, not a type: write `type " + d->name + " = <type>;`", name_tok);
            }
            semicolon();
            d->is_public = is_pub;
            d->is_export = is_export;
            // The name is a type from here on: `is_type_start` must recognize it so that `sizeof(Op)` and
            // `Op x;` parse as a type rather than as an expression.
            type_names_.insert(d->name);
            push_decl(std::move(d));
            continue;
        }

        if (at_kw("struct") || at_kw("union")) {
            const bool is_union = at_kw("union");
            if (nocopy && is_union) throw err_at("`union` does not support `nocopy` (a union is copied by bits)", nocopy_tok);
            auto d = parse_struct(is_union);
            d->is_public = is_pub;
            d->is_export = is_export;
            if (auto* sd = dynamic_cast<StructDecl*>(d.get())) {
                sd->is_packed = packed;
                sd->is_union = is_union;
                sd->nocopy = nocopy;
                sd->align_expr = std::move(align_expr);
                if (is_const_decl) {
                    if (is_union) throw err_at("`const` cannot modify a `union` (members are always public fields)", const_tok);
                    mark_body_const(sd->body);
                }
            }
            push_decl(std::move(d));
            continue;
        }

        if (at_kw("enum")) {
            if (is_const_decl) throw err_at("`const` can only modify a `struct` or `class` declaration", const_tok);
            if (has_layout) {
                throw err_at("`enum` does not support layout modifiers (`packed` / `align(N)`)", layout_tok);
            }
            auto d = parse_enum();
            d->is_public = is_pub;
            d->is_export = is_export;
            push_decl(std::move(d));
            continue;
        }

        if (at_kw("class")) {
            if (has_layout) {
                throw err_at("`class` does not support layout modifiers (`packed` / `align(N)`)", layout_tok);
            }
            auto d = parse_class();
            d->is_public = is_pub;
            d->is_export = is_export;
            if (is_const_decl) {
                if (auto* cd = dynamic_cast<ClassDecl*>(d.get())) mark_body_const(cd->body);
            }
            push_decl(std::move(d));
            continue;
        }

        if (at_kw("interface")) {
            if (is_const_decl) throw err_at("`const` can only modify a `struct` or `class` declaration", const_tok);
            if (has_layout) {
                throw err_at("`interface` does not support layout modifiers (`packed` / `align(N)`)", layout_tok);
            }
            auto d = parse_interface();
            d->is_public = is_pub;
            d->is_export = is_export;
            push_decl(std::move(d));
            continue;
        }

        if (has_layout) {
            throw err_at("layout modifier `packed` / `align(N)` can only be used in a `struct` or `union` declaration", layout_tok);
        }

        if (at_kw("static")) {
            throw err_at("`static` has been removed: unit-level variables are private by default; use `public` to share across units", tk);
        }

        if (at_kw("const")) {
            // `const` at unit level introduces either a const variable (`const int N = 3;`) or a
            // readonly return type (`const char* f() { ... }`). Both start with a type; an
            // identifier followed by `(` or `<` makes it a function definition.
            TypeSpecPtr spec = parse_type();
            if (peek().kind == TokKind::Ident && (peek(1).is_op("(") || peek(1).is_op("<"))) {
                const Token name_tok = advance();
                std::vector<TypeParam> tps = parse_type_params();
                const std::unordered_set<std::string> saved_type_names = type_names_;
                for (const auto& p : tps) type_names_.insert(p.name);
                auto f = parse_func(name_tok.text, std::move(spec), /*ret_ref=*/false, name_tok);
                f->type_params = std::move(tps);
                type_names_ = saved_type_names;
                f->is_public = is_pub;
                f->is_export = is_export;
                push_decl(std::move(f));
                continue;
            }
            // const variable: `const` is a declaration marker on a bare `const T`
            const bool is_const = spec->kind == TypeSpec::K::Named && spec->is_const;
            auto globals = parse_global_list(std::move(spec), is_const);
            for (auto& g : globals) g->is_public = is_pub;
            for (auto& g : globals) g->is_export = is_export;
            for (auto& g : globals) push_decl(std::move(g));
            continue;
        }

        if (at_kw("auto")) throw err("`auto` cannot be used for unit-level variables; an explicit type is required");

        // function definition or unit-level variable
        bool ret_ref = false;
        TypeSpecPtr spec = parse_return_type(ret_ref);
        if (at_kw("operator")) {
            auto f = parse_operator_func(std::move(spec), ret_ref, tk);
            f->is_public = is_pub;
            f->is_export = is_export;
            push_decl(std::move(f));
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
            f->is_export = is_export;
            push_decl(std::move(f));
        } else {
            if (ret_ref) {
                throw err_at("`ref` can only modify a return type (`ref T f(...)`); a variable cannot have a reference type",
                             tk);
            }
            // unit-level variable: private by default; `public` comes from the prefix
            auto globals = parse_global_list(std::move(spec), false);
            for (auto& g : globals) g->is_public = is_pub;
            for (auto& g : globals) g->is_export = is_export;
            for (auto& g : globals) push_decl(std::move(g));
        }
    }

    if (groups_.size() != g0) throw err_at("unterminated `#if` block: missing `}`", t);
    cur_decls_ = nullptr;
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

std::unique_ptr<Decl> Parser::parse_builtin(const Token& tk) {
    bool ret_ref = false;
    TypeSpecPtr spec = parse_return_type(ret_ref);
    const Token name_tok = expect_ident("builtin name");
    if (at_op("(")) {
        auto fn = std::make_unique<FuncDecl>();
        fn->name = name_tok.text;
        fn->ret = std::move(spec);
        fn->params = parse_params(&fn->is_varargs, /*allow_ref=*/true);
        fn->is_builtin = true;
        fn->is_public = true;
        fn->line = tk.line;
        fn->col = tk.col;
        semicolon();
        return fn;
    }
    if (ret_ref) throw err_at("a builtin constant cannot have a reference type", tk);
    auto gv = std::make_unique<GlobalVar>();
    gv->name = name_tok.text;
    gv->spec = std::move(spec);
    gv->is_const = true;
    gv->is_builtin = true;
    gv->line = tk.line;
    gv->col = tk.col;
    semicolon();
    return gv;
}

std::unique_ptr<Decl> Parser::parse_struct(bool is_union) {
    DeclTargetScope decl_scope(*this);
    MemberGroup members;
    auto* saved_members = cur_members_;
    cur_members_ = &members;
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
    const size_t g0 = groups_.size();
    while (true) {
        if (at_op("}")) {
            if (groups_.size() > g0) { directive_arm_end(); continue; }
            break;
        }
        if (at_directive()) { directive_header(); continue; }
        // The members of each arm collect into `cur_members_` (a `#if` arm has its own MemberGroup);
        // an unselected arm is simply dropped when the chain is popped, so no rollback is needed.
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
        if (at_kw("export")) {
            throw err("`export` applies to top-level declarations only; export the type itself");
        }
        if (at_op("~")) {
            if (explicit_mod) throw err("destructor cannot use `public`/`private`");
            if (is_union) throw err("union `" + name + "` cannot have a destructor");
            if (cur_members_->dtor) throw err("struct `" + name + "` can only have one destructor");
            cur_members_->dtor = parse_dtor();
            continue;
        }
        // `implicit T(...)`: the constructor may be used as an implicit conversion (struct only)
        bool implicit_ctor = false;
        if (peek().kind == TokKind::Ident && peek().text == "implicit" && peek(1).kind == TokKind::Ident &&
            peek(2).is_op("(")) {
            advance();
            implicit_ctor = true;
        }
        if (peek().kind == TokKind::Ident && peek().text == name && peek(1).is_op("(")) {
            if (is_union) throw err("union `" + name + "` cannot have a constructor");
            const Token ctor_name = advance();
            cur_members_->ctors.push_back(parse_ctor(ctor_name));
            cur_members_->ctors.back()->is_public = member_pub;
            cur_members_->ctors.back()->is_implicit = implicit_ctor;
            continue;
        }
        if (implicit_ctor) throw err("`implicit` can only modify a constructor");
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
        if (at_kw("virtual")) {
            throw err("`virtual` is only meaningful in a class: a struct has no inheritance, so its methods "
                      "are always static");
        }
        bool ret_ref = false;
        TypeSpecPtr spec = parse_return_type(ret_ref);
        if (at_kw("operator")) {
            if (is_union) throw err("union `" + name + "` cannot have methods");
            cur_members_->methods.push_back(parse_member_operator(std::move(spec), ret_ref));
            cur_members_->methods.back()->is_public = member_pub;
            continue;
        }
        const Token name_tok = expect_ident("field or method name");
        if (at_op("(")) {
            if (is_union) throw err("union `" + name + "` cannot have methods");
            cur_members_->methods.push_back(parse_method(std::move(spec), ret_ref, name_tok));
            cur_members_->methods.back()->is_public = member_pub;
            if (prefix_override) cur_members_->methods.back()->is_override = true;
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
            // `const T name;` is an instance field: it is initialized by every constructor, either
            // through the initializer list or through a default member initializer `const T x = expr;`
            // (SPEC 3.7).
            if (is_union) {
                throw err_at("union `" + name + "` does not support const fields (union members are always public fields)",
                             name_tok);
            }
            const auto add_const_field = [&](const std::string& fname) {
                cur_members_->fields.emplace_back(fname, clone_spec(*spec));
                cur_members_->field_visibility[fname] = member_pub;
                if (at_op("=")) {
                    advance();
                    cur_members_->field_default[fname] = parse_expr();
                }
            };
            add_const_field(name_tok.text);
            for (;;) {
                if (!at_op(",")) break;
                advance();
                const Token extra = expect_ident("field name");
                if (at_op(":")) {
                    throw err_at(
                        "bit-fields are not supported: use explicit shifts/masks or a `byte[N]` field with offset constants",
                        extra);
                }
                add_const_field(extra.text);
            }
            semicolon();
            continue;
        }
        cur_members_->fields.emplace_back(name_tok.text, clone_spec(*spec));
        cur_members_->field_visibility[name_tok.text] = member_pub;
        if (at_op("=")) {
            throw err_at("a default member initializer is only supported on a `const` field "
                         "(`const int x = 5;`); assign the field in the constructor body instead",
                         name_tok);
        }
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
            cur_members_->field_visibility[fname] = member_pub;
            cur_members_->fields.emplace_back(std::move(fname), clone_spec(*spec));
        }
        semicolon();
    }
    expect_op("}");
    if (groups_.size() != g0) throw err_at("unterminated `#if` block: missing `}`", tk);
    if (at_op(";")) advance();
    type_names_ = saved_type_names;
    decl->body = std::move(members);
    cur_members_ = saved_members;
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
            return spec_display(*s.elem) + "[" +
                   (s.len ? std::string("N") : std::to_string(s.size)) + "]";
        case TypeSpec::K::Func:
        case TypeSpec::K::Delegate: {
            std::string n = s.kind == TypeSpec::K::Delegate ? "delegate (" : "(";
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
    if (at_kw("const")) {
        advance();
        m->is_const = true;
    }
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
    c->params = parse_params(nullptr, /*allow_ref=*/true, /*allow_move=*/true);
    // `: x(v), y(w)` — member initializer list (decided 2026-11-17)
    if (at_op(":")) {
        advance();
        for (;;) {
            const Token fname = expect_ident("field name");
            expect_op("(");
            ExprPtr init = parse_expr();
            expect_op(")");
            c->init_list.emplace_back(fname.text, std::move(init));
            if (!at_op(",")) break;
            advance();
        }
    }
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
    DeclTargetScope decl_scope(*this);
    MemberGroup members;
    auto* saved_members = cur_members_;
    cur_members_ = &members;
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
    const size_t g0 = groups_.size();
    while (true) {
        if (at_op("}")) {
            if (groups_.size() > g0) { directive_arm_end(); continue; }
            break;
        }
        if (at_directive()) { directive_header(); continue; }
        // Members collect into `cur_members_` (a `#if` arm has its own MemberGroup); no rollback needed.
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
        if (at_kw("export")) {
            throw err("`export` applies to top-level declarations only; export the type itself");
        }
        if (at_op("~")) {
            if (explicit_mod) throw err("destructor cannot use `public`/`private`");
            if (cur_members_->dtor) throw err("class `" + cd->name + "` can only have one destructor");
            cur_members_->dtor = parse_dtor();
            continue;
        }
        bool prefix_override = false;
        bool prefix_virtual = false;
        while (at_kw("override") || at_kw("virtual")) {
            if (at_kw("override")) {
                if (prefix_override) throw err("`override` cannot be repeated");
                prefix_override = true;
            } else {
                if (prefix_virtual) throw err("`virtual` cannot be repeated");
                prefix_virtual = true;
            }
            advance();
        }
        if (prefix_override && prefix_virtual) {
            throw err("`virtual` and `override` cannot be combined: `override` already implies the base method "
                      "is virtual, and a fresh virtual method is written `virtual` alone");
        }
        const Token t = peek();
        if (t.kind == TokKind::Ident && t.text == cd->name && peek(1).is_op("(")) {
            if (prefix_override || prefix_virtual) {
                throw err_at("a constructor is never virtual: construction runs on the concrete class", t);
            }
            const Token ctor_name = advance();
            cur_members_->ctors.push_back(parse_ctor(ctor_name));
            cur_members_->ctors.back()->is_public = member_pub;
            continue;
        }
        bool prefix_weak = false;
        if (at_kw("weak")) {
            advance();
            prefix_weak = true;
        }
        if (peek().kind == TokKind::Ident && peek().text == "implicit" && peek(1).kind == TokKind::Ident &&
            peek(2).is_op("(")) {
            throw err("`implicit` constructors are only allowed in a struct: a class handle conversion would be "
                      "a hidden allocation");
        }
        bool ret_ref = false;
        TypeSpecPtr spec = parse_return_type(ret_ref);
        if (prefix_weak) spec->weak = true;
        if (at_kw("operator")) {
            if (prefix_weak) {
                throw err("`weak` can only be used on class fields, not on member operators");
            }
            auto op = parse_member_operator(std::move(spec), ret_ref);
            // A class's `==`/`!=` stays identity comparison (SPEC 3.11): overloading it would make
            // `a == b` mean two different things for the same type depending on the static type.
            if (op->name == "operator==" || op->name == "operator!=") {
                throw err("`==` on a class is always identity comparison and cannot be overloaded; write a free "
                          "function (`bool same(C a, C b)`) when value equality is wanted");
            }
            op->is_override = prefix_override;
            op->is_virtual = prefix_virtual;
            op->is_public = member_pub;
            cur_members_->methods.push_back(std::move(op));
            continue;
        }
        const Token name_tok = expect_ident("member name");
        if (at_op("(")) {
            if (prefix_weak) throw err_at("`weak` can only be used on fields, not on methods", name_tok);
            auto m = parse_method(std::move(spec), ret_ref, name_tok);
            if (prefix_override) m->is_override = true;
            if (prefix_virtual) m->is_virtual = true;
            m->is_public = member_pub;
            cur_members_->methods.push_back(std::move(m));
            continue;
        }
        if (ret_ref) {
            throw err_at("`ref` can only modify a method return type; a field cannot have a reference type", name_tok);
        }
        if (prefix_virtual) throw err_at("`virtual` can only be used on a method", name_tok);
        if (prefix_override) throw err_at("`override` can only be used on methods", name_tok);
        if (spec->is_const) {
            // `const T name;` is an instance field, initialized by every constructor through the
            // initializer list or a default member initializer `const T x = expr;` (SPEC 3.7).
            if (prefix_weak) throw err_at("`weak` cannot be combined with a `const` field", name_tok);
            const auto add_const_field = [&](const std::string& fname) {
                cur_members_->fields.emplace_back(fname, clone_spec(*spec));
                cur_members_->field_visibility[fname] = member_pub;
                if (at_op("=")) {
                    advance();
                    cur_members_->field_default[fname] = parse_expr();
                }
            };
            add_const_field(name_tok.text);
            for (;;) {
                if (!at_op(",")) break;
                advance();
                add_const_field(expect_ident("field name").text);
            }
            semicolon();
            continue;
        }
        cur_members_->fields.emplace_back(name_tok.text, clone_spec(*spec));
        cur_members_->field_visibility[name_tok.text] = member_pub;
        if (at_op("=")) {
            throw err_at("a default member initializer is only supported on a `const` field "
                         "(`const int x = 5;`); assign the field in the constructor body instead",
                         name_tok);
        }
        while (at_op(",")) {
            advance();
            const std::string fname = expect_ident("field name").text;
            cur_members_->field_visibility[fname] = member_pub;
            cur_members_->fields.emplace_back(fname, clone_spec(*spec));
        }
        semicolon();
    }
    expect_op("}");
    if (groups_.size() != g0) throw err_at("unterminated `#if` block: missing `}`", tk);
    if (at_op(";")) advance();
    type_names_ = saved_type_names;
    cd->body = std::move(members);
    cur_members_ = saved_members;
    return cd;
}

std::unique_ptr<Decl> Parser::parse_interface() {
    DeclTargetScope decl_scope(*this);
    MemberGroup members;
    auto* saved_members = cur_members_;
    cur_members_ = &members;
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
    const size_t g0 = groups_.size();
    while (true) {
        if (at_op("}")) {
            if (groups_.size() > g0) { directive_arm_end(); continue; }
            break;
        }
        if (at_directive()) { directive_header(); continue; }
        // Interface methods collect into `cur_members_`; a `#if` arm has its own MemberGroup.
        if (at_kw("public", "private")) {
            throw err("`public`/`private` cannot be used on interface methods (interface methods are always public)");
        }
        if (at_kw("export")) {
            throw err("`export` applies to top-level declarations only; export the interface itself");
        }
        if (at_kw("const")) {
            throw err("interface `" + id->name + "` does not support const members");
        }
        // `virtual` on an interface method is redundant (every interface method is abstract), accepted so
        // that the SPEC's interface examples read naturally.
        if (at_kw("virtual")) advance();
        bool ret_ref = false;
        TypeSpecPtr spec = parse_return_type(ret_ref);
        if (at_kw("operator")) {
            cur_members_->methods.push_back(parse_interface_operator(std::move(spec), ret_ref));
            continue;
        }
        const Token name_tok = expect_ident("method name");
        cur_members_->methods.push_back(parse_method(std::move(spec), ret_ref, name_tok));
    }
    expect_op("}");
    if (groups_.size() != g0) throw err_at("unterminated `#if` block: missing `}`", tk);
    if (at_op(";")) advance();
    type_names_ = saved_type_names;
    id->body = std::move(members);
    cur_members_ = saved_members;
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
    // An interface operator has no `this`, so the receiver is the first operand and may borrow it:
    // `Self operator<<(ref Self b, byte c)`. The borrow is only allowed on the receiver (checked by sema).
    m->params = parse_params(nullptr, /*allow_ref=*/true);
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
    if (at_kw("const")) {
        advance();
        m->is_const = true;
    }
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
    if (sym == "[]") {
        // Build 161: the subscript must be a member of the type it indexes. The free-function form had
        // to copy its receiver on every element access (a class cannot take a `ref` parameter), which
        // measured 50x slower in a loop — and the member form takes a borrowed `this`/handle instead.
        throw err_at("`operator[]` must be declared inside the type it indexes (a member of the `struct` "
                     "or `class`): the free-function form copies the receiver on every element access",
                     tk);
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
