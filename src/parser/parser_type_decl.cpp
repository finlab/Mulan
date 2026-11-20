// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing — type declarations
//
// owns:      struct/class/interface/union/enum declaration bodies, base lists, member, method,
//            constructor, destructor and operator signatures, the `const`-body marking and the
//            display names used for generic base classes
// not:       the unit header, `use`, `extern`/`builtin` (parser_unit.cpp); statements and expressions
//            (parser_expr.cpp); any semantic decision
// pass:      parsing; reached from the unit-level declaration loop
// concept:   parser-decls

#include "parser_impl.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace muc_detail;

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
        if (try_parse_promote(is_union)) continue;
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
        if (at_op("<")) {
            throw err_at("method-level generics are not supported yet; write a free generic function instead",
                         name_tok);
        }
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
        if (at_directive()) throw no_directive_here("between enum members");
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

// `variant Name { Alt T; Pair T, U; None; }` (SPEC 10.1.1). The internal node is still `SumDecl`:
// the surface name changed, the layout/value semantics did not.
std::unique_ptr<Decl> Parser::parse_variant() {
    const Token tk = expect_kw("variant");
    const Token name_tok = expect_ident("variant name");
    auto decl = std::make_unique<SumDecl>();
    decl->name = name_tok.text;
    decl->line = tk.line;
    decl->col = tk.col;
    if (at_op("<")) decl->type_params = parse_type_params();
    type_names_.insert(decl->name);  // the name is a type from here on
    expect_op("{");
    while (!at_op("}")) {
        if (at_eof()) throw err_at("unterminated variant declaration", tk);
        if (at_directive()) throw no_directive_here("between the alternatives of a variant");
        SumAlt alt;
        const Token at = expect_ident("alternative name");
        alt.name = at.text;
        alt.line = at.line;
        alt.col = at.col;
        if (!at_op(";")) {
            // Payload types are written like fields (`Some T;` / `Pair T, U;`), without parentheses.
            for (;;) {
                alt.fields.push_back(parse_type());
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
        }
        expect_op(";");
        decl->alts.push_back(std::move(alt));
    }
    expect_op("}");
    if (at_op(";")) advance();
    return decl;
}

bool Parser::try_parse_promote(bool is_union) {
    if (!(peek().kind == TokKind::Ident && peek().text == "promote" && peek(1).kind == TokKind::Ident &&
          (peek(2).is_op(";") || peek(2).is_op("(")))) {
        return false;
    }
    if (is_union) throw err_at("a union has no methods, so `promote` is not allowed", peek());
    const Token pt = advance();
    PromoteSpec ps;
    ps.line = pt.line;
    ps.col = pt.col;
    ps.field = expect_ident("promoted field name").text;
    if (at_op("(")) {
        advance();
        if (!at_op(")")) {
            for (;;) {
                ps.methods.push_back(expect_ident("method name").text);
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
        }
        expect_op(")");
    }
    semicolon();
    cur_members_->promotes.push_back(std::move(ps));
    return true;
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
    m->params = parse_params(nullptr, /*allow_ref=*/true);
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
    // `: Base(x), field(v), ...` — the construction prologue (SPEC 3.12)
    if (at_op(":")) {
        advance();
        for (;;) {
            const Token nm = expect_ident("initializer name");
            if (at_op("<")) {
                throw err_at("an initializer names the type without its arguments: write `" + nm.text +
                                 "(...)` — a generic base is spelled without `<...>` here (SPEC 3.12)",
                             peek());
            }
            CtorInit item;
            item.name = nm.text;
            item.line = nm.line;
            item.col = nm.col;
            expect_op("(");
            if (!at_op(")")) {
                for (;;) {
                    item.args.push_back(parse_expr());
                    if (!at_op(",")) break;
                    advance();
                }
            }
            expect_op(")");
            c->init_list.push_back(std::move(item));
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
    const bool is_union = false;  // a class is never a union (keeps the member loop's promote check uniform)
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
        if (try_parse_promote(is_union)) continue;
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
        if (at_kw("const") && !peek(1).is_kw("ref")) {
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
        if (at_op("<")) {
            throw err_at("method-level generics are not supported yet; write a free generic function instead",
                         name_tok);
        }
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
    // `const ref T`: the read-only borrow. `const ref` is the preferred spelling (it reads like C++'s
    // `const&`); the older `ref const T` spelling stays accepted (SPEC 3.8).
    bool const_before_ref = false;
    if (at_kw("const") && peek(1).is_kw("ref")) {
        advance();  // const
        const_before_ref = true;
    }
    if (at_kw("ref")) {
        advance();
        ret_ref = true;
    }
    TypeSpecPtr spec = parse_type();
    if (const_before_ref) {
        if (spec->kind != TypeSpec::K::Named) {
            throw err_at("`const` before `ref` needs a named borrowed type; write `const ref T` for this shape",
                         peek());
        }
        spec->is_const = true;
    }
    return spec;
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
    m->params = parse_params(nullptr, /*allow_ref=*/true);
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