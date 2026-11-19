// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing — units and declarations
//
// owns:      the unit header, `use`, `extern`/`builtin` declarations, qualified type names and the
//            free-function operator declarations
// not:       struct/class/interface/enum declaration bodies (parser_type_decl.cpp), statements,
//            expressions and type syntax (parser_expr.cpp), any semantic decision
// pass:      parsing; unit order is the order the loader hands over
// concept:   parser-units, parser-decls
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
// `keep_explicit_nonconst` lets a `const class` keep an explicitly non-const method: it exists as an
// override slot for a mutating virtual of the base (SPEC 3.7), so the declaration must survive here.
void mark_body_const(MemberGroup& m, bool keep_explicit_nonconst = false) {
    for (auto& f : m.fields) {
        if (f.second) f.second->is_const = true;
    }
    for (auto& mm : m.methods) {
        if (!keep_explicit_nonconst || !mm->const_written) mm->is_const = true;
    }
    for (auto& g : m.groups) {
        for (auto& arm : g.dir->arms) mark_body_const(arm.members, keep_explicit_nonconst);
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
        bool nomove = false;
        Token nomove_tok = tk;
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
            // `nomove` is the move-side counterpart of `nocopy`: the value must stay where it was
            // constructed (its address is part of its meaning). A `struct`, like `nocopy`.
            if (at_kw("nomove")) {
                if (nomove) throw err_at("duplicate modifier `nomove`", peek());
                nomove = true;
                nomove_tok = peek();
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
        if (nomove && !at_kw("struct")) {
            throw err_at("`nomove` can only modify a `struct` declaration", nomove_tok);
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
            // `use a;` and `use a, b.c, d;` (a trailing comma is accepted). The list is collected first
            // and published only after the semicolon, so a malformed item never imports the units before it.
            std::vector<std::unique_ptr<UseDecl>> batch;
            for (;;) {
                std::string uname = parse_unit_name();
                auto use = std::make_unique<UseDecl>();
                use->unit_name = std::move(uname);
                use->line = tk.line;
                use->col = tk.col;
                batch.push_back(std::move(use));
                if (!at_op(",")) break;
                advance();
                if (at_op(";")) break;
            }
            semicolon();
            for (auto& use : batch) {
                if (!groups_.empty() && groups_.back().decl_mode && cur_decls_) {
                    // The arm may not be selected: the semantic pass decides and then adds the `use`
                    // (P3.3). The dependency is still discovered by the loader's candidate scan.
                    module_names_.insert(use->unit_name);
                    push_decl(std::move(use));
                } else {
                    module_names_.insert(use->unit_name);
                    unit->uses.push_back(std::move(use));
                }
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

        // `type T += I;` was the planned interface-extension syntax; SPEC 3.18 replaced it with `extend`.
        if (peek().kind == TokKind::Ident && peek().text == "type" && peek(1).kind == TokKind::Ident &&
            peek(2).is_op("+=")) {
            throw err_at("`type` declares only a transparent alias; interface extension is written "
                         "`extend T with I;` / `extend I with J;` (SPEC 3.18, not implemented yet)",
                         peek(2));
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
            bool saw_directive = false;
            {
                const std::size_t save = pos_;
                if (peek().is_op("#")) saw_directive = true;
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
                    if (peek().is_op("#")) saw_directive = true;
                }
                pos_ = save;
            }
            if (!is_sum && saw_directive) {
                throw no_directive_here("between the alternatives of a variant");
            }
            if (is_sum) {
                throw err_at("`type` declares only a transparent alias; declare a tagged union with "
                             "`variant " + name_tok.text + " { Alt T; None; }` (SPEC 10.1.1)",
                             name_tok);
            }
            if (!type_params.empty()) {
                throw err_at("generic type aliases are not supported; a generic tagged union is "
                             "declared with `variant Option<T> { Some T; None; }`",
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

        if (at_kw("variant")) {
            if (is_const_decl) throw err_at("`const` can only modify a `struct` or `class` declaration", const_tok);
            if (has_layout) {
                throw err_at("`variant` does not support layout modifiers (`packed` / `align(N)`)", layout_tok);
            }
            auto d = parse_variant();
            d->is_public = is_pub;
            d->is_export = is_export;
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
                sd->nomove = nomove;
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
                if (auto* cd = dynamic_cast<ClassDecl*>(d.get())) {
                    cd->is_const_decl = true;
                    mark_body_const(cd->body, /*keep_explicit_nonconst=*/true);
                }
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

        if (at_kw("const") && !peek(1).is_kw("ref")) {
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
