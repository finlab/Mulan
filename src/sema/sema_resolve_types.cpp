// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Declaration and field resolution: the declarative half of the declaration pass
// (definitions of Sema members).
//
// owns:      `place_structs`, `resolve_unit_types` and the per-declaration resolvers (sum/struct/class
//            fields, sizeof operands, const members, synthesized constructors) plus `eval_const_int`
// not:       visibility and units (sema_decl.cpp), layout checks (sema_layout.cpp), any body checking
// pass:      the type/signature resolution step — everything that makes a declaration's shape final
// concept:   declaration-resolution, field-resolution

#include "sema_impl.hpp"

#include "directive.hpp"

namespace muc_detail {

    void Sema::place_structs() {
        for (auto& u : units_) {
            for (auto& d : u->decls) {
                if (auto s = dynamic_cast<StructDecl*>(d.get())) {
                    s->ty = Type::make_struct(u->name, s->name);
                } else if (auto e = dynamic_cast<EnumDecl*>(d.get())) {
                    // Unlike the Python version (see the report): the enum type is created here, so that a cross-unit
                    // reference to "an enum of a unit not yet resolved" never yields a null type.
                    e->ty = Type::make_enum(u->name, e->name);
                } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                    c->ty = Type::make_class(u->name, c->name);
                    c->ty->decl_const = c->is_const_decl;
                } else if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) {
                    i->ty = Type::make_interface(u->name, i->name);
                } else if (auto sm = dynamic_cast<SumDecl*>(d.get())) {
                    // The payload/layout types are filled in by `resolve_sum` (they need the alternatives'
                    // field types resolved).
                    sm->ty = Type::make_sum(u->name, sm->name, {}, {}, {}, nullptr);
                }
            }
        }
    }

    // Sum type (SPEC 10.1): resolve the alternatives' payload types and synthesize the layout type
    // `struct { i32 tag; union { payload... } }`. `sd->ty` must already exist (created in
    // `place_structs` for a declared sum, or by the instantiator for a generic instance), and it is
    // filled in place so every reference keeps pointing at the same object.
    void Sema::resolve_sum(SumDecl* sd, Unit* u) {
        if (sd->alts.size() < 2) {
            err("sum type `" + sd->name + "` needs at least two alternatives", sd);
        }
        std::vector<std::string> names;
        std::vector<std::vector<TypePtr>> fields;
        std::vector<TypePtr> payload;
        std::unordered_set<std::string> seen;
        for (auto& alt : sd->alts) {
            if (!seen.insert(alt.name).second) {
                err("duplicate alternative `" + alt.name + "` in sum type `" + sd->name + "`", sd);
            }
            std::vector<TypePtr> fts;
            for (auto& fs : alt.fields) {
                TypePtr ft = resolve_type(fs.get(), u);
                if (is_void(ft)) err("payload of alternative `" + alt.name + "` cannot be `void`", sd);
                // A payload stored by value cannot contain the sum itself: `type H = Holds(H) | None`
                // has no finite layout. `resolve_type` returns the same Type object, so identity is
                // enough (a pointer to the sum is fine and is the documented way to recurse).
                if (ft.get() == sd->ty.get()) {
                    err("payload of alternative `" + alt.name + "` would make sum `" + sd->name +
                            "` infinitely large (store a pointer or a class handle instead)",
                        sd);
                }
                if (noncopyable_struct(ft)) {
                    err("payload of alternative `" + alt.name + "` is `" + ft->str() +
                            "`, which needs a destructor and cannot be stored by value in a sum (use a "
                            "pointer or a class)",
                        sd);
                }
                fts.push_back(ft);
            }
            TypePtr pt;
            if (fts.size() == 1) {
                pt = fts[0];
            } else if (fts.size() > 1) {
                // several payload fields: an anonymous struct laid out like a tuple
                pt = Type::make_struct(u->name, "$alt$" + sd->name + "$" + alt.name);
                for (size_t i = 0; i < fts.size(); ++i) {
                    Field f;
                    f.name = "f" + std::to_string(i);
                    f.type = fts[i];
                    pt->fields.push_back(std::move(f));
                }
            }
            names.push_back(alt.name);
            payload.push_back(pt);
            fields.push_back(std::move(fts));
        }
        // payload area = a union of the alternatives (all at offset 0, like `union`)
        TypePtr uni = Type::make_struct(u->name, "$payload$" + sd->name);
        uni->is_union = true;
        for (size_t i = 0; i < payload.size(); ++i) {
            if (!payload[i]) continue;
            Field f;
            f.name = "a" + std::to_string(i);
            f.type = payload[i];
            uni->fields.push_back(std::move(f));
        }
        if (uni->fields.empty()) {  // all alternatives are unit values: keep the union non-empty
            Field f;
            f.name = "none";
            f.type = Type::make_prim("byte");
            uni->fields.push_back(std::move(f));
        }
        TypePtr layout = Type::make_struct(u->name, "$sum$" + sd->name);
        {
            Field tag;
            tag.name = "tag";
            tag.type = Type::make_prim("int");
            layout->fields.push_back(std::move(tag));
            Field pay;
            pay.name = "payload";
            pay.type = uni;
            layout->fields.push_back(std::move(pay));
        }
        sd->ty->sum_names = std::move(names);
        sd->ty->sum_fields = std::move(fields);
        sd->ty->sum_payload = std::move(payload);
        sd->ty->sum_layout = std::move(layout);
    }

    void Sema::resolve_struct_fields(StructDecl* sd, Unit* u) {
        if (!sd || sd->fields_resolved) return;
        if (!sd->ty) sd->ty = Type::make_struct(u->name, sd->name);
        sd->ty->is_packed = sd->is_packed;
        sd->ty->is_union = sd->is_union;
        sd->ty->nocopy = sd->nocopy;
        sd->ty->nomove = sd->nomove;
        const std::string kw = sd->is_union ? "union" : "struct";
        std::vector<Field> fields;
        std::unordered_set<std::string> fnames;
        if (sd->fields.size() > kMaxFields) {
            err(kw + " `" + sd->name + "` declares more than " + std::to_string(kMaxFields) +
                    " fields; split the data into nested structs",
                sd);
        }
        for (auto& [fname, spec] : sd->fields) {
            if (!fnames.insert(fname).second) {
                err("duplicate field `" + fname + "` in " + kw + " `" + sd->name + "`", sd);
            }
            TypePtr ft = resolve_type(spec.get(), u);
            if (is_void(ft)) err("field of " + kw + " `" + sd->name + "` cannot be void", sd);
            if (same_type(ft, sd->ty)) {
                err(kw + " `" + sd->name + "` cannot contain itself by value", sd);
            }
            if (spec->weak) err("`weak` can only be used on class fields", sd);
            if (sd->is_union && ft->needs_cleanup()) {
                err("field `" + fname + "` of union `" + sd->name + "` cannot be `" + ft->str() +
                        "`: union members cannot hold values that need reference counting",
                    sd);
            }
            bool field_pub = true;  // struct defaults to public
            if (auto vit = sd->field_visibility.find(fname); vit != sd->field_visibility.end()) {
                field_pub = vit->second;
            }
            fields.push_back(Field{fname, ft, false, field_pub});
        }
        sd->ty->fields = std::move(fields);
        if (sd->is_union && sd->ty->fields.empty()) {
            err("union `" + sd->name + "` must have at least one member", sd);
        }
        sd->fields_resolved = true;
    }

    void Sema::resolve_sizeof_types(const Expr* e, Unit* u) {
        if (!e) return;
        if (auto s = dynamic_cast<const SizeofExpr*>(e)) {
            if (s->spec && s->spec->kind == TypeSpec::K::Named && !s->spec->is_self &&
                s->spec->args.empty()) {
                const std::string& n = s->spec->name;
                if (!is_prim_name(n) && n.find('.') == std::string::npos) {
                    for (auto& d : u->decls) {
                        // A struct is stored by value, so its layout (and every field's) is needed now. A
                        // class is a reference: `sizeof` it is the handle size, independent of its fields,
                        // and its fields are deliberately resolved late (`class A { B b; }` may name a `B`
                        // declared further down). Resolving them here made that ordering fail — and, when
                        // the field named a class that was not even indexed yet, crashed (#75).
                        if (auto* sd = dynamic_cast<StructDecl*>(d.get()); sd && sd->name == n) {
                            resolve_struct_fields(sd, u);
                            break;
                        }
                        if (auto* cd = dynamic_cast<ClassDecl*>(d.get()); cd && cd->name == n) {
                            // The handle type is enough: its size does not depend on the fields.
                            if (!cd->ty) cd->ty = Type::make_class(u->name, cd->name);
                            break;
                        }
                    }
                }
            }
            if (s->expr) resolve_sizeof_types(s->expr.get(), u);
            return;
        }
        if (auto b = dynamic_cast<const Binary*>(e)) {
            resolve_sizeof_types(b->left.get(), u);
            resolve_sizeof_types(b->right.get(), u);
            return;
        }
        if (auto un = dynamic_cast<const Unary*>(e)) { resolve_sizeof_types(un->operand.get(), u); return; }
        if (auto nt = dynamic_cast<const NotExpr*>(e)) { resolve_sizeof_types(nt->operand.get(), u); return; }
        if (auto c = dynamic_cast<const Cast*>(e)) { resolve_sizeof_types(c->expr.get(), u); return; }
        if (auto c = dynamic_cast<const Call*>(e)) {
            resolve_sizeof_types(c->callee.get(), u);
            for (const auto& a : c->args) resolve_sizeof_types(a.get(), u);
            return;
        }
        if (auto m = dynamic_cast<const Member*>(e)) resolve_sizeof_types(m->obj.get(), u);
    }

    void Sema::resolve_class_fields(ClassDecl* c, Unit* u) {
        if (!c || c->fields_resolved) return;
        // A class named by a sizeof in an earlier declaration may still be a bare declaration here: the
        // placeholder is created on demand, exactly as check_class does.
        if (!c->ty) c->ty = Type::make_class(u->name, c->name);
        // Reject a too-deep chain *before* flattening the base fields: copying them level by level is
        // O(depth^2), so an absurd hierarchy would otherwise burn seconds before the diagnostic.
        {
            int depth = 0;
            std::unordered_set<const Type*> seen;
            for (TypePtr b = c->ty ? c->ty->base : nullptr; b; b = b->base) {
                if (!seen.insert(b.get()).second) break;  // a cycle: check_hierarchy_cycles reports it
                if (++depth > kMaxInheritanceDepth) {
                    err("class `" + c->name + "` is more than " + std::to_string(kMaxInheritanceDepth) +
                            " levels deep in its inheritance chain; flatten the hierarchy",
                        c);
                }
            }
        }
        std::vector<Field> fields;
        if (c->ty->base) fields = c->ty->base->fields;
        std::unordered_set<std::string> fnames;
        for (auto& f : fields) fnames.insert(f.name);
        // The base's fields are flattened into this class, so the bound applies to the sum: 256 levels of
        // 4096 fields each would otherwise add up to a million entries in one layout.
        if (fields.size() + c->fields.size() > kMaxFields) {
            err("class `" + c->name + "` has more than " + std::to_string(kMaxFields) +
                    " fields including the inherited ones; split the data into nested classes",
                c);
        }
        for (auto& [fname, spec] : c->fields) {
            if (!fnames.insert(fname).second) {
                err("duplicate field `" + fname + "` in class `" + c->name + "`", c);
            }
            TypePtr ft = resolve_type(spec.get(), u);
            if (is_void(ft)) err("class field cannot be void", c);
            if (spec->weak && !is_class_type(ft)) {
                err("`weak` field must be a class reference (interfaces do not support `weak` yet)", c);
            }
            // A class field of an uncopyable struct is allowed: a class is a reference type, so its "copy"
            // retains the handle and never copies the field (agents/value-semantics.md §8).
            bool field_pub = false;  // class defaults to private
            if (auto vit = c->field_visibility.find(fname); vit != c->field_visibility.end()) {
                field_pub = vit->second;
            }
            fields.push_back(Field{fname, ft, spec->weak, field_pub});
        }
        c->ty->fields = std::move(fields);
        c->fields_resolved = true;
    }

    bool Sema::eval_const_int(Expr* e, long long& out, int* bits, bool* sgn) {
        if (!e) return false;
        // One evaluator: the language fixes a single constant-expression evaluator (SPEC 3.7.2) and it
        // knows the integer width of every operand, so `const int MAX = 2147483647; MAX + 1` wraps at 32
        // bits here exactly as it does in `#if` (#76). The `#if` evaluator in directive.cpp is that one.
        static const UserConstTable kEmpty;
        const UserConstTable* table = directive_const_table();
        UserConst uc;
        std::string why;
        if (!eval_user_const(*e, table ? *table : kEmpty, uc, why)) return false;
        if (uc.is_float) return false;  // an array length / `step` / `align(N)` is an integer
        out = uc.value;
        if (bits) *bits = uc.bits;
        if (sgn) *sgn = uc.is_signed;
        return true;
    }


    void Sema::resolve_const_members(std::vector<std::unique_ptr<ConstMember>>& list,
                               const TypePtr& owner, Unit* u, const std::string& kw,
                               const std::string& type_name) {
        for (auto& cm : list) {
            cm->owner = owner;
            cm->rty = resolve_type(cm->spec.get(), u);
            if (is_void(cm->rty)) {
                err("const member `" + cm->name + "` in " + kw + " `" + type_name +
                        "` cannot be void",
                    cm.get());
            }
            if (!(is_prim(cm->rty) || is_enum_t(cm->rty))) {
                err("const member `" + cm->name +
                        "` must have a scalar type (bool, integer, float, double, or enum), found `" +
                        cm->rty->str() + "`",
                    cm.get());
            }
        }
    }

    // A `const T` view was copied from T while T may still have been an empty placeholder (its fields and
    // methods are filled in `resolve_unit_types`). Once every shape is final, replace each view's body
    // with the canonical type's, keeping the view's own cv flag and identity.

    // A type that has default member initializers but declares no constructor of its own gets a
    // synthesized zero-argument constructor: every construction site then runs the prologue that applies
    // them, instead of silently leaving the fields zeroed (SPEC 3.7).
    void Sema::synthesize_default_ctor(StructDecl* sd) {
        auto ctor = std::make_unique<CtorDecl>();
        ctor->line = sd->line;
        ctor->col = sd->col;
        ctor->is_public = true;
        ctor->is_synthesized = true;
        sd->ty->implicit_ctor = true;
        sd->ctors.push_back(std::move(ctor));
    }

    void Sema::synthesize_default_ctor(ClassDecl* c) {
        auto ctor = std::make_unique<CtorDecl>();
        ctor->line = c->line;
        ctor->col = c->col;
        ctor->is_public = true;
        ctor->is_synthesized = true;
        c->ty->implicit_ctor = true;
        c->ctors.push_back(std::move(ctor));
    }

    void Sema::resolve_unit_types(Unit* u) {        cur_ = u;

        // Structs: the placeholder type is ready, fields are filled in here (fields may reference other structs/enums)
        for (auto& d : u->decls) {
            auto sd = dynamic_cast<StructDecl*>(d.get());
            if (!sd) continue;
            resolve_struct_fields(sd, u);  // idempotent: collect may already have resolved them
            const std::string kw = sd->is_union ? "union" : "struct";
            resolve_const_members(sd->const_members, sd->ty, u, kw, sd->name);

            sd->ty->methods.clear();
            for (auto& m : sd->methods) {
                resolve_method_ret(m.get(), u);
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.is_const = m->is_const;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = false;
                sig.is_override = m->is_override;
                sig.is_public = m->is_public;
                sig.owner = sd->ty;
                sig.is_operator = m->is_operator;
                if (m->is_operator) check_member_operator_sig(m.get(), sd->ty);
                sd->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, sd->name, m->name, m->pty, m->is_const);
            }
            // A type with default member initializers but no constructor of its own gets a synthesized
            // zero-argument constructor, so every construction site applies those initializers instead of
            // silently leaving the fields zeroed (SPEC 3.7, build 215).
            if (sd->ctors.empty() && !sd->field_default.empty()) synthesize_default_ctor(sd);
            sd->ty->ctors.clear();
            for (auto& ctor : sd->ctors) {
                ctor->pty.clear();
                for (auto& p : ctor->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (p->by_move) {
                        // `T(move T other)`: the move constructor (agents/value-semantics.md §8.1)
                        if (ctor->params.size() != 1 || !same_type(pt, sd->ty)) {
                            err("the move constructor of struct `" + sd->name +
                                    "` must be written `" + sd->name + "(move " + sd->name +
                                    " other)` (one parameter of the struct's own type)",
                                p.get());
                        }
                        if (sd->ty->has_move_ctor) {
                            err("struct `" + sd->name + "` already declares a move constructor", p.get());
                        }
                        ctor->is_move = true;
                        sd->ty->has_move_ctor = true;
                    }
                    if (p->by_ref) {
                        // Only the copy constructor may borrow its argument (agents/value-semantics.md §8.1):
                        // `T(const ref T other)`. Any other `ref` parameter on a constructor would need the
                        // argument-passing machinery that constructors do not have.
                        if (ctor->params.size() != 1 || !same_type(pt, sd->ty)) {
                            err("a constructor may take a `ref` parameter only as the copy constructor "
                                "`" + sd->name + "(const ref " + sd->name + " other)`",
                                p.get());
                        }
                        if (!p->is_const) {
                            err("a copy constructor must not modify its source: write `" + sd->name +
                                    "(const ref " + sd->name + " other)`",
                                p.get());
                        }
                        if (sd->ty->has_copy_ctor) {
                            err("struct `" + sd->name + "` already declares a copy constructor", p.get());
                        }
                        if (sd->ty->nocopy) {
                            err("struct `" + sd->name + "` is `nocopy`: it cannot declare a copy constructor",
                                p.get());
                        }
                        ctor->is_copy = true;
                        sd->ty->has_copy_ctor = true;
                    }
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (!p->by_ref && !ctor->is_move && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                            p.get());
                    }
                    ctor->pty.push_back(pt);
                }
                if (ctor->is_implicit) {
                    if (ctor->pty.size() != 1) {
                        err("`implicit` constructor of struct `" + sd->name +
                                "` must take exactly one parameter (the source value)",
                            ctor.get());
                    }
                    if (!ctor->is_public) {
                        err("`implicit` constructor of struct `" + sd->name + "` must be public", ctor.get());
                    }
                    if (ctor->pty.size() == 1 && same_type(ctor->pty[0], sd->ty)) {
                        err("an `implicit` constructor cannot take the struct itself (it would be a copy)",
                            ctor.get());
                    }
                }
                ctor->cname = ctor->is_move ? move_ctor_cname(u->name, sd->name)
                                            : ctor->is_copy ? copy_ctor_cname(u->name, sd->name)
                                                            : ctor_cname(u->name, sd->name, ctor->pty);
                sd->ty->ctors.push_back(ctor->pty);
                sd->ty->ctor_param_names.push_back(ctor_param_names(ctor.get()));
                sd->ty->ctor_is_public.push_back(ctor->is_public);
                sd->ty->ctor_is_implicit.push_back(ctor->is_implicit);
            }
            sd->ty->has_dtor = sd->dtor != nullptr;
            if (sd->dtor) sd->dtor->cname = dtor_cname(u->name, sd->name);
        }

        // class: base class, interfaces, flattened fields, method/constructor signatures
        for (auto& d : u->decls) {
            auto c = dynamic_cast<ClassDecl*>(d.get());
            if (!c) continue;
            if (!c->ty) c->ty = Type::make_class(u->name, c->name);
            c->ty->decl_const = c->is_const_decl;

            bool has_base = false;
            TypeSpec where;
            where.line = c->line;
            where.col = c->col;
            for (size_t i = 0; i < c->bases.size(); ++i) {
                // Structured spec first: an instantiated generic interface (`class C : IIterator<int>`)
                // has no name to look up — it is created by the instantiator (U34 M5b).
                TypePtr bt;
                if (i < c->base_specs.size() && c->base_specs[i]) {
                    bt = resolve_type(c->base_specs[i].get(), u);
                } else {
                    bt = resolve_named_type(c->bases[i], u, &where);
                }
                if (bt->kind == TypeKind::Class) {
                    if (i != 0 || has_base) err("a class may have at most one base class, and it must come first", c);
                    if (bt->decl_const) {
                        err("a `const class` cannot be used as a base class (`" + bt->str() +
                                "` is a `const class`): `const` types are leaves, use composition "
                                "(`const class D { Base b; }`) or an interface instead",
                            c);
                    }
                    if (bt->is_const) err("a base class cannot be `const`", c);
                    has_base = true;
                    c->ty->base = bt;
                } else if (bt->kind == TypeKind::Interface) {
                    for (auto& prev : c->ty->interfaces) {
                        if (same_type(prev, bt)) {
                            err("class `" + c->name + "` implements interface `" + bt->str() + "` more than once", c);
                        }
                    }
                    c->ty->interfaces.push_back(bt);
                } else {
                    err("`" + c->bases[i] + "` is not a class or interface", c);
                }
            }

            // fields are handled by the topological pass below (base class fields must be ready first)

            resolve_const_members(c->const_members, c->ty, u, "class", c->name);

            c->ty->methods.clear();
            c->ty->is_abstract = false;
            for (auto& m : c->methods) {
                resolve_method_ret(m.get(), u);
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.is_const = m->is_const;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = !m->body;
                sig.is_override = m->is_override;
                // An abstract method has no implementation to name, so it is always virtual; an `override`
                // stays virtual (a further subclass may override it too) — `check_class` verifies the base
                // declaration allows that. Everything else needs `virtual` (build 160: static by default).
                sig.is_virtual = m->is_virtual || m->is_override || !m->body;
                sig.is_public = m->is_public;
                // A non-`const` method of a `const class` is an override slot for a mutating virtual of the
                // base: it is not part of the public surface (SPEC 3.7).
                if (c->is_const_decl && !m->is_const) sig.is_public = false;
                sig.owner = c->ty;
                sig.is_operator = m->is_operator;
                if (m->is_operator) check_member_operator_sig(m.get(), c->ty);
                c->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, c->name, m->name, m->pty, m->is_const);
                if (!m->body) c->ty->is_abstract = true;
            }
            // Same synthesized zero-argument constructor as for structs (SPEC 3.7, build 215).
            if (c->ctors.empty() && !c->field_default.empty()) synthesize_default_ctor(c);
            for (auto& ctor : c->ctors) {
                ctor->pty.clear();
                for (auto& p : ctor->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (p->by_move) {
                        err("`class` is a reference type: it has no move or copy constructor, so `move` "
                            "parameters are only meaningful on a struct",
                            p.get());
                    }
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                            p.get());
                    }
                    ctor->pty.push_back(pt);
                }
                ctor->cname = ctor_cname(u->name, c->name, ctor->pty);
                c->ty->ctors.push_back(ctor->pty);
                c->ty->ctor_param_names.push_back(ctor_param_names(ctor.get()));
                c->ty->ctor_is_public.push_back(ctor->is_public);
            }
            if (c->dtor) c->dtor->cname = dtor_cname(u->name, c->name);
        }

        // class fields: resolved in base-class dependency order (a base class may be declared later in the same unit)
        {
            std::unordered_map<std::string, ClassDecl*> by_name;
            for (auto& d : u->decls) {
                if (auto c = dynamic_cast<ClassDecl*>(d.get())) by_name[c->name] = c;
            }
            std::unordered_map<ClassDecl*, int> state;
            std::function<void(ClassDecl*)> resolve_fields = [&](ClassDecl* c) {
                if (!c || state[c] != 0) return;
                state[c] = 1;
                for (auto& bn : c->bases) {
                    if (bn.find('.') != std::string::npos) continue;
                    auto it = by_name.find(bn);
                    if (it != by_name.end() && it->second != c) resolve_fields(it->second);
                }
                resolve_class_fields(c, u);
                state[c] = 2;
            };
            for (auto& d : u->decls) {
                if (auto c = dynamic_cast<ClassDecl*>(d.get())) resolve_fields(c);
            }
        }

        // sum types: payload + layout (after the struct/class fields they may mention)
        for (auto& d : u->decls) {
            if (auto sm = dynamic_cast<SumDecl*>(d.get())) resolve_sum(sm, u);
        }

        // interface: base interfaces and method signatures
        for (auto& d : u->decls) {
            auto itf = dynamic_cast<InterfaceDecl*>(d.get());
            if (!itf) continue;
            if (!itf->ty) itf->ty = Type::make_interface(u->name, itf->name);
            TypeSpec where;
            where.line = itf->line;
            where.col = itf->col;
            for (size_t i = 0; i < itf->bases.size(); ++i) {
                // Structured spec when present: `interface IInt : Base<int>` names an instantiated
                // generic interface, which has no lookup name.
                TypePtr bt = (i < itf->base_specs.size() && itf->base_specs[i])
                                 ? resolve_type(itf->base_specs[i].get(), u)
                                 : resolve_named_type(itf->bases[i], u, &where);
                if (bt->kind != TypeKind::Interface) {
                    err("an interface can only inherit from an interface", itf);
                }
                for (auto& prev : itf->ty->interfaces) {
                    if (same_type(prev, bt)) {
                        err("interface `" + itf->name + "` inherits `" + bt->str() + "` more than once", itf);
                    }
                }
                itf->ty->interfaces.push_back(bt);
            }
            itf->ty->methods.clear();
            TypePtr saved_class = cur_class_;
            cur_class_ = itf->ty;  // make `Self` in signatures resolve to the interface type
            for (auto& m : itf->methods) {
                resolve_method_ret(m.get(), u);
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.is_const = m->is_const;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = !m->body;
                sig.is_override = false;
                sig.owner = itf->ty;
                sig.is_operator = m->is_operator;
                sig.ret_self = m->ret && m->ret->is_self;
                for (auto& p : m->params) sig.params_self.push_back(p->spec && p->spec->is_self);
                if (m->is_operator) init_interface_operator(m.get(), sig, itf->name);
                itf->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, itf->name, m->name, m->pty);
            }
            cur_class_ = saved_class;
        }

        // enums: check for duplicate members (the type is already created in place_structs)
        for (auto& d : u->decls) {
            auto ed = dynamic_cast<EnumDecl*>(d.get());
            if (!ed) continue;
            std::unordered_set<std::string> names;
            for (auto& [m, v] : ed->members) {
                if (!names.insert(m).second) {
                    err("duplicate member `" + m + "` in enum `" + ed->name + "`", ed);
                }
            }
        }

        for (auto& d : u->decls) {
            auto g = dynamic_cast<GlobalVar*>(d.get());
            if (!g) continue;
            g->rty = resolve_type(g->spec.get(), u);
            if (is_void(g->rty)) err("variable `" + g->name + "` cannot be void", g);
        }

        for (auto& d : u->decls) {
            auto f = dynamic_cast<FuncDecl*>(d.get());
            if (!f) continue;
            f->rty = make_borrow_ret(f->ret.get(), f->ret_ref, f, u, "function `" + f->name + "`");
            if (noncopyable_struct(referent_of(f->rty)) && !referent_of(f->rty)->movable()) {
                err("struct `" + referent_of(f->rty)->str() +
                        "` cannot be returned by value: " + noncopyable_reason(referent_of(f->rty)) +
                        "; declare `" + referent_of(f->rty)->str() + "(const ref " + referent_of(f->rty)->str() + " other)` or `" + referent_of(f->rty)->str() + "(move " + referent_of(f->rty)->str() + " other)`" + (borrowable_type(referent_of(f->rty)) ? ", or return a `ref " + referent_of(f->rty)->str() + "` borrow" : ""),
                    f);
            }
            // `extern` imports a C symbol and `export` publishes one for DLL consumers, so both sides
            // of the boundary must obey the C ABI; a purely internal function keeps the faster one.
            const bool c_abi = f->is_extern || f->is_export;
            const std::string ckw = f->is_extern ? "`extern`" : "`export`";
            f->c_abi = c_abi;
            f->pty.clear();
            f->pty_ref.clear();
            f->pty_cref.clear();
            for (auto& p : f->params) {
                TypePtr pt = resolve_type(p->spec.get(), u);
                if (!p->by_ref) pt = decay_param_type(pt);
                if (is_void(pt)) err("parameter `" + p->name + "` cannot be void", p.get());
                if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                    err("struct `" + pt->str() +
                            "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                        p.get());
                }
                // A C caller never participates in Mulan's ownership protocol: it cannot move a value
                // into the callee or hand over a resource, so a by-value struct that needs cleanup
                // (a destructor, or a field/array that owns something) is rejected at the boundary
                // even when it is movable inside the language (#30).
                if (c_abi && !p->by_ref && pt->kind == TypeKind::Struct && pt->needs_cleanup() &&
                    !contains_reference_type(pt)) {
                    err(ckw + " function `" + f->name + "` cannot take struct `" + pt->str() +
                            "` by value: it needs cleanup (" + noncopyable_reason(pt) +
                            "), and a C caller does not own it the way a Mulan callee expects; pass a "
                            "pointer or use a plain C struct",
                        p.get());
                }
                if (p->by_ref) {
                    if (c_abi) {
                        err(ckw + " functions cannot take `ref` parameters (`&` borrows are a language mechanism; write a pointer)", p.get());
                    }
                    if (!borrowable_type(pt)) {
                        if (pt->kind == TypeKind::Struct && pt->is_union) {
                            err("`ref` parameters cannot borrow a union value", p.get());
                        }
                        if (pt->kind == TypeKind::Struct) {
                            err("struct `" + pt->str() +
                                    "` cannot be borrowed by `ref` (it contains reference-counted fields or a destructor)",
                                p.get());
                        }
                        err("`ref` parameters can only borrow a struct or scalar value (found `" + pt->str() + "`)", p.get());
                    }
                }
                f->pty.push_back(pt);
                f->pty_ref.push_back(p->by_ref);
                f->pty_cref.push_back(p->by_ref && p->is_const);
            }
            if (c_abi) {
                // C has no array results: a C header returns a pointer (or fills a struct).
                if (f->rty && f->rty->kind == TypeKind::Array) {
                    err(ckw + " function `" + f->name + "` cannot return a fixed-size array `" +
                            f->rty->str() + "`: C has no array result (return a pointer or wrap it in a struct)",
                        f);
                }
                // A C caller never participates in Mulan's ownership protocol: it cannot move a value
                // into the callee or hand over a resource, so a by-value struct that needs cleanup
                // (a destructor, or a field/array that owns something) is rejected at the boundary
                // even when it is movable inside the language (#30).
                if (f->rty && f->rty->kind == TypeKind::Struct && f->rty->needs_cleanup() &&
                    !contains_reference_type(f->rty)) {
                    err(ckw + " function `" + f->name + "` cannot return struct `" + f->rty->str() +
                            "` by value: it needs cleanup (" + noncopyable_reason(f->rty) +
                            "), and a C caller does not participate in Mulan's ownership protocol; "
                            "return a pointer or use a plain C struct",
                        f);
                }
                // A delegate owns an environment: C has no such ABI (and no way to release it).
                if (contains_delegate_type(f->rty)) {
                    err(ckw + " function `" + f->name +
                            "` cannot return a `delegate` (the environment is reference counted; pass "
                            "`.code`/`.env` explicitly as `void*`)",
                        f);
                }
                for (auto& pt : f->pty) {
                    if (contains_delegate_type(pt)) {
                        err(ckw + " function `" + f->name +
                                "` cannot take a `delegate` parameter (pass `.code`/`.env` explicitly as "
                                "`void*`)",
                            f);
                    }
                }
                if (contains_reference_type(f->rty)) {
                    err(ckw + " function `" + f->name + "` cannot return a class or interface (C code does not participate in reference counting)",
                        f);
                }
                for (auto& pt : f->pty) {
                    if (contains_reference_type(pt)) {
                        err("parameter of " + ckw + " function `" + f->name + "` cannot be a class or interface", f);
                    }
                }
                if (contains_slice_type(f->rty) || [&] {
                        for (auto& pt : f->pty)
                            if (contains_slice_type(pt)) return true;
                        return false;
                    }()) {
                    err(ckw + " function `" + f->name +
                            "` cannot use dynamic arrays `T[]` (they carry an RC header and are not C layout)",
                        f);
                }
                // Sum types are a built-in whose layout is an implementation detail: build a plain
                // `struct { tag; union }` on the C side and convert explicitly (SPEC 10.1.1).
                if (contains_sum_type(f->rty) || [&] {
                        for (auto& pt : f->pty)
                            if (contains_sum_type(pt)) return true;
                        return false;
                    }()) {
                    err(ckw + " function `" + f->name +
                            "` cannot use a sum type: its tag+payload layout is an implementation detail, "
                            "not a public interface (pass a plain `struct { tag; union }` and convert)",
                        f);
                }
                if (f->is_extern) f->cname = f->name;  // the C symbol itself
            }
            if (!f->is_extern) {
                if (f->name == "main" && u->name == entry_) {
                    f->cname = "mu_main";
                    f->is_entry = true;
                } else {
                    f->cname = func_cname(u->name, f);
                }
            }
        }
    }

}  // namespace muc_detail
