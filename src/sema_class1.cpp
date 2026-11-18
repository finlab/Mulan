// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds class / interface checking (part 1) (definitions of Sema members).
//
// owns:      declaration checks for class/struct/interface (members, methods, ctors, dtors, const members),
//            hierarchy cycles, the abstract-member set, interface satisfaction and default materialization
//            (apply_*_iface_defaults, impl_sig, materialize_iface_default), plus block/statement bodies
// not:       expression typing, call/overload resolution, layout, codegen; for/switch/return checks live in
//            sema_class2.cpp — the statement pass is split across the two files (see agents/code-map.md)
// pass:      declaration checks run before any body; interface defaults are materialized before bodies
// concept:   class-decl, struct-decl, interface-satisfaction, iface-defaults, statements
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

    // The interface and base-class graphs are DAGs, so a plain recursion visits shared ancestors once per
    // path — exponentially many times in a diamond, which a generated program can exploit (`interface
    // A{i} : A{i-1}, B{i-1}` doubles the paths per level). Every walk below takes a visited set.
    static bool first_visit(std::unordered_set<const Type*>* seen, const TypePtr& t) {
        if (!seen) return true;
        return seen->insert(t.get()).second;
    }

    static bool mentions_self(const MethodSig& sig) {
        if (sig.ret_self) return true;
        for (bool s : sig.params_self)
            if (s) return true;
        return false;
    }

    // `name(T1, T2)` with the implementor already substituted: the diagnostic has to name the exact
    // signature the implementor is missing (SPEC 3.10), not just the method name.
    static std::string sig_text(const MethodSig& sig) {
        std::string out = sig.name + "(";
        for (size_t i = 0; i < sig.params.size(); ++i) {
            if (i) out += ", ";
            out += sig.params[i] ? sig.params[i]->str() : std::string("?");
        }
        out += ")";
        return out;
    }

    // `Self` in an interface member signature denotes the implementor (SPEC 3.15.6), but the declared
    // signature keeps the interface type in that slot. Comparing a declaration against an implementation
    // must therefore substitute the implementor first: the constraint check already substitutes
    // (sema_generic.cpp), and without the same substitution here the `:` check and the constraint check
    // disagree, so no type can satisfy both.
    static MethodSig impl_sig(const MethodSig& sig, const TypePtr& impl) {
        MethodSig out = sig;
        // Operator members keep their own rule (the receiver is dropped from the member form, and the
        // remaining operands are matched as declared — SPEC 3.11/3.15.5), so they are left untouched.
        if (out.is_operator) return out;
        if (out.ret_self) {
            out.ret = impl;
            out.ret_self = false;
        }
        for (size_t i = 0; i < out.params_self.size() && i < out.params.size(); ++i) {
            if (out.params_self[i]) {
                out.params[i] = impl;
                out.params_self[i] = false;
            }
        }
        return out;
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
        gen->cname = method_cname(ty->unit, type_name, gen->name, gen->pty, gen->is_const);
        MethodSig ms;
        ms.name = gen->name;
        ms.ret = inst.ret;
        ms.params = inst.params;
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
                if (req.is_operator || !req.body_self) continue;
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
            // `Self` denotes the implementor type; a value type cannot spell itself in that position
            // (no interface value exists for a struct), so such a signature still needs an explicit
            // implementation — and a default returning `this` would not typecheck anyway.
            if (mentions_self(*req)) {
                err("struct `" + sd->name + "` must implement `" + sig_text(want) + "` of interface `" +
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
        check_ctor_delegate_cycles(sd->ctors);
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
