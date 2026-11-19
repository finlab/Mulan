// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds class / struct declaration checking (definitions of Sema members).
//
// owns:      declaration checks for class/struct (members, methods, constructors, destructors, const members),
//            the method/override matching, and the shared signature walkers (find_sig / collect_sigs /
//            find_method / has_method_named)
// not:       interface satisfaction and default materialization (sema_iface.cpp), statement bodies
//            (sema_stmt.cpp), expression typing, call resolution, layout, codegen
// pass:      declaration checks run before any body; interface defaults are materialized before bodies
// concept:   class-decl, struct-decl, override-matching
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
        member_used_this_ = false;
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


    const MethodSig* Sema::find_sig(const TypePtr& t, const MethodSig& sig, int depth,
                                    std::unordered_set<const Type*>* seen, const TypePtr& impl) {
        if (!t || depth > 256) return nullptr;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return nullptr;
        for (auto& m : t->methods) {
            if (impl ? same_sig(impl_sig(m, impl), sig) : same_sig(m, sig)) return &m;
        }
        if (t->kind == TypeKind::Class && t->base) {
            if (auto r = find_sig(t->base, sig, depth + 1, &visited, impl)) return r;
        }
        for (auto& i : t->interfaces) {
            if (auto r = find_sig(i, sig, depth + 1, &visited, impl)) return r;
        }
        return nullptr;
    }

    void Sema::collect_sigs(const TypePtr& t, const MethodSig& sig,
                             std::vector<const MethodSig*>& out, int depth,
                             std::unordered_set<const Type*>* seen, const TypePtr& impl) {
        if (!t || depth > 256) return;
        std::unordered_set<const Type*> local;
        std::unordered_set<const Type*>& visited = seen ? *seen : local;
        if (!first_visit(&visited, t)) return;
        for (auto& m : t->methods) {
            if (impl ? same_sig(impl_sig(m, impl), sig) : same_sig(m, sig)) out.push_back(&m);
        }
        if (t->kind == TypeKind::Class && t->base) collect_sigs(t->base, sig, out, depth + 1, &visited, impl);
        for (auto& i : t->interfaces) collect_sigs(i, sig, out, depth + 1, &visited, impl);
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

    // Delegating constructors form a chain; a cycle would recurse until the stack dies (SPEC 3.12).
    void Sema::check_ctor_delegate_cycles(const std::vector<std::unique_ptr<CtorDecl>>& ctors) {
        const size_t n = ctors.size();
        for (size_t i = 0; i < n; ++i) {
            size_t cur = i;
            for (size_t hops = 0; hops <= n; ++hops) {
                const int next = ctors[cur]->delegate_index;
                if (next < 0) break;
                if (static_cast<size_t>(next) == i) {
                    err("delegating constructors form a cycle", ctors[i].get());
                }
                cur = static_cast<size_t>(next);
            }
        }
    }

    void Sema::fix_const_class_slots(ClassDecl* c) {
        if (!c || !c->is_const_decl || !c->ty || !c->ty->base) return;
        for (auto& m : c->methods) {
            if (!m->is_override || !m->is_const) continue;
            const MethodSig* slot = find_method(c->ty->base, m->name, m->pty.size());
            if (!slot || slot->is_const || !slot->is_virtual) continue;
            if (!same_param_sig(slot->params, m->pty)) continue;
            m->is_const = false;
            m->cname = method_cname(c->ty->unit, c->name, m->name, m->pty, false);
            for (auto& sig : c->ty->methods) {
                if (sig.owner.get() != c->ty.get() || sig.name != m->name || !sig.is_const) continue;
                if (!same_param_sig(sig.params, m->pty)) continue;
                sig.is_const = false;
                sig.is_public = false;  // a slot-only method is not part of the public surface
                break;
            }
        }
    }

    void Sema::check_method(ClassDecl* c, MethodDecl* m) {
        if (!m->body) return;  // abstract method
        begin_member(c->ty, m->rty, c->name + "." + m->name, m->line, m->col);
        const bool saved_const = cur_method_const_;
        // A `const class` fixes its content after construction (SPEC 3.7): every method body is read-only,
        // including the body of an explicitly non-const override slot. Constructors and the destructor are
        // the only writable phases (they do not go through here).
        cur_method_const_ = m->is_const || (c->ty && c->ty->decl_const);
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
        // A statement-list `#if` must be expanded before the body is checked (P3.3c); the prologue check
        // looks at the body too, to point at a `super(...)` statement that replaced a base initializer.
        expand_directive_stmts(ctor->body.get());
        check_ctor_prologue(c->ty, ctor, c->field_default);
        check_block(ctor->body.get());
        in_ctor_ = false;
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
        check_ctor_prologue(sd->ty, ctor, sd->field_default);
        check_block(ctor->body.get());
        in_ctor_ = false;
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
                if (item.name == f.name) {
                    listed = true;
                    break;
                }
            }
            if (listed) continue;
            CtorInit init;
            init.kind = CtorInit::Kind::Field;
            init.name = f.name;
            init.line = ctor->line;
            init.col = ctor->col;
            init.args.push_back(clone_expr(*it->second));
            ctor->init_list.push_back(std::move(init));
        }
    }

    // An instantiated generic type is named `Box<int>`; a prologue item spells the source name (`Box`).
    static std::string plain_type_name(const std::string& n) {
        const size_t lt = n.find('<');
        return lt == std::string::npos ? n : n.substr(0, lt);
    }

    // `super(...)` used to be the base-constructor spelling. Find it in the body so the diagnosis can
    // point at the statement instead of at a "missing base call" (SPEC 3.12).
    static Call* stray_super_call(Block* body) {
        if (!body) return nullptr;
        for (auto& st : body->stmts) {
            auto es = dynamic_cast<ExprStmt*>(st.get());
            auto call = es ? dynamic_cast<Call*>(es->expr.get()) : nullptr;
            auto nm = call ? dynamic_cast<Name*>(call->callee.get()) : nullptr;
            if (nm && nm->ident == "super") return call;
        }
        return nullptr;
    }

    // `Ctor(...) : Base(a), field(v) { }` — validate the construction prologue (SPEC 3.12). The entries
    // are classified by name (base class / own class / field), the constructors are resolved, and the
    // execution order the codegen follows is fixed here: delegate → base → members → field initializers
    // (declaration order) → body.
    void Sema::check_ctor_prologue(const TypePtr& ty, CtorDecl* ctor,
                                   const std::unordered_map<std::string, ExprPtr>& defaults) {
        const bool has_class_base = ty->base && ty->base->kind == TypeKind::Class;
        size_t baseCount = has_class_base ? ty->base->fields.size() : 0;

        // 1. classify. Indices, not pointers: the field defaults are appended below, which may
        // reallocate the vector (a default member initializer is a prologue item like any other).
        int base_at = -1;
        int delegate_at = -1;
        for (size_t i = 0; i < ctor->init_list.size(); ++i) {
            CtorInit& item = ctor->init_list[i];
            if (has_class_base && plain_type_name(ty->base->name) == plain_type_name(item.name)) {
                if (base_at >= 0) err("the base constructor is initialized twice", &item);
                item.kind = CtorInit::Kind::Base;
                base_at = static_cast<int>(i);
            } else if (plain_type_name(ty->name) == plain_type_name(item.name)) {
                if (delegate_at >= 0) err("only one delegating constructor call is allowed", &item);
                item.kind = CtorInit::Kind::Delegate;
                delegate_at = static_cast<int>(i);
            } else {
                item.kind = CtorInit::Kind::Field;
            }
        }

        // 2. delegating constructor: `Derived(args) : Derived(other args) { ... }`
        if (delegate_at >= 0) {
            CtorInit* delegate_item = &ctor->init_list[static_cast<size_t>(delegate_at)];
            if (base_at >= 0) {
                err("a delegating constructor cannot also initialize the base class", delegate_item);
            }
            for (auto& item : ctor->init_list) {
                if (item.kind == CtorInit::Kind::Field) {
                    err("a delegating constructor cannot initialize fields: the delegated constructor "
                        "already runs the member step",
                        &item);
                }
            }
            if (!ty->ctors.empty()) {
                std::vector<Expr*> raw;
                for (auto& a : delegate_item->args) raw.push_back(a.get());
                const auto* sig = choose_ctor(ty->ctors, raw, "delegating constructor", delegate_item);
                for (size_t i = 0; i < delegate_item->args.size(); ++i) {
                    check_assign(delegate_item->args[i].get(), (*sig)[i], "delegating constructor argument");
                }
                if (same_param_sig(*sig, ctor->pty)) {
                    err("a delegating constructor cannot delegate to itself", delegate_item);
                }
                ctor->is_delegating = true;
                ctor->delegate_pty = *sig;
                ctor->delegate_index = static_cast<int>(sig - &ty->ctors[0]);
            } else {
                err("type `" + ty->str() + "` has no constructor to delegate to", delegate_item);
            }
            return;  // no field coverage check: the delegated constructor owns the whole object
        }

        // A non-delegating constructor takes the field defaults the list does not name
        // (`const int x = 5;`); a delegating one owns no field step at all, so it takes none (SPEC 3.12).
        apply_field_defaults(defaults, ty->fields, ctor);

        // 3. base-class call
        if (has_class_base) {
            const auto& base_ctors = ty->base->ctors;
            if (base_at >= 0) {
                CtorInit* base_item = &ctor->init_list[static_cast<size_t>(base_at)];
                if (!base_ctors.empty()) {
                    std::vector<Expr*> raw;
                    for (auto& a : base_item->args) raw.push_back(a.get());
                    const auto* sig = choose_ctor(base_ctors, raw, "base class", base_item);
                    for (size_t i = 0; i < base_item->args.size(); ++i) {
                        check_assign(base_item->args[i].get(), (*sig)[i], "base constructor argument");
                    }
                    ctor->base_pty = *sig;
                } else if (!base_item->args.empty()) {
                    err("base class `" + ty->base->name + "` has no constructor taking arguments", base_item);
                }
                ctor->has_base_call = true;
            } else {
                bool base_default = false;
                for (auto& cs : base_ctors) {
                    if (cs.empty()) { base_default = true; break; }
                }
                if (!base_ctors.empty() && !base_default) {
                    if (ctor->is_synthesized) {
                        err("type `" + ty->name + "` declares no constructor, but its base class `" +
                                ty->base->name + "` has no parameterless one: declare a constructor — `" +
                                ty->name + "(...) : " + ty->base->name + "(...) { ... }`",
                            ctor);
                    }
                    if (Call* stray = stray_super_call(ctor->body.get())) {
                        err("a base-class constructor call is not a statement: write it in the "
                            "constructor's initializer list — `" + ty->name + "(args) : " + ty->base->name +
                                "(args) { ... }` (SPEC 3.12)",
                            stray);
                    }
                    err("base class `" + ty->base->name +
                            "` has no parameterless constructor; initialize it as `: " + ty->base->name +
                            "(...)`",
                        ctor);
                }
            }
        }

        // 4. field initializers
        std::unordered_set<std::string> inited;
        for (auto& item : ctor->init_list) {
            if (item.kind != CtorInit::Kind::Field) continue;
            if (item.args.size() != 1) {
                err("field initializer `" + item.name + "` takes exactly one value", &item);
            }
            const int idx = ty->field_index(item.name);
            if (idx < 0) {
                err("constructor initializer names an unknown field `" + item.name + "`", &item);
            }
            if (static_cast<size_t>(idx) < baseCount) {
                err("cannot initialize the base class field `" + item.name +
                        "` here: the base constructor initializes it",
                    &item);
            }
            if (!inited.insert(item.name).second) {
                err("field `" + item.name + "` is initialized twice", &item);
            }
            check_assign(item.args[0].get(), ty->fields[static_cast<size_t>(idx)].type,
                         "constructor initializer for `" + item.name + "`");
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


    void Sema::check_struct(StructDecl* sd) {
        check_const_members(sd->const_members, sd->ty, sd->is_union ? "union" : "struct", sd->name);
        if (!sd->promotes.empty()) check_promotes(sd->ty, sd->promotes, sd);
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
        check_ctor_delegate_cycles(sd->ctors);
        check_struct_dtor(sd);
    }

    void Sema::check_class(ClassDecl* c) {
        check_const_members(c->const_members, c->ty, "class", c->name);
        if (!c->promotes.empty()) check_promotes(c->ty, c->promotes, c);
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
        // (A `const class`'s override slots were resolved before any body was checked, so the sigs below
        // already carry the slot's signature — see `fix_const_class_slots`, SPEC 3.7.)
        // A top-level `const` on a parameter is not part of the signature (SPEC 3.7).
        std::vector<MethodDecl*> seen;
        for (auto& m : c->methods) {
            if (m->from_iface_default) {
                // Materialized from an interface default (a default whose body reads `this`): the body
                // text belongs to the interface's unit, so its type names resolve there while the
                // class's own members stay reachable — the same relaxation `check_struct` applies.
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
                extra_visible_unit_ = c->ty->unit;
                check_method(c, m.get());
                continue;
            }
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
                    if (auto s = find_sig(i, sig, 0, nullptr, c->ty)) {
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
                for (auto& i : c->ty->interfaces) collect_sigs(i, sig, matches, 0, nullptr, c->ty);
                // A `ref T` implementation satisfies a declared `T`: a borrow is usable as a value
                // (StdLibSPEC §6.2, the same rule the structural path applies). The itable thunk loads
                // through the borrow.
                auto ret_ok = [](const TypePtr& want, const TypePtr& got) {
                    return same_type(want, got) || same_type(want, referent_of(got));
                };
                for (auto* ms : matches) {
                    if (!ret_ok(ms->ret, m->rty)) {
                        err("override return type does not match the declaration in `" + ms->owner->str() + "`",
                            m.get());
                    }
                }
                if (!ret_ok(base_sig->ret, m->rty)) {
                    err("override has a mismatched return type", m.get());
                }
            } else if (m->is_override) {
                // A `const` mismatch is by far the most common way an `override` misses: the method is
                // `const` (implicitly so in a `const class`) while the interface member is not, or the
                // other way round. Say which direction it is instead of "does not match anything".
                const MethodSig* loose = nullptr;
                if (c->ty->base) loose = find_method(c->ty->base, sig.name, sig.params.size());
                for (auto& i : c->ty->interfaces) {
                    if (loose) break;
                    if (auto s2 = find_method(i, sig.name, sig.params.size())) loose = s2;
                }
                if (loose && same_param_sig(loose->params, sig.params) &&
                    loose->is_const != sig.is_const) {
                    if (sig.is_const) {
                        err("method `" + sig.name + "` is `const`, but `" + loose->owner->str() +
                                "` declares it without `const`; declare it "
                                "`" + (m->rty ? m->rty->str() : std::string("T")) + " " + sig.name +
                                "(...) const;` in the interface, or make the implementation non-`const`",
                            m.get());
                    } else {
                        err("method `" + sig.name + "` must be `const`: `" + loose->owner->str() +
                                "` declares it `const`, and a method that may mutate cannot satisfy a "
                                "read-only contract",
                            m.get());
                    }
                } else if (loose && mentions_self(*loose)) {
                    // The one case that reads as "the signatures look identical but cannot match": the
                    // contract's slot is `Self`, and the implementor has to spell its own type there.
                    err("`" + sig.name + "` must spell the implementor's own type: the contract declares `" +
                            sig_text(impl_sig(*loose, c->ty)) +
                            "` (`Self` denotes the implementor, SPEC 3.15.6)",
                        m.get());
                } else {
                    err("`override` does not match any base class or interface method", m.get());
                }
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
        check_ctor_delegate_cycles(c->ctors);
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


}  // namespace muc_detail
