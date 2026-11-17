// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds class / interface checking (part 1) (definitions of Sema members).
#include "sema_impl.hpp"

#include "ast_clone.hpp"

namespace muc_detail {

    void Sema::begin_member(const TypePtr& cls, const TypePtr& ret, const std::string& name, int line,
                      int col) {
        scopes_.clear();
        auto_groups_.clear();
        scopes_.emplace_back();
        loop_depth_ = 0;
        begin_move_scope();
        cur_class_ = cls;
        cur_ret_ = ret;
        cur_name_ = name;
        in_ctor_ = false;
        in_dtor_ = false;
        in_finally_ = false;
        auto self = std::make_unique<LocalVar>();
        self->name = "this";
        self->rty = cls;
        self->line = line;
        self->col = col;
        declare_local(self.get());
        this_local_.clear();
        this_local_.push_back(std::move(self));
    }

    void Sema::end_member() {
        scopes_.clear();
        auto_groups_.clear();
        cur_class_ = nullptr;
        cur_ret_ = nullptr;
        cur_name_.clear();
        in_ctor_ = false;
        this_local_.clear();
    }

    void Sema::declare_params(std::vector<std::unique_ptr<LocalVar>>& out,
                        std::vector<std::unique_ptr<Param>>& params,
                        const std::vector<TypePtr>& pty) {
        out.clear();
        for (size_t i = 0; i < params.size() && i < pty.size(); ++i) {
            auto lv = std::make_unique<LocalVar>();
            lv->name = params[i]->name;
            lv->rty = pty[i];
            lv->is_const = params[i]->is_const;
            // A `move` parameter is a borrow the constructor may steal from: the caller zeroes the source
            // afterwards, so the parameter must never destroy it (agents/value-semantics.md §8.3).
            lv->by_ref = params[i]->by_ref || params[i]->by_move;
            lv->line = params[i]->line;
            lv->col = params[i]->col;
            declare_local(lv.get());
            out.push_back(std::move(lv));
        }
    }

    bool Sema::same_sig(const MethodSig& a, const MethodSig& b) {
        if (a.name != b.name) return false;
        // `const` is part of a method's signature (decided 2026-11-17): `m()` and `m() const` are
        // different overloads, and an override must match its base method's constness.
        if (a.is_const != b.is_const) return false;
        // Only an **operator**'s first `Self` operand is the receiver (SPEC 3.11: "the receiver is the
        // first operand and must be spelled `Self`"); a member operator takes it as the implicit
        // `this`, so the two parameter lists differ by exactly that entry. A plain method's `Self` is
        // an ordinary parameter type (SPEC 3.15.6, `bool less(Self other)`), so it must never be
        // dropped — the implementor spells it out too.
        auto receiver_at = [](const MethodSig& s) -> int {
            if (!s.is_operator) return -1;
            if (s.params_self.size() > s.params.size()) return -2;
            for (size_t i = 0; i < s.params_self.size(); ++i) {
                if (s.params_self[i]) return i == 0 ? 0 : -2;
            }
            return -1;
        };
        const int ra = receiver_at(a);
        const int rb = receiver_at(b);
        if (ra == -2 || rb == -2) return false;  // the receiver must be the first operand
        const bool drop_a = ra == 0 && rb < 0 && b.params.size() + 1 == a.params.size();
        const bool drop_b = rb == 0 && ra < 0 && a.params.size() + 1 == b.params.size();
        std::vector<const TypePtr*> av, bv;
        for (size_t i = 0; i < a.params.size(); ++i) {
            if (drop_a && i == 0) continue;
            av.push_back(&a.params[i]);
        }
        for (size_t i = 0; i < b.params.size(); ++i) {
            if (drop_b && i == 0) continue;
            bv.push_back(&b.params[i]);
        }
        if (av.size() != bv.size()) return false;
        for (size_t i = 0; i < av.size(); ++i) {
            if (!same_type(*av[i], *bv[i])) return false;
        }
        return true;
    }

    // The interface and base-class graphs are DAGs, so a plain recursion visits shared ancestors once per
    // path — exponentially many times in a diamond, which a generated program can exploit (`interface
    // A{i} : A{i-1}, B{i-1}` doubles the paths per level). Every walk below takes a visited set.
    static bool first_visit(std::unordered_set<const Type*>* seen, const TypePtr& t) {
        if (!seen) return true;
        return seen->insert(t.get()).second;
    }

    const MethodSig* Sema::find_sig(const TypePtr& t, const MethodSig& sig, int depth,
                                    std::unordered_set<const Type*>* seen) {
        if (!t || depth > 256) return nullptr;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return nullptr;
        for (auto& m : t->methods) {
            if (same_sig(m, sig)) return &m;
        }
        if (t->kind == TypeKind::Class && t->base) {
            if (auto r = find_sig(t->base, sig, depth + 1, &visited)) return r;
        }
        for (auto& i : t->interfaces) {
            if (auto r = find_sig(i, sig, depth + 1, &visited)) return r;
        }
        return nullptr;
    }

    void Sema::collect_sigs(const TypePtr& t, const MethodSig& sig,
                             std::vector<const MethodSig*>& out, int depth,
                             std::unordered_set<const Type*>* seen) {
        if (!t || depth > 256) return;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return;
        for (auto& m : t->methods) {
            if (same_sig(m, sig)) out.push_back(&m);
        }
        if (t->kind == TypeKind::Class && t->base) collect_sigs(t->base, sig, out, depth + 1, &visited);
        for (auto& i : t->interfaces) collect_sigs(i, sig, out, depth + 1, &visited);
    }

    const MethodSig* Sema::find_method(const TypePtr& t, const std::string& name, size_t argc,
                                        int depth, std::unordered_set<const Type*>* seen) {
        if (!t || depth > 256) return nullptr;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return nullptr;
        for (auto& m : t->methods) {
            if (m.name == name && m.params.size() == argc) return &m;
        }
        if (t->kind == TypeKind::Class && t->base) {
            if (auto r = find_method(t->base, name, argc, depth + 1, &visited)) return r;
        }
        for (auto& i : t->interfaces) {
            if (auto r = find_method(i, name, argc, depth + 1, &visited)) return r;
        }
        return nullptr;
    }

    bool Sema::has_method_named(const TypePtr& t, const std::string& name, int depth,
                                std::unordered_set<const Type*>* seen) {
        if (!t || depth > 256) return false;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return false;
        for (auto& m : t->methods) {
            if (m.name == name) return true;
        }
        if (t->kind == TypeKind::Class && t->base &&
            has_method_named(t->base, name, depth + 1, &visited)) {
            return true;
        }
        for (auto& i : t->interfaces) {
            if (has_method_named(i, name, depth + 1, &visited)) return true;
        }
        return false;
    }

    void Sema::check_method(ClassDecl* c, MethodDecl* m) {
        if (!m->body) return;  // abstract method
        begin_member(c->ty, m->rty, c->name + "." + m->name, m->line, m->col);
        const bool saved_const = cur_method_const_;
        cur_method_const_ = m->is_const;
        declare_params(m->param_locals, m->params, m->pty);
        check_block(m->body.get());
        if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
            err("method `" + m->name + "` may reach the end of its body without returning a value", m);
        }
        cur_method_const_ = saved_const;
        end_member();
    }

    void Sema::check_ctor(ClassDecl* c, CtorDecl* ctor) {
        begin_member(c->ty, t_void(), c->name + " constructor", ctor->line, ctor->col);
        in_ctor_ = true;
        declare_params(ctor->param_locals, ctor->params, ctor->pty);
        apply_field_defaults(c->field_default, c->ty->fields, ctor);
        check_ctor_init_list(c->ty, ctor);

        bool needs_super = false;
        if (c->ty->base) {
            if (c->ty->base->ctors.empty()) {
                needs_super = false;
            } else {
                needs_super = true;
                for (auto& cs : c->ty->base->ctors) {
                    if (cs.empty()) { needs_super = false; break; }
                }
            }
        }

        Block* body = ctor->body.get();
        // A statement-list `#if` in the body must be expanded before the leading `super(...)` is looked
        // for: the selected arm decides whether the first statement is the super call (P3.3c).
        expand_directive_stmts(body);
        bool first_super = false;
        if (body && !body->stmts.empty()) {
            auto es = dynamic_cast<ExprStmt*>(body->stmts[0].get());
            auto call = es ? dynamic_cast<Call*>(es->expr.get()) : nullptr;
            auto nm = call ? dynamic_cast<Name*>(call->callee.get()) : nullptr;
            first_super = nm && nm->ident == "super";
        }
        if (needs_super && !first_super) {
            err("constructor must call `super(...)` as its first statement", ctor);
        }
        ctor->has_explicit_super = first_super;
        if (first_super && !ctor->init_list.empty()) {
            err("an initializer list cannot be combined with an explicit `super(...)` yet: the base "
                "constructor already runs the member step",
                ctor);
        }

        push();
        size_t start = 0;
        if (body) {
            if (first_super) {
                super_ok_ = true;
                check_stmt(body->stmts[0].get());
                super_ok_ = false;
                start = 1;
            }
            for (size_t i = start; i < body->stmts.size(); ++i) {
                check_stmt(body->stmts[i].get());
            }
        }
        pop();
        super_ok_ = false;
        end_member();
    }

    void Sema::check_dtor(ClassDecl* c) {
        DtorDecl* d = c->dtor.get();
        if (!d) return;
        begin_member(c->ty, t_void(), c->name + " destructor", d->line, d->col);
        in_dtor_ = true;
        check_block(d->body.get());
        in_dtor_ = false;
        end_member();
    }

    void Sema::check_struct_method(StructDecl* sd, MethodDecl* m) {
        if (!m->body) {
            err("struct method `" + m->name + "` must have a body", m);
        }
        begin_member(sd->ty, m->rty, sd->name + "." + m->name, m->line, m->col);
        const bool saved_const = cur_method_const_;
        cur_method_const_ = m->is_const;
        declare_params(m->param_locals, m->params, m->pty);
        check_block(m->body.get());
        if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
            err("method `" + m->name + "` may reach the end of its body without returning a value", m);
        }
        cur_method_const_ = saved_const;
        end_member();
    }

    void Sema::check_struct_ctor(StructDecl* sd, CtorDecl* ctor) {
        begin_member(sd->ty, t_void(), sd->name + " constructor", ctor->line, ctor->col);
        in_ctor_ = true;
        declare_params(ctor->param_locals, ctor->params, ctor->pty);
        apply_field_defaults(sd->field_default, sd->ty->fields, ctor);
        check_ctor_init_list(sd->ty, ctor);
        check_block(ctor->body.get());
        end_member();
    }

    // A constructor that does not name a field with a default member initializer gets that expression as
    // an initializer entry: one code path for `: x(v)` and `const int x = 5;` (SPEC 3.7).
    void Sema::apply_field_defaults(const std::unordered_map<std::string, ExprPtr>& defaults,
                                    const std::vector<Field>& fields, CtorDecl* ctor) {
        if (defaults.empty()) return;
        for (const auto& f : fields) {
            auto it = defaults.find(f.name);
            if (it == defaults.end() || !it->second) continue;
            bool listed = false;
            for (const auto& item : ctor->init_list) {
                if (item.first == f.name) {
                    listed = true;
                    break;
                }
            }
            if (listed) continue;
            ctor->init_list.emplace_back(f.name, clone_expr(*it->second));
        }
    }

    // `C(...) : x(v), y(w) { }`: validate the member initializer list (decided 2026-11-17). The entries
    // are lowered by codegen before the body; a base-class field cannot be named here (the base
    // constructor owns it), and every `const` field must be initialized here.
    void Sema::check_ctor_init_list(const TypePtr& ty, CtorDecl* ctor) {
        size_t baseCount = 0;
        if (ty->base && ty->base->kind == TypeKind::Class) baseCount = ty->base->fields.size();
        std::unordered_set<std::string> inited;
        for (auto& item : ctor->init_list) {
            const int idx = ty->field_index(item.first);
            if (idx < 0) {
                // `: Base(x)` / `: Cfg(7)` look like field entries but name the base class / the class
                // itself. Both are constructor forms the language does not have yet; say so instead of
                // "unknown field".
                if (ty->base && ty->base->name == item.first) {
                    err("a base-class initializer (`: " + item.first +
                            "(...)`) is not supported yet: call `super(...)` at the start of the "
                            "constructor body (it cannot be combined with an initializer list yet)",
                        item.second.get());
                }
                if (ty->name == item.first) {
                    err("delegating constructors (`: " + item.first +
                            "(...)`) are not supported yet: initialize the fields directly",
                        item.second.get());
                }
                err("constructor initializer names an unknown field `" + item.first + "`", item.second.get());
            }
            if (static_cast<size_t>(idx) < baseCount) {
                err("cannot initialize the base class field `" + item.first +
                        "` here: the base constructor initializes it",
                    item.second.get());
            }
            if (!inited.insert(item.first).second) {
                err("field `" + item.first + "` is initialized twice", item.second.get());
            }
            check_assign(item.second.get(), ty->fields[static_cast<size_t>(idx)].type,
                         "constructor initializer for `" + item.first + "`");
        }
        // A const field has no other way to be initialized (it cannot be written in the body).
        // A `builtin` constant (the `comptime` unit, 9.3) is compiler-provided and is exempt.
        for (size_t i = baseCount; i < ty->fields.size(); ++i) {
            const Field& f = ty->fields[i];
            if (!f.type || !f.type->is_const) continue;
            if (!inited.count(f.name)) {
                err("const field `" + f.name +
                        "` must be initialized in the constructor initializer list",
                    ctor);
            }
        }
    }

    void Sema::check_struct_dtor(StructDecl* sd) {
        DtorDecl* d = sd->dtor.get();
        if (!d) return;
        begin_member(sd->ty, t_void(), sd->name + " destructor", d->line, d->col);
        in_dtor_ = true;
        check_block(d->body.get());
        in_dtor_ = false;
        end_member();
    }

    void Sema::check_const_members(std::vector<std::unique_ptr<ConstMember>>& list, const TypePtr& ty,
                             const std::string& kw, const std::string& type_name) {
        std::unordered_set<std::string> seen;
        for (auto& cm : list) {
            if (!seen.insert(cm->name).second) {
                err("duplicate const member `" + cm->name + "` in " + kw + " `" + type_name + "`",
                    cm.get());
            }
            if (ty->field_type(cm->name)) {
                err("const member `" + cm->name + "` in " + kw + " `" + type_name +
                        "` has the same name as a field",
                    cm.get());
            }
            if (has_method_named(ty, cm->name)) {
                err("const member `" + cm->name + "` in " + kw + " `" + type_name +
                        "` has the same name as a method",
                    cm.get());
            }
            if (cm->name == type_name) {
                err("const member `" + cm->name + "` in " + kw + " `" + type_name +
                        "` has the same name as a constructor",
                    cm.get());
            }
            ConstVal v;
            if (!eval_const_member(cm.get(), v)) {
                err("initializer of const member `" + cm->name +
                        "` must be a compile-time constant",
                    cm.get());
            }
        }
    }

    // `d` is `b` itself or inherits from it (used so that a refining default wins, as it does for classes).
    // The depth bound covers the enforced limit (`kMaxInterfaceDepth` = 256), so a program that compiles
    // never loses an inherited member to a truncated walk.
    static bool iface_extends(const TypePtr& d, const TypePtr& b, int depth = 0,
                              std::unordered_set<const Type*>* seen = nullptr) {
        if (!d || !b || depth > kMaxInterfaceDepth) return false;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!visited.insert(d.get()).second) return false;
        if (same_type(d, b)) return true;
        for (const auto& p : d->interfaces) {
            if (iface_extends(p, b, depth + 1, &visited)) return true;
        }
        return false;
    }

    // U34 M4: the default implementation of `sig` on `itf`, walking the interface inheritance chain
    // (most derived first, so a refining default wins). `id->methods[i]` and `itf->methods[i]` are
    // parallel: sema_decl.cpp builds both in one pass, in declaration order. Generic interface
    // instances register their cloned decl under the display name, so `ISeq<int>` works too.
    MethodDecl* Sema::find_iface_default_decl(const TypePtr& itf, const MethodSig& sig, TypePtr* owner,
                                              int depth, std::unordered_set<const Type*>* seen) {
        if (!itf || depth > kMaxInterfaceDepth) return nullptr;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, itf)) return nullptr;
        auto info = infos_.find(itf->unit);
        if (info != infos_.end()) {
            auto found = info->second.interfaces.find(itf->name);
            if (found != info->second.interfaces.end()) {
                InterfaceDecl* id = found->second;
                for (size_t i = 0; i < id->methods.size() && i < itf->methods.size(); ++i) {
                    if (!same_sig(itf->methods[i], sig)) continue;
                    if (!itf->methods[i].is_abstract && id->methods[i]->body) {
                        if (owner) *owner = itf;
                        return id->methods[i].get();
                    }
                    break;  // declared here without a body: nothing to inherit from this interface
                }
            }
        }
        for (const auto& b : itf->interfaces) {
            if (MethodDecl* d = find_iface_default_decl(b, sig, owner, depth + 1, &visited)) return d;
        }
        return nullptr;
    }

    // Monomorphization, not inheritance: S gains no layout, no vtable slot and no `super`; the result is
    // an ordinary method that the normal passes check and emit.
    void Sema::materialize_iface_default(StructDecl* sd, const TypePtr& itf, const MethodSig& sig) {
        MethodDecl* def = find_iface_default_decl(itf, sig);
        if (!def) return;
        auto gen = clone_method(*def);
        gen->is_override = true;
        gen->is_public = true;
        gen->from_iface_default = true;
        gen->iface_unit = itf->unit;
        // The signature is the interface's, already resolved in the interface's unit: a default written
        // in another unit may name types that S's unit does not see, so it must not be re-resolved here.
        gen->rty = sig.ret;
        gen->ret_ref = is_borrow_type(sig.ret);
        gen->pty = sig.params;
        gen->cname = method_cname(sd->ty->unit, sd->name, gen->name, gen->pty, gen->is_const);
        MethodSig ms;
        ms.name = gen->name;
        ms.ret = sig.ret;
        ms.params = sig.params;
        ms.is_override = true;
        ms.is_public = true;
        ms.owner = sd->ty;
        sd->ty->methods.push_back(std::move(ms));
        sd->methods.push_back(std::move(gen));
    }

    // U33 S1 / U34 M4: `struct S : I1, I2` — only interfaces may be listed. Every interface method is
    // satisfied either by one of S's own methods or, when the interface supplies a default, by an
    // instance of that default materialized with `this` = S (static dispatch).
    //
    // Runs as its own pass, before any function body is checked: a call `s.m()` in another unit's body
    // must already see the materialized signature. Layout, vtable and `super` are untouched.
    void Sema::apply_struct_iface_defaults(StructDecl* sd) {
        // The interfaces in the `:` list (each one a chain), and the (signature, declaring interface)
        // pairs they require, so that two interfaces contributing the same signature are seen together.
        std::vector<TypePtr> chains;
        std::vector<std::pair<const MethodSig*, TypePtr>> reqs;
        std::unordered_set<const Type*> visited;  // shared by the `collect` walks of every listed interface
        for (size_t i = 0; i < sd->base_specs.size(); ++i) {
            if (!sd->base_specs[i]) continue;
            TypePtr bt = resolve_type(sd->base_specs[i].get(), cur_);
            if (!bt) continue;
            if (bt->kind != TypeKind::Interface) {
                err("`" + sd->bases[i] + "`: only interfaces are allowed after `:` on a struct", sd);
                continue;
            }
            bool dup = false;
            for (const auto& c : chains) dup = dup || same_type(c, bt);
            if (dup) continue;
            chains.push_back(bt);
            std::function<void(const TypePtr&, int)> collect = [&](const TypePtr& itf, int depth) {
                if (!itf || depth > kMaxInterfaceDepth) return;
                if (!visited.insert(itf.get()).second) return;  // a DAG: shared ancestors once
                for (const auto& m : itf->methods) reqs.push_back({&m, itf});
                for (const auto& p : itf->interfaces) collect(p, depth + 1);
            };
            collect(bt, 0);
        }
        for (size_t r = 0; r < reqs.size(); ++r) {
            const MethodSig* req = reqs[r].first;
            if (req->is_operator) continue;  // operators are resolved at compile time and need no member
            bool seen_sig = false;
            for (size_t k = 0; k < r && !seen_sig; ++k) seen_sig = same_sig(*reqs[k].first, *req);
            if (seen_sig) continue;
            const std::string itf_name = reqs[r].second->name;
            const MethodSig* impl = nullptr;
            for (const auto& m : sd->ty->methods) {
                if (same_sig(m, *req)) {
                    impl = &m;
                    break;
                }
            }
            if (impl) {
                // `same_sig` deliberately ignores the return type (overloading never uses it), so the
                // "satisfies" check must compare it explicitly; otherwise `int f()` would be satisfied
                // by `long f()`.
                if (!same_type(impl->ret, req->ret)) {
                    err("override return type does not match the declaration in `" + itf_name + "`", sd);
                }
                // `override` is optional here: for a value type the `:` list already declares the
                // contract and nobody can subclass it, so the marker would not distinguish anything
                // (a class keeps it mandatory — it says "this overrides the base chain").
                continue;
            }
            // `Self` denotes the implementor type; a value type cannot spell itself in that position
            // (no interface value exists for a struct), so such a signature still needs an explicit
            // implementation — and a default returning `this` would not typecheck anyway.
            bool has_self = req->ret_self;
            for (bool s : req->params_self) has_self = has_self || s;
            if (has_self) {
                err("struct `" + sd->name + "` must implement `" + req->name + "` of interface `" +
                        itf_name + "`",
                    sd);
                continue;
            }
            // One provider per chain, most derived first; a chain whose default refines another chain's
            // replaces it (same rule as for classes). Two unrelated providers must be resolved
            // explicitly: picking one silently would make the program depend on declaration order.
            std::vector<TypePtr> providers;
            for (const auto& chain : chains) {
                TypePtr owner;
                if (!find_iface_default_decl(chain, *req, &owner)) continue;
                bool dominated = false;
                for (auto& p : providers) {
                    if (iface_extends(p, owner)) {
                        dominated = true;
                        break;
                    }
                    if (iface_extends(owner, p)) {
                        p = owner;
                        dominated = true;
                        break;
                    }
                }
                if (!dominated) providers.push_back(owner);
            }
            if (providers.empty()) {
                err("struct `" + sd->name + "` must implement `" + req->name + "` of interface `" +
                        itf_name + "`",
                    sd);
                continue;
            }
            if (providers.size() > 1) {
                err("struct `" + sd->name + "` must implement `" + req->name + "`: interfaces `" +
                        providers[0]->name + "` and `" + providers[1]->name +
                        "` provide different default implementations",
                    sd);
                continue;
            }
            materialize_iface_default(sd, providers[0], *req);
        }
    }

    void Sema::check_struct(StructDecl* sd) {
        check_const_members(sd->const_members, sd->ty, sd->is_union ? "union" : "struct", sd->name);
        // Duplicate signatures: a top-level `const` on a parameter is not part of the signature
        // (SPEC 3.7), so `m(int)` and `m(const int)` are the same method.
        std::vector<MethodDecl*> seen;
        for (auto& m : sd->methods) {
            // A materialized default points its diagnostics at the struct, not at the interface body
            // (the body's line/col would name a declaration the user cannot change here).
            Node* where = m->from_iface_default ? static_cast<Node*>(sd) : static_cast<Node*>(m.get());
            for (MethodDecl* prev : seen) {
                if (prev->name != m->name || prev->is_const != m->is_const ||
                    !same_param_sig(prev->pty, m->pty)) {
                    continue;
                }
                std::string key = m->name + "(";
                for (auto& p : m->pty) key += p->str() + ",";
                key += m->is_const ? ") const" : ")";
                err("duplicate method `" + key + "` in struct `" + sd->name + "`", where);
            }
            seen.push_back(m.get());
            if (sd->ty->field_type(m->name)) {
                err("method `" + m->name + "` in struct `" + sd->name + "` has the same name as a field", where);
            }
            if (!m->from_iface_default) {
                check_struct_method(sd, m.get());
                continue;
            }
            // The body text belongs to the interface's unit, so its type names resolve there; S's own
            // members stay reachable (a default may call any interface method, private or not). The same
            // body is also checked strictly in its declaring unit (check_interface), so a resolution this
            // relaxation would wave through cannot go unreported.
            // RAII: a diagnostic thrown from the body must not leave the context switched.
            struct CtxGuard {
                Sema* sema;
                Unit* unit;
                std::string visible;
                ~CtxGuard() {
                    sema->cur_ = unit;
                    sema->extra_visible_unit_ = visible;
                }
            } guard{this, cur_, extra_visible_unit_};
            auto info = infos_.find(m->iface_unit);
            if (info != infos_.end()) cur_ = info->second.unit;
            extra_visible_unit_ = sd->ty->unit;
            check_struct_method(sd, m.get());
        }
        std::unordered_set<std::string> ctor_keys;
        for (auto& ctor : sd->ctors) {
            std::string key;
            // The parameter mode is part of the signature: `T(ref T)` and `T(move T)` are different
            // constructors, so `ref`/`move` must appear in the key.
            for (size_t i = 0; i < ctor->pty.size(); ++i) {
                const Param* p = i < ctor->params.size() ? ctor->params[i].get() : nullptr;
                key += (p && p->by_move ? std::string("move ") : p && p->by_ref ? std::string("ref ") : std::string()) +
                       ctor->pty[i]->str() + ",";
            }
            if (!ctor_keys.insert(key).second) {
                err("duplicate constructor signature in struct `" + sd->name + "`", ctor.get());
            }
            check_struct_ctor(sd, ctor.get());
        }
        check_struct_dtor(sd);
    }

    void Sema::check_class(ClassDecl* c) {
        check_const_members(c->const_members, c->ty, "class", c->name);
        // The hierarchy walkers (`is_subclass_of`, `is_interface`) stop at a fixed depth, and every class
        // copies its base's fields: a chain deeper than this both misbehaves and costs O(n^2).
        {
            int depth = 0;
            std::unordered_set<const Type*> seen;
            for (TypePtr b = c->ty->base; b; b = b->base) {
                if (!seen.insert(b.get()).second) break;  // a cycle: the hierarchy check reports it
                if (++depth > kMaxInheritanceDepth) {
                    err("class `" + c->name + "` is more than " + std::to_string(kMaxInheritanceDepth) +
                            " levels deep in its inheritance chain; flatten the hierarchy",
                        c);
                }
            }
        }
        // A top-level `const` on a parameter is not part of the signature (SPEC 3.7).
        std::vector<MethodDecl*> seen;
        for (auto& m : c->methods) {
            MethodSig sig;
            sig.name = m->name;
            sig.is_const = m->is_const;
            sig.params = m->pty;
            for (MethodDecl* prev : seen) {
                if (prev->name != m->name || prev->is_const != m->is_const ||
                    !same_param_sig(prev->pty, m->pty)) {
                    continue;
                }
                std::string key = m->name + "(";
                for (auto& p : m->pty) key += p->str() + ",";
                key += m->is_const ? ") const" : ")";
                err("duplicate method `" + key + "` in class `" + c->name + "`", m.get());
            }
            seen.push_back(m.get());
            if (c->ty->field_type(m->name)) {
                err("method `" + m->name + "` in class `" + c->name + "` has the same name as a field", m.get());
            }

            const MethodSig* base_sig = nullptr;
            bool base_from_class = false;
            if (c->ty->base) {
                if (auto s = find_sig(c->ty->base, sig)) {
                    base_sig = s;
                    base_from_class = true;
                }
            }
            if (!base_sig) {
                for (auto& i : c->ty->interfaces) {
                    if (auto s = find_sig(i, sig)) {
                        base_sig = s;
                        break;
                    }
                }
            }
            if (base_sig) {
                // Methods are static-dispatch by default (build 160): a base method only takes part in
                // dynamic dispatch when it was declared `virtual` (or is abstract). Interface members are
                // reached through the itable and need no `virtual`.
                if (base_from_class && !base_sig->is_virtual) {
                    err("`" + base_sig->owner->str() + "::" + sig.name +
                            "` is not virtual: declare it `virtual` in `" + base_sig->owner->str() +
                            "` before overriding it (methods are static-dispatch by default)",
                        m.get());
                }
                if (!m->is_override) {
                    err("overriding or implementing a base class or interface method requires `override`",
                        m.get());
                }
                std::vector<const MethodSig*> matches;
                collect_sigs(c->ty->base ? c->ty->base : c->ty, sig, matches);
                for (auto& i : c->ty->interfaces) collect_sigs(i, sig, matches);
                for (auto* ms : matches) {
                    if (!same_type(ms->ret, m->rty)) {
                        err("override return type does not match the declaration in `" + ms->owner->str() + "`",
                            m.get());
                    }
                }
                if (!same_type(base_sig->ret, m->rty)) {
                    err("override has a mismatched return type", m.get());
                }
            } else if (m->is_override) {
                err("`override` does not match any base class or interface method", m.get());
            }
            check_method(c, m.get());
        }
        std::unordered_set<std::string> ctor_keys;
        for (auto& ctor : c->ctors) {
            std::string key;
            for (auto& p : ctor->pty) key += p->str() + ",";
            if (!ctor_keys.insert(key).second) {
                err("duplicate constructor signature in class `" + c->name + "`", ctor.get());
            }
            check_ctor(c, ctor.get());
        }
        // An implicit default constructor of a derived class requires a parameterless constructor in the base class
        if (c->ctors.empty() && c->ty->base && !c->ty->base->ctors.empty()) {
            bool base_default = false;
            for (auto& cs : c->ty->base->ctors) {
                if (cs.empty()) { base_default = true; break; }
            }
            if (!base_default) {
                err("base class of class `" + c->name + "` has no parameterless constructor; declare a constructor explicitly", c);
            }
        }
        // A derived class field must not shadow a base class method name (accessing by field name would be confused with a method call)
        if (c->ty->base) {
            for (auto& [fname, spec] : c->fields) {
                if (has_method_named(c->ty->base, fname)) {
                    err("field `" + fname + "` in class `" + c->name + "` has the same name as a base class method", c);
                }
            }
        }
        check_dtor(c);
    }

    void Sema::check_interface(InterfaceDecl* itf) {
        // The interface graph walker (`iface_extends`) stops at a fixed depth; a deeper chain would
        // silently drop inherited members. The metric is the *longest path*, not the number of visited
        // nodes: a diamond re-visits its shared ancestors, so counting visits rejects a shallow DAG
        // (`A : B, C`, both `: D`) once it fans out. Heights are memoized, so each interface is walked
        // once.
        {
            std::unordered_map<const Type*, int> height;
            std::unordered_set<const Type*> busy;
            std::function<int(const TypePtr&)> longest = [&](const TypePtr& cur) -> int {
                if (!cur) return 0;
                if (auto it = height.find(cur.get()); it != height.end()) return it->second;
                if (!busy.insert(cur.get()).second) return 0;  // a cycle: check_hierarchy_cycles reports it
                int best = 0;
                for (auto& b : cur->interfaces) best = std::max(best, longest(b) + 1);
                busy.erase(cur.get());
                height[cur.get()] = best;
                return best;
            };
            const int depth = longest(itf->ty);
            if (depth > kMaxInterfaceDepth) {
                err("interface `" + itf->name + "` inherits more than " +
                        std::to_string(kMaxInterfaceDepth) +
                        " interfaces in one chain; flatten the hierarchy",
                    itf);
            }
        }
        for (auto& m : itf->methods) {
            if (!m->body) continue;  // abstract declaration other than a default method
            begin_member(itf->ty, m->rty, itf->name + "." + m->name, m->line, m->col);
            declare_params(m->param_locals, m->params, m->pty);
            check_block(m->body.get());
            if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
                err("interface method `" + m->name + "` may reach the end of its body without returning a value", m.get());
            }
            end_member();
        }
    }

    bool Sema::class_has_concrete(const TypePtr& cls, const MethodSig& req) {
        TypePtr cur = cls;
        int guard = 0;
        while (cur && cur->kind == TypeKind::Class && guard++ < 1024) {
            for (auto& m : cur->methods) {
                if (same_sig(m, req) && !m.is_abstract) return true;
            }
            cur = cur->base;
        }
        return false;
    }

    bool Sema::iface_has_default(const TypePtr& t, const MethodSig& req, int depth,
                                 std::unordered_set<const Type*>* seen) {
        if (!t || depth > 256) return false;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return false;
        for (auto& m : t->methods) {
            if (same_sig(m, req) && !m.is_abstract) return true;
        }
        for (auto& i : t->interfaces) {
            if (iface_has_default(i, req, depth + 1, &visited)) return true;
        }
        return false;
    }

    void Sema::collect_abstract_reqs(const TypePtr& t, std::vector<MethodSig>& out,
                                      int depth, std::unordered_set<const Type*>* seen) {
        if (!t || depth > 256) return;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return;
        for (auto& m : t->methods) {
            // Operators are compile-time-only contract members: they have no vtable slot and are
            // resolved statically, so an interface operator never makes an implementor abstract.
            if (m.is_abstract && !m.is_operator) out.push_back(m);
        }
        if (t->kind == TypeKind::Class && t->base) {
            collect_abstract_reqs(t->base, out, depth + 1, &visited);
        }
        for (auto& i : t->interfaces) {
            collect_abstract_reqs(i, out, depth + 1, &visited);
        }
    }

    void Sema::collect_defaults(const TypePtr& t, std::vector<MethodSig>& out, int depth,
                                std::unordered_set<const Type*>* seen) {
        if (!t || depth > 256) return;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return;
        for (auto& m : t->methods) {
            if (!m.is_abstract) out.push_back(m);
        }
        for (auto& i : t->interfaces) {
            collect_defaults(i, out, depth + 1, &visited);
        }
    }

    void Sema::check_hierarchy_cycles() {
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (!c || !c->ty) continue;
                std::unordered_set<Type*> seen;
                TypePtr cur = c->ty;
                while (cur && cur->kind == TypeKind::Class) {
                    if (!seen.insert(cur.get()).second) err("class `" + c->name + "` has a circular inheritance chain", c);
                    cur = cur->base;
                }
            }
        }

        std::unordered_map<Type*, int> color;
        std::function<void(TypePtr)> visit = [&](TypePtr t) {
            if (!t || t->kind != TypeKind::Interface) return;
            int& col = color[t.get()];
            if (col == 1) err("circular interface inheritance: " + t->str());
            if (col == 2) return;
            col = 1;
            for (auto& b : t->interfaces) visit(b);
            col = 2;
        };
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                if (auto i = dynamic_cast<InterfaceDecl*>(d.get()); i && i->ty) visit(i->ty);
            }
        }
    }

    void Sema::compute_abstract_all() {
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (!c || !c->ty) continue;

                std::vector<MethodSig> reqs;
                collect_abstract_reqs(c->ty, reqs);
                bool abs = false;
                for (auto& req : reqs) {
                    if (!class_has_concrete(c->ty, req) && !iface_has_default(c->ty, req)) {
                        abs = true;
                        break;
                    }
                }
                c->ty->is_abstract = abs;

                std::vector<MethodSig> defaults;
                for (TypePtr cur = c->ty; cur && cur->kind == TypeKind::Class; cur = cur->base) {
                    for (auto& i : cur->interfaces) collect_defaults(i, defaults);
                }
                for (size_t i = 0; i < defaults.size(); ++i) {
                    for (size_t j = i + 1; j < defaults.size(); ++j) {
                        if (same_sig(defaults[i], defaults[j]) &&
                            !class_has_concrete(c->ty, defaults[i])) {
                            err("default interface method `" + defaults[i].name +
                                    "` is conflicting and must be overridden in the class",
                                c);
                        }
                    }
                }
            }
        }
    }

    void Sema::check_block(Block* b) {
        if (!b) return;
        push();
        // A statement-list `#if` is expanded in place (P3.3): the unit's constants are complete, so the
        // condition uses the same values ordinary code does.
        for (size_t i = 0; i < b->stmts.size();) {
            if (dynamic_cast<DirectiveStmt*>(b->stmts[i].get())) {
                expand_directive_stmt(b, i);
                continue;
            }
            check_stmt(b->stmts[i].get());
            ++i;
        }
        pop();
    }

    // Splice every statement-list `#if` of this block in place, without checking the statements. The
    // constructor path needs the list expanded before it looks for a leading `super(...)`.
    void Sema::expand_directive_stmts(Block* b) {
        if (!b) return;
        for (size_t i = 0; i < b->stmts.size();) {
            if (dynamic_cast<DirectiveStmt*>(b->stmts[i].get())) {
                expand_directive_stmt(b, i);
                continue;
            }
            ++i;
        }
    }

    void Sema::check_stmt(Stmt* s) {
        if (!s) return;
        if (auto lv = dynamic_cast<LocalVar*>(s)) {
            check_local(lv);
        } else if (auto es = dynamic_cast<ExprStmt*>(s)) {
            check_expr(es->expr.get());
        } else if (auto as = dynamic_cast<AssignStmt*>(s)) {
            check_assign_stmt(as);
        } else if (auto iff = dynamic_cast<IfStmt*>(s)) {
            check_cond(iff->cond.get(), "if condition");
            // `if x is P(v) { ... }`: the bound names are locals of the then-branch only (SPEC 10.1.2).
            IsExpr* ise = dynamic_cast<IsExpr*>(iff->cond.get());
            // A `move` inside either arm leaves the variable moved on that path only; after the `if` the
            // variable counts as moved if *any* arm moved it (may-move analysis).
            const auto movedAtEntry = moved_locals_;
            push();
            if (ise && !ise->binds.empty()) {
                for (size_t k = 0; k < ise->binds.size(); ++k) {
                    auto lv = std::make_unique<LocalVar>();
                    lv->name = ise->binds[k];
                    lv->rty = ise->bind_types[k];
                    lv->line = ise->line;
                    lv->col = ise->col;
                    declare_local(lv.get());
                    ise->bind_locals.push_back(lv.get());
                    if (cur_func_) cur_func_->synthetic_locals.push_back(std::move(lv));
                }
            }
            check_block(iff->then.get());
            pop();
            const auto afterThen = moved_locals_;
            moved_locals_ = movedAtEntry;
            if (iff->els) {
                if (auto b = dynamic_cast<Block*>(iff->els.get())) {
                    check_block(b);
                } else {
                    check_stmt(iff->els.get());
                }
            }
            for (LocalVar* lv : afterThen) moved_locals_.insert(lv);
            for (LocalVar* lv : movedAtEntry) moved_locals_.insert(lv);
        } else if (auto ws = dynamic_cast<WhileStmt*>(s)) {
            check_cond(ws->cond.get(), "while condition");
            ++loop_depth_;
            ++move_loop_count_;
            check_block(ws->body.get());
            --move_loop_count_;
            --loop_depth_;
        } else if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            ++move_loop_count_;
            check_for(fs);
            --move_loop_count_;
        } else if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            check_switch(sw);
        } else if (dynamic_cast<BreakStmt*>(s)) {
            if (in_finally_) err("`break` is not allowed inside `finally`", s);
            if (!para_stack_.empty() && loop_depth_ == para_stack_.back().loop_base)
                err("`break` cannot leave a `para` section", s);
            if (in_para_for_body_ && loop_depth_ == para_for_body_depth_)
                err("`break` cannot leave a `para for` loop", s);
            if (loop_depth_ == 0) err("`break` can only appear in a loop", s);
        } else if (dynamic_cast<ContinueStmt*>(s)) {
            if (in_finally_) err("`continue` is not allowed inside `finally`", s);
            if (!para_stack_.empty() && loop_depth_ == para_stack_.back().loop_base)
                err("`continue` cannot leave a `para` section", s);
            if (in_para_for_body_ && loop_depth_ == para_for_body_depth_)
                err("`continue` cannot leave a `para for` loop", s);
            if (loop_depth_ == 0) err("`continue` can only appear in a loop", s);
        } else if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            if (in_finally_) err("`return` is not allowed inside `finally`", rs);
            check_return(rs);
        } else if (auto ts = dynamic_cast<ThrowStmt*>(s)) {
            if (in_finally_) err("`throw` is not allowed inside `finally`; an escaping exception aborts", ts);
            if (in_dtor_) err("a destructor must not throw", ts);
            TypePtr t = check_expr(ts->value.get());
            TypePtr ex = exception_type();
            if (!ex) err("std.exception.Exception is not available", ts);
            if (!t || t->kind != TypeKind::Class || !is_subclass_of(t, ex)) {
                err("`throw` requires an `Exception` subclass, found " + (t ? t->str() : std::string("void")),
                    ts);
            }
        } else if (auto tr = dynamic_cast<TryStmt*>(s)) {
            check_block(tr->body.get());
            TypePtr ex = exception_type();
            if (!ex) err("std.exception.Exception is not available", tr);
            for (auto& c : tr->catches) {
                TypePtr ct = resolve_type(c->spec.get(), cur_);
                c->rty = ct;
                if (!ct || ct->kind != TypeKind::Class || !is_subclass_of(ct, ex)) {
                    err("`catch` type must be an `Exception` subclass, found " +
                            (ct ? ct->str() : std::string("void")),
                        c.get());
                }
                auto lv = std::make_unique<LocalVar>();
                lv->name = c->name;
                lv->rty = ct;
                lv->is_const = true;
                lv->line = c->line;
                lv->col = c->col;
                c->var = std::move(lv);
                push();
                declare_local(c->var.get());
                check_block(c->body.get());
                pop();
            }
            if (tr->finally_body) {
                const bool saved = in_finally_;
                in_finally_ = true;
                check_block(tr->finally_body.get());
                in_finally_ = saved;
            }
        } else if (auto b = dynamic_cast<Block*>(s)) {
            check_block(b);
        } else if (auto pp = dynamic_cast<ParaStmt*>(s)) {
            check_para(pp);
        } else if (auto pf = dynamic_cast<ParaForStmt*>(s)) {
            check_para_for(pf);
        } else if (auto es = dynamic_cast<ErrorStmt*>(s)) {
            // A selected `#error` arm (only reachable through a spliced `#if`).
            err("#error " + es->message, es);
        } else {
            err("unknown statement", s);
        }
    }

    void Sema::check_para(ParaStmt* p) {
        if (single_thread_) {
            // `--single-thread` drops the atomics from reference counting, so a second thread would
            // race on every shared object: reject instead of silently generating a data race.
            err("`para` needs atomic reference counting; it cannot be used with `--single-thread`", p);
            return;
        }
        p->captures.clear();
        for (auto& sec : p->sections) {
            ParaCtx ctx;
            ctx.scope_base = scopes_.size();
            ctx.loop_base = loop_depth_;
            ctx.lambda_base = lambda_stack_.size();
            para_stack_.push_back(std::move(ctx));
            push();
            check_block(sec.get());
            pop();
            ParaCtx done = std::move(para_stack_.back());
            para_stack_.pop_back();
            std::vector<LocalVar*> caps;
            for (LocalVar* lv : done.seen) {
                if (!done.declared.count(lv)) caps.push_back(lv);
            }
            p->captures.push_back(std::move(caps));
        }
    }

    void Sema::check_para_for(ParaForStmt* p) {
        if (single_thread_) {
            err("`para for` needs atomic reference counting; it cannot be used with `--single-thread`", p);
            return;
        }
        ForInStmt* f = p->loop.get();
        if (!f) return;
        if (f->has_idx) {
            err("`para for i, x` is not supported yet: iterate values and index explicitly", f);
        }
        ParaCtx ctx;
        ctx.scope_base = scopes_.size();
        ctx.loop_base = loop_depth_;
        ctx.lambda_base = lambda_stack_.size();
        para_stack_.push_back(std::move(ctx));
        pending_para_for_ = true;
        check_for(f);
        pending_para_for_ = false;
        ParaCtx done = std::move(para_stack_.back());
        para_stack_.pop_back();
        p->captures.clear();
        for (LocalVar* lv : done.seen) {
            if (!done.declared.count(lv)) p->captures.push_back(lv);
        }
    }

    bool Sema::roots_at_outer_value_local(Expr* e) const {
        if (!e || para_stack_.empty()) return false;
        LocalVar* root = nullptr;
        Expr* cur = e;
        while (cur) {
            if (auto n = dynamic_cast<Name*>(cur)) {
                if (auto lv = std::get_if<LocalVar*>(&n->sym)) root = *lv;
                break;
            }
            if (auto m = dynamic_cast<Member*>(cur)) {
                TypePtr ot = m->obj->ty;
                if (ot && (ot->kind == TypeKind::Class || ot->kind == TypeKind::Interface ||
                           ot->kind == TypeKind::Ptr))
                    return false;  // storage belongs to an object / pointed-to memory, not the caller's frame
                cur = m->obj.get();
                continue;
            }
            if (auto ix = dynamic_cast<Index*>(cur)) {
                TypePtr bt = ix->base->ty;
                if (bt && (bt->kind == TypeKind::Class || bt->kind == TypeKind::Interface ||
                           bt->kind == TypeKind::Ptr || bt->kind == TypeKind::Slice))
                    return false;  // heap buffer or explicitly shared memory
                cur = ix->base.get();
                continue;
            }
            return false;
        }
        if (!root) return false;
        if (para_stack_.back().declared.count(root)) return false;
        if (root->by_ref) return false;  // explicit borrow
        if (root->rty && (root->rty->kind == TypeKind::Class || root->rty->kind == TypeKind::Interface))
            return false;  // the handle is a value, but the object it points at is shared explicitly
        return true;
    }

    void Sema::check_local(LocalVar* lv) {
        if (lv->is_auto) {
            auto ie = dynamic_cast<InitExpr*>(lv->init.get());
            if (!ie || !ie->expr) err("`auto` must be initialized with an expression", lv);
            Expr* e = ie->expr.get();
            if (dynamic_cast<NullLit*>(e)) err("cannot infer a type from `null`", lv);
            TypePtr t = check_expr(e);
            if (is_void(t)) err("cannot infer a type from a `void` expression", lv);
            if (t->kind == TypeKind::Range) err("cannot infer a type from a range", lv);
            lv->rty = t;
            // Several auto declarators in one declaration statement must infer the same type
            auto it = auto_groups_.find(lv->group);
            if (it == auto_groups_.end()) {
                auto_groups_[lv->group] = t;
            } else if (!same_type(it->second, t)) {
                err("`auto` in the same declaration is inferred as different types: " + it->second->str() + " and " +
                        t->str(),
                    lv);
            }
        } else {
            lv->rty = resolve_type(lv->spec.get(), cur_);
            if (is_void(lv->rty)) err("variable cannot have type `void`", lv);
        }

        if (lv->is_const && !lv->init) {
            err("`const` variable `" + lv->name + "` must be initialized", lv);
        }

        if (!lv->init && lv->rty && lv->rty->kind == TypeKind::Struct &&
            !lv->rty->ctors.empty()) {
            bool has_default = false;
            for (auto& cs : lv->rty->ctors) {
                if (cs.empty()) {
                    has_default = true;
                    break;
                }
            }
            if (!has_default) {
                err("struct `" + lv->rty->str() +
                        "` has no parameterless constructor and must be initialized with a constructor",
                    lv);
            }
        }

        declare_local(lv);
        if (lv->init) check_decl_init(lv->zero, lv->rty, lv->init.get(), "initialization", lv);
    }

    void Sema::check_decl_init(bool& zero_flag, const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node) {
        if (!init) err("missing initializer", node);
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            if (ia->items.empty()) {
                if (!is_zeroable(ty)) {
                    err("type `" + ty->str() +
                            "` cannot be initialized with `{}`; only aggregate types (struct, array, slice, string) can use `{}`"
                            "use a plain constant for scalars (0, 0.0, false, null)",
                        node);
                }
                zero_flag = true;
                return;
            }
        }
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            if (auto il = dynamic_cast<IntLit*>(ie->expr.get())) {
                if (il->value == 0 && is_zeroable(ty)) {
                    err("zero-initialize aggregate type `" + ty->str() + "` with `{}`; `= 0` is no longer supported", node);
                }
            }
        }
        check_init_into(ty, init, what, node);
    }

    void Sema::check_init_into(const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node) {
        if (!init) err("missing initializer", node);
        if (auto ia0 = dynamic_cast<InitAggregate*>(init)) {
            if (ia0->items.empty()) {
                if (!is_zeroable(ty)) {
                    err("type `" + ty->str() +
                            "` cannot be initialized with `{}`; only aggregate types (struct, array, slice, string) can use `{}`"
                            "use a plain constant for scalars (0, 0.0, false, null)",
                        node);
                }
                return;
            }
        }
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            Expr* e = ie->expr.get();
            if (!e) err("missing initializer expression", node);
            if (auto il = dynamic_cast<IntLit*>(e)) {
                if (il->value == 0 && is_zeroable(ty)) {
                    err("zero-initialize aggregate type `" + ty->str() + "` with `{}`; `= 0` is no longer supported", node);
                }
            }
            if (auto call = dynamic_cast<Call*>(e);
                call && ty->kind == TypeKind::Struct && !ty->ctors.empty()) {
                direct_init_expr_ = e;
                check_expr(e, ty);
                direct_init_expr_ = nullptr;
                if (call->callee_kind == "struct_ctor") return;  // in-place construction
                if (same_type(call->ty, ty)) return;             // same-type returned value: copy initialization
                // Anything else may still be a copy-initialization through an implicit constructor:
                // `string c = s.cstr();` has the same type as `const char* p = s.cstr();` followed by
                // `string b = p;`, so the coercion rules of assignment apply here too (SPEC 3.6).
                check_assign(e, ty, what);
                return;
            }
            if (ty->kind == TypeKind::Array && is_char(ty->elem) &&
                dynamic_cast<StringLit*>(e)) {
                // A string literal initializes a fixed-size char/byte array (copied + NUL padded, 3.4).
                const std::string& bytes = static_cast<StringLit*>(e)->value;
                if (bytes.size() >= ty->array_size) {
                    err("string literal (" + std::to_string(bytes.size()) + " bytes) does not fit in `" +
                            ty->str() + "` (it needs room for the terminating NUL)",
                        e);
                }
                check_expr(e);
                return;
            }
            check_assign(e, ty, what);
            return;
        }
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            check_aggregate(ia, ty, what);
            return;
        }
        if (dynamic_cast<InitRange*>(init)) {
            err("range fill (`a..b`) is only allowed in fixed-size array initializers; "
                "write the elements out or use an index loop",
                node);
        }
        err("unknown initializer", node);
    }
}  // namespace muc_detail
