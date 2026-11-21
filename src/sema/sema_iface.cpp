// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Interface satisfaction and default materialization (definitions of Sema members).
//
// owns:      what it means for a type to satisfy an interface — the nominal check with `Self` substituted,
//            the abstract-member set, and the materialization of interface defaults as ordinary methods
//            (struct and class), plus the interface/base hierarchy cycle check
// not:       class/struct member checks (sema_class1.cpp), statement bodies, expression or call checks
// pass:      before any body is checked: materialized defaults must already be visible to call sites
// concept:   interface-satisfaction, iface-defaults, abstract-set, hierarchy-cycles

#include "sema_impl.hpp"

#include "ast_clone.hpp"

namespace muc_detail {

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
    // Monomorphization, not inheritance: S gains no layout, no vtable slot and no `super`; the result is
    // an ordinary method that the normal passes check and emit.
    void Sema::materialize_iface_default(const TypePtr& ty, std::vector<std::unique_ptr<MethodDecl>>& decls,
                                         const std::string& type_name, const TypePtr& itf,
                                         const MethodSig& sig) {
        MethodDecl* def = find_iface_default_decl(itf, sig);
        if (!def) return;
        // `Self` slots become the implementor: the clone is an ordinary method of `ty`.
        const MethodSig inst = impl_sig(sig, ty);
        auto gen = clone_method(*def);
        gen->is_override = true;
        gen->is_public = true;
        gen->from_iface_default = true;
        gen->iface_unit = itf->unit;
        // It is an ordinary method of the implementor now; the static-only marker stays on the interface.
        gen->body_self = false;
        // The signature is the interface's, already resolved in the interface's unit: a default written
        // in another unit may name types that S's unit does not see, so it must not be re-resolved here.
        gen->rty = inst.ret;
        gen->ret_ref = is_borrow_type(inst.ret);
        gen->pty = inst.params;
        gen->cname = method_cname(ty->unit, type_name, gen->name,
                                  method_decl_symbol_params(gen.get()), gen->is_const);
        MethodSig ms;
        ms.name = gen->name;
        ms.ret = inst.ret;
        ms.params = inst.params;
        // The materialized signature must carry the same const/ref pattern as the interface member;
        // without it a const default is called as a non-const method (wrong symbol / wrong filtering).
        ms.is_const = inst.is_const;
        ms.params_ref = inst.params_ref;
        ms.params_cref = inst.params_cref;
        ms.params_self = inst.params_self;
        ms.is_override = true;
        ms.is_public = true;
        ms.owner = ty;
        ty->methods.push_back(std::move(ms));
        decls.push_back(std::move(gen));
    }

    void Sema::materialize_iface_default(StructDecl* sd, const TypePtr& itf, const MethodSig& sig) {
        materialize_iface_default(sd->ty, sd->methods, sd->name, itf, sig);
    }

    // A class normally reaches an interface default through the itable. A default whose body reads
    // `this` cannot: the receiver would have to be the implementor, which the dynamic form does not know.
    // Such a default is materialized into the class as an ordinary static-dispatch method, exactly as
    // `struct S : I` does (and it takes no itable slot — see buildIfaceSlots).
    void Sema::apply_class_iface_defaults(ClassDecl* c) {
        std::vector<TypePtr> chains;
        std::unordered_set<const Type*> seen;
        std::function<void(const TypePtr&, int)> collect = [&](const TypePtr& itf, int depth) {
            if (!itf || depth > kMaxInterfaceDepth) return;
            if (!seen.insert(itf.get()).second) return;
            chains.push_back(itf);
            for (const auto& p : itf->interfaces) collect(p, depth + 1);
        };
        for (const auto& i : c->ty->interfaces) collect(i, 0);

        std::vector<MethodSig> done;
        for (const auto& chain : chains) {
            for (const auto& req : chain->methods) {
                // A default that reads `this`, or whose signature mentions `Self`, has no itable slot
                // (SPEC 3.15.6), so the dynamic form cannot reach its body: materialize it per implementor.
                if (req.is_operator || !(req.body_self || mentions_self(req))) continue;
                bool dup = false;
                for (const auto& d : done) dup = dup || same_sig(d, req);
                if (dup) continue;
                if (class_has_concrete(c->ty, req)) {  // the class already declares it
                    done.push_back(req);
                    continue;
                }
                TypePtr owner;
                if (!find_iface_default_decl(chain, req, &owner)) continue;
                materialize_iface_default(c->ty, c->methods, c->name, owner, req);
                done.push_back(req);
            }
        }
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
            // `Self` denotes this struct (SPEC 3.15.6); the declared signature stores the interface type
            // in that slot, so compare against the substituted form.
            const MethodSig want = impl_sig(*req, sd->ty);
            bool seen_sig = false;
            for (size_t k = 0; k < r && !seen_sig; ++k) {
                seen_sig = same_sig(impl_sig(*reqs[k].first, sd->ty), want);
            }
            if (seen_sig) continue;
            const std::string itf_name = reqs[r].second->name;
            const MethodSig* impl = nullptr;
            for (const auto& m : sd->ty->methods) {
                if (same_sig(m, want)) {
                    impl = &m;
                    break;
                }
            }
            if (impl) {
                // `same_sig` deliberately ignores the return type (overloading never uses it), so the
                // "satisfies" check must compare it explicitly; otherwise `int f()` would be satisfied
                // by `long f()`.
                if (!same_type(impl->ret, want.ret)) {
                    err("override return type does not match the declaration in `" + itf_name + "`", sd);
                }
                // `override` is optional here: for a value type the `:` list already declares the
                // contract and nobody can subclass it, so the marker would not distinguish anything
                // (a class keeps it mandatory — it says "this overrides the base chain").
                continue;
            }
            // Both kinds of default are materialized here: a `Self` in the signature is substituted with
            // this struct (`impl_sig`), and a body that reads `this` already has the right receiver.
            // Neither form has an itable slot (SPEC 3.15.6), so materialization is the only way to reach
            // the default body.
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
                err("struct `" + sd->name + "` must implement `" + sig_text(want) + "` of interface `" +
                        itf_name + "`",
                    sd);
                continue;
            }
            if (providers.size() > 1) {
                err("struct `" + sd->name + "` must implement `" + sig_text(want) + "`: interfaces `" +
                        providers[0]->name + "` and `" + providers[1]->name +
                        "` provide different default implementations",
                    sd);
                continue;
            }
            materialize_iface_default(sd, providers[0], *req);
        }
    }

    // ---------------------------------------------------------------- `extend I with J;` (SPEC 3.18)

    bool Sema::iface_chain_has(const TypePtr& iface, const TypePtr& target, int depth) {
        if (!iface || !target || depth > kMaxInterfaceDepth) return false;
        if (same_type(iface, target)) return true;
        for (const auto& p : iface->interfaces) {
            if (iface_chain_has(p, target, depth + 1)) return true;
        }
        return false;
    }

    bool Sema::unit_can_see(const Unit* u, const std::string& other) {
        if (!u) return false;
        if (u->name == other) return true;
        for (const auto& us : u->uses) {
            if (us->unit_name == other) return true;
        }
        return false;
    }

    bool Sema::extension_visible(const MethodSig& sig) const {
        if (sig.ext_unit.empty()) return true;
        if (!cur_) return false;
        return unit_can_see(cur_, sig.ext_unit);
    }

    TypePtr Sema::unit_level_type(const std::string& name, Unit* u) {
        if (TypePtr t = find_type_named(u->name, name, false)) return t;
        for (const auto& us : u->uses) {
            if (!unit_visible(us->unit_name)) continue;
            if (TypePtr t = find_type_named(us->unit_name, name, true)) return t;
        }
        return nullptr;
    }

    void Sema::apply_interface_extensions() {
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto* ed = dynamic_cast<ExtendDecl*>(d.get());
                if (!ed) continue;
                recover([&] { apply_one_extension(ed, u.get()); });
            }
        }
    }

    void Sema::apply_one_extension(ExtendDecl* ed, Unit* u) {
        TypePtr itf = unit_level_type(ed->target, u);
        if (!itf) {
            err("`extend`: unknown interface `" + ed->target + "`", ed);
            return;
        }
        if (itf->kind != TypeKind::Interface) {
            err("`extend` applies to an interface (SPEC 3.18); `" + ed->target + "` is " +
                    (itf->kind == TypeKind::Class ? "a class" : "not an interface"),
                ed);
            return;
        }
        TypePtr ext = unit_level_type(ed->with, u);
        if (!ext) {
            err("`extend`: unknown interface `" + ed->with + "`", ed);
            return;
        }
        if (ext->kind != TypeKind::Interface) {
            err("`extend I with J` needs `J` to be an interface; `" + ed->with + "` is not one", ed);
            return;
        }
        // `J : I`: the extension only refines the interface it extends.
        if (!iface_chain_has(ext, itf)) {
            err("`extend " + ed->target + " with " + ed->with + "`: `" + ed->with + "` does not derive "
                    "from `" + ed->target + "` (declare it `interface " + ed->with + " : " + ed->target +
                    "`)",
                ed);
            return;
        }
        // Default methods only: an abstract member of `J` must already be declared by `I`.
        for (const auto& m : ext->methods) {
            if (!m.is_abstract) continue;
            bool in_base = false;
            for (const auto& b : itf->methods) {
                if (same_sig(b, m)) {
                    in_base = true;
                    break;
                }
            }
            if (!in_base) {
                err("`extend " + ed->target + " with " + ed->with + "`: `" + m.name +
                        "` is abstract but `" + ed->target + "` does not declare it; an extension can "
                        "only add default methods",
                    ed);
                return;
            }
        }

        // Apply to every type implementing `I`. The extension's visibility is enforced at call sites
        // (`filter_extension_visibility`), so a type declared in a unit that never sees the extension
        // still gains the method for the units that do.
        for (auto& tu : units_) {
            cur_ = tu.get();
            for (auto& d : tu->decls) {
                TypePtr ty;
                std::vector<std::unique_ptr<MethodDecl>>* decls = nullptr;
                std::string type_name;
                std::vector<TypePtr> chains;
                if (auto* sd = dynamic_cast<StructDecl*>(d.get()); sd && sd->ty && !sd->ty->is_union) {
                    ty = sd->ty;
                    decls = &sd->methods;
                    type_name = sd->name;
                    // A struct keeps its `:` list as specs; they are already resolved by now.
                    for (auto& spec : sd->base_specs) {
                        if (!spec) continue;
                        TypePtr bt = resolve_type(spec.get(), tu.get());
                        if (bt && bt->kind == TypeKind::Interface) chains.push_back(bt);
                    }
                } else if (auto* cd = dynamic_cast<ClassDecl*>(d.get()); cd && cd->ty) {
                    ty = cd->ty;
                    decls = &cd->methods;
                    type_name = cd->name;
                    for (TypePtr b = cd->ty; b && b->kind == TypeKind::Class; b = b->base) {
                        for (auto& i : b->interfaces) chains.push_back(i);
                    }
                } else {
                    continue;
                }
                bool implements = false;
                for (auto& c : chains) {
                    if (iface_chain_has(c, itf)) {
                        implements = true;
                        break;
                    }
                }
                if (!implements) continue;
                for (const auto& m : ext->methods) {
                    if (m.is_abstract || m.is_operator) continue;
                    // Already provided by the type itself or by an interface default: keep that one.
                    const MethodSig* have = nullptr;
                    for (const auto& existing : ty->methods) {
                        if (same_sig(existing, m)) {
                            have = &existing;
                            break;
                        }
                    }
                    if (have) {
                        // A method already present is kept (the type's own, or an interface default).
                        // Another extension providing the same signature is an ambiguity, not a merge.
                        if (!have->ext_unit.empty() && have->ext_unit != u->name) {
                            err("`extend`: `" + m.name + "` is provided for `" + ty->str() +
                                    "` by extensions in both `" + have->ext_unit + "` and `" + u->name +
                                    "`; remove one of them",
                                ed);
                            return;
                        }
                        continue;
                    }
                    const size_t before = ty->methods.size();
                    materialize_iface_default(ty, *decls, type_name, ext, m);
                    if (ty->methods.size() > before) ty->methods.back().ext_unit = u->name;
                }
            }
        }
        cur_ = u;
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
        for (size_t mi = 0; mi < itf->methods.size(); ++mi) {
            auto& m = itf->methods[mi];
            if (!m->body) continue;  // abstract declaration other than a default method
            begin_member(itf->ty, m->rty, itf->name + "." + m->name, m->line, m->col);
            declare_params(m->param_locals, m->params, m->pty);
            check_block(m->body.get());
            if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
                err("interface method `" + m->name + "` may reach the end of its body without returning a value", m.get());
            }
            // Reading `this` makes the default static-only: the receiver is the implementor, and a body
            // dispatched through the itable only has `{object, descriptor}`.
            if (member_used_this_ && mi < itf->ty->methods.size()) {
                m->body_self = true;
                itf->ty->methods[mi].body_self = true;
            }
            member_used_this_ = false;
            end_member();
        }
    }

    bool Sema::class_has_concrete(const TypePtr& cls, const MethodSig& req) {
        // `Self` in the requirement denotes this class (SPEC 3.15.6).
        const MethodSig want = impl_sig(req, cls);
        TypePtr cur = cls;
        int guard = 0;
        while (cur && cur->kind == TypeKind::Class && guard++ < 1024) {
            for (auto& m : cur->methods) {
                if (same_sig(m, want) && !m.is_abstract) return true;
            }
            cur = cur->base;
        }
        return false;
    }

    bool Sema::iface_has_default(const TypePtr& t, const MethodSig& req, int depth,
                                 std::unordered_set<const Type*>* seen, const TypePtr& impl) {
        if (!t || depth > 256) return false;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return false;
        const MethodSig need = impl ? impl_sig(req, impl) : req;
        for (auto& m : t->methods) {
            // A default that mentions `Self` has no itable slot and is not materialized yet, so it does
            // not make the implementor concrete: the type must implement it explicitly (backlog U34).
            if (impl && mentions_self(m)) continue;
            const MethodSig want = impl ? impl_sig(m, impl) : m;
            if (same_sig(want, need) && !m.is_abstract) return true;
        }
        for (auto& i : t->interfaces) {
            if (iface_has_default(i, req, depth + 1, &visited, impl)) return true;
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
                c->ty->missing_members.clear();
                for (auto& req : reqs) {
                    if (!class_has_concrete(c->ty, req) && !iface_has_default(c->ty, req, 0, nullptr, c->ty)) {
                        abs = true;
                        c->ty->missing_members.push_back(req.owner->str() + "::" + req.name);
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
                            defaults[i].owner != defaults[j].owner &&
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

    void Sema::init_interface_operator(MethodDecl* m, MethodSig& sig, const std::string& iface_name) {
        // The receiver is the first operand: an interface member has no `this`, so `Self` is the only
        // way to say the operator applies to the implementor. Only that operand may borrow (the rest
        // are ordinary operands, e.g. the byte of a stream).
        if (m->params.empty() || !m->params[0]->spec || !m->params[0]->spec->is_self) {
            err("interface `" + iface_name +
                    "`: an operator must spell its receiver as the first `Self` operand "
                    "(write `T operator" + m->name.substr(8) + "(Self c, ...)`)",
                m);
        }
        for (size_t i = 1; i < m->params.size(); ++i) {
            if (m->params[i]->by_ref) {
                err("interface `" + iface_name +
                        "`: only the receiver (the first operand) may be a `ref` parameter",
                    m->params[i].get());
            }
        }
        // The witness type has no storage, so a borrow return can only mean "the element the
        // subscription exposes"; every other operator must return a value.
        if (m->ret_ref && m->name != "operator[]") {
            err("interface `" + iface_name +
                    "`: only `operator[]` may return a borrow; another operator's receiver is a value, "
                    "not storage",
                m);
        }
        if (!m->params.empty()) {
            sig.recv_ref = m->params[0]->by_ref;
            sig.recv_cref = sig.recv_ref && m->params[0]->is_const;
        }
    }

}  // namespace muc_detail
