// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 本文件包含 class / interface 检查（上）（Sema 成员函数定义）。
#include "sema_impl.hpp"

namespace muc_detail {

    void Sema::begin_member(const TypePtr& cls, const TypePtr& ret, const std::string& name, int line,
                      int col) {
        scopes_.clear();
        auto_groups_.clear();
        scopes_.emplace_back();
        loop_depth_ = 0;
        cur_class_ = cls;
        cur_ret_ = ret;
        cur_name_ = name;
        in_ctor_ = false;
        in_dtor_ = false;
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
            lv->line = params[i]->line;
            lv->col = params[i]->col;
            declare_local(lv.get());
            out.push_back(std::move(lv));
        }
    }

    bool Sema::same_sig(const MethodSig& a, const MethodSig& b) {
        if (a.name != b.name || a.params.size() != b.params.size()) return false;
        for (size_t i = 0; i < a.params.size(); ++i) {
            if (!same_type(a.params[i], b.params[i])) return false;
        }
        return true;
    }

    const MethodSig* Sema::find_sig(const TypePtr& t, const MethodSig& sig, int depth) {
        if (!t || depth > 256) return nullptr;
        for (auto& m : t->methods) {
            if (same_sig(m, sig)) return &m;
        }
        if (t->kind == TypeKind::Class && t->base) {
            if (auto r = find_sig(t->base, sig, depth + 1)) return r;
        }
        for (auto& i : t->interfaces) {
            if (auto r = find_sig(i, sig, depth + 1)) return r;
        }
        return nullptr;
    }

    void Sema::collect_sigs(const TypePtr& t, const MethodSig& sig,
                             std::vector<const MethodSig*>& out, int depth) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (same_sig(m, sig)) out.push_back(&m);
        }
        if (t->kind == TypeKind::Class && t->base) collect_sigs(t->base, sig, out, depth + 1);
        for (auto& i : t->interfaces) collect_sigs(i, sig, out, depth + 1);
    }

    const MethodSig* Sema::find_method(const TypePtr& t, const std::string& name, size_t argc,
                                        int depth) {
        if (!t || depth > 256) return nullptr;
        for (auto& m : t->methods) {
            if (m.name == name && m.params.size() == argc) return &m;
        }
        if (t->kind == TypeKind::Class && t->base) {
            if (auto r = find_method(t->base, name, argc, depth + 1)) return r;
        }
        for (auto& i : t->interfaces) {
            if (auto r = find_method(i, name, argc, depth + 1)) return r;
        }
        return nullptr;
    }

    bool Sema::has_method_named(const TypePtr& t, const std::string& name, int depth) {
        if (!t || depth > 256) return false;
        for (auto& m : t->methods) {
            if (m.name == name) return true;
        }
        if (t->kind == TypeKind::Class && t->base && has_method_named(t->base, name, depth + 1)) {
            return true;
        }
        for (auto& i : t->interfaces) {
            if (has_method_named(i, name, depth + 1)) return true;
        }
        return false;
    }

    void Sema::check_method(ClassDecl* c, MethodDecl* m) {
        if (!m->body) return;  // 抽象方法
        begin_member(c->ty, m->rty, c->name + "." + m->name, m->line, m->col);
        declare_params(m->param_locals, m->params, m->pty);
        check_block(m->body.get());
        if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
            err("method `" + m->name + "` may reach the end of its body without returning a value", m);
        }
        end_member();
    }

    void Sema::check_ctor(ClassDecl* c, CtorDecl* ctor) {
        begin_member(c->ty, t_void(), c->name + " constructor", ctor->line, ctor->col);
        in_ctor_ = true;
        declare_params(ctor->param_locals, ctor->params, ctor->pty);

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
        declare_params(m->param_locals, m->params, m->pty);
        check_block(m->body.get());
        if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
            err("method `" + m->name + "` may reach the end of its body without returning a value", m);
        }
        end_member();
    }

    void Sema::check_struct_ctor(StructDecl* sd, CtorDecl* ctor) {
        begin_member(sd->ty, t_void(), sd->name + " constructor", ctor->line, ctor->col);
        in_ctor_ = true;
        declare_params(ctor->param_locals, ctor->params, ctor->pty);
        check_block(ctor->body.get());
        end_member();
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
        // U33 S1：`struct S : I1, I2` —— 只能列接口；静态满足，不产生值类型。
        for (size_t i = 0; i < sd->base_specs.size(); ++i) {
            if (!sd->base_specs[i]) continue;
            TypePtr bt = resolve_type(sd->base_specs[i].get(), cur_);
            if (!bt) continue;
            if (bt->kind != TypeKind::Interface) {
                err("`" + sd->bases[i] + "`: only interfaces are allowed after `:` on a struct", sd);
            }
            // 接口方法（含接口继承链；默认方法也要求显式实现，见 backlog U33）
            std::vector<const MethodSig*> reqs;
            std::function<void(const TypePtr&, int)> collect = [&](const TypePtr& itf, int depth) {
                if (!itf || depth > 64) return;
                for (const auto& m : itf->methods) reqs.push_back(&m);
                for (const auto& p : itf->interfaces) collect(p, depth + 1);
            };
            collect(bt, 0);
            for (const MethodSig* req : reqs) {
                if (req->is_operator) continue;  // 运算符不进 vtable，也不要求成员实现
                const MethodSig* impl = nullptr;
                for (const auto& m : sd->ty->methods) {
                    if (same_sig(m, *req)) {
                        impl = &m;
                        break;
                    }
                }
                if (!impl) {
                    err("struct `" + sd->name + "` must implement `" + req->name + "` of interface `" +
                            bt->name + "`",
                        sd);
                    continue;
                }
                if (!impl->is_override) {
                    err("method `" + req->name + "` of struct `" + sd->name + "` implements `" + bt->name +
                            "." + req->name + "` and must be marked `override`",
                        sd);
                }
            }
        }

        check_const_members(sd->const_members, sd->ty, sd->is_union ? "union" : "struct", sd->name);
        std::unordered_set<std::string> seen;
        for (auto& m : sd->methods) {
            std::string key = m->name + "(";
            for (auto& p : m->pty) key += p->str() + ",";
            key += ")";
            if (!seen.insert(key).second) {
                err("duplicate method `" + key + "` in struct `" + sd->name + "`", m.get());
            }
            if (sd->ty->field_type(m->name)) {
                err("method `" + m->name + "` in struct `" + sd->name + "` has the same name as a field", m.get());
            }
            check_struct_method(sd, m.get());
        }
        std::unordered_set<std::string> ctor_keys;
        for (auto& ctor : sd->ctors) {
            std::string key;
            for (auto& p : ctor->pty) key += p->str() + ",";
            if (!ctor_keys.insert(key).second) {
                err("duplicate constructor signature in struct `" + sd->name + "`", ctor.get());
            }
            check_struct_ctor(sd, ctor.get());
        }
        check_struct_dtor(sd);
    }

    void Sema::check_class(ClassDecl* c) {
        check_const_members(c->const_members, c->ty, "class", c->name);
        std::unordered_set<std::string> seen;
        for (auto& m : c->methods) {
            MethodSig sig;
            sig.name = m->name;
            sig.params = m->pty;
            std::string key = m->name + "(";
            for (auto& p : m->pty) key += p->str() + ",";
            key += ")";
            if (!seen.insert(key).second) {
                err("duplicate method `" + key + "` in class `" + c->name + "`", m.get());
            }
            if (c->ty->field_type(m->name)) {
                err("method `" + m->name + "` in class `" + c->name + "` has the same name as a field", m.get());
            }

            const MethodSig* base_sig = nullptr;
            if (c->ty->base) base_sig = find_sig(c->ty->base, sig);
            if (!base_sig) {
                for (auto& i : c->ty->interfaces) {
                    if (auto s = find_sig(i, sig)) {
                        base_sig = s;
                        break;
                    }
                }
            }
            if (base_sig) {
                if (!m->is_override) {
                    err("overriding or implementing a base class method requires `override`", m.get());
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
        // 派生类隐式默认构造器要求基类有无参构造器
        if (c->ctors.empty() && c->ty->base && !c->ty->base->ctors.empty()) {
            bool base_default = false;
            for (auto& cs : c->ty->base->ctors) {
                if (cs.empty()) { base_default = true; break; }
            }
            if (!base_default) {
                err("base class of class `" + c->name + "` has no parameterless constructor; declare a constructor explicitly", c);
            }
        }
        // 派生类字段不能遮蔽基类方法名（用字段名访问会与方法调用混淆）
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
        for (auto& m : itf->methods) {
            if (!m->body) continue;  // 默认方法之外的抽象声明
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

    bool Sema::iface_has_default(const TypePtr& t, const MethodSig& req, int depth) {
        if (!t || depth > 256) return false;
        for (auto& m : t->methods) {
            if (same_sig(m, req) && !m.is_abstract) return true;
        }
        for (auto& i : t->interfaces) {
            if (iface_has_default(i, req, depth + 1)) return true;
        }
        return false;
    }

    void Sema::collect_abstract_reqs(const TypePtr& t, std::vector<MethodSig>& out,
                                      int depth) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (m.is_abstract) out.push_back(m);
        }
        if (t->kind == TypeKind::Class && t->base) {
            collect_abstract_reqs(t->base, out, depth + 1);
        }
        for (auto& i : t->interfaces) {
            collect_abstract_reqs(i, out, depth + 1);
        }
    }

    void Sema::collect_defaults(const TypePtr& t, std::vector<MethodSig>& out, int depth) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (!m.is_abstract) out.push_back(m);
        }
        for (auto& i : t->interfaces) {
            collect_defaults(i, out, depth + 1);
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
        for (auto& s : b->stmts) check_stmt(s.get());
        pop();
    }

    void Sema::check_stmt(Stmt* s) {
        if (!s) return;
        if (auto lv = dynamic_cast<LocalVar*>(s)) {
            check_local(lv);
        } else if (auto es = dynamic_cast<ExprStmt*>(s)) {
            check_expr(es->expr.get());
        } else if (auto as = dynamic_cast<AssignStmt*>(s)) {
            check_assign_stmt(as);
        } else if (auto is = dynamic_cast<IfStmt*>(s)) {
            check_cond(is->cond.get(), "if condition");
            check_block(is->then.get());
            if (is->els) {
                if (auto b = dynamic_cast<Block*>(is->els.get())) {
                    check_block(b);
                } else {
                    check_stmt(is->els.get());
                }
            }
        } else if (auto ws = dynamic_cast<WhileStmt*>(s)) {
            check_cond(ws->cond.get(), "while condition");
            ++loop_depth_;
            check_block(ws->body.get());
            --loop_depth_;
        } else if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            check_for(fs);
        } else if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            check_switch(sw);
        } else if (dynamic_cast<BreakStmt*>(s)) {
            if (loop_depth_ == 0) err("`break` can only appear in a loop", s);
        } else if (dynamic_cast<ContinueStmt*>(s)) {
            if (loop_depth_ == 0) err("`continue` can only appear in a loop", s);
        } else if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            check_return(rs);
        } else if (auto b = dynamic_cast<Block*>(s)) {
            check_block(b);
        } else {
            err("unknown statement", s);
        }
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
            // 同一声明语句的多个 auto 声明符必须推断出同一类型
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
                if (call->callee_kind == "struct_ctor") return;  // 就地构造
                if (!same_type(call->ty, ty)) {
                    err("struct `" + ty->str() +
                            "` has a constructor and must be built with `T(...)` or copy-initialized from an expression of the same type",
                        node);
                }
                return;  // 同类型返回值：拷贝初始化
            }
            if (ty->kind == TypeKind::Array && is_char(ty->elem) &&
                dynamic_cast<StringLit*>(e)) {
                check_expr(e, str_type());
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
