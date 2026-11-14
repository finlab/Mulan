// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds type resolution, scopes and unit checks (definitions of Sema members).
#include "sema_impl.hpp"

namespace muc_detail {

    TypePtr Sema::resolve_type(const TypeSpec* spec, Unit* u) {
        if (!spec) err("missing type");
        if (spec->resolved) {
            // A generic type argument already resolved at the use site (see TypeSpec::resolved).
            if (spec->is_const && !spec->resolved->is_const) {
                auto c = std::make_shared<Type>(*spec->resolved);
                c->is_const = true;
                return c;
            }
            return spec->resolved;
        }
        switch (spec->kind) {
            case TypeSpec::K::Named: {
                if (spec->is_self) {
                    if (!cur_class_ || cur_class_->kind != TypeKind::Interface) {
                        err_ts("`Self` is only allowed inside an interface", spec);
                    }
                    return cur_class_;
                }
                if (spec->name == "buffer") {
                    if (spec->args.size() != 1)
                        err_ts("`buffer<T>` takes exactly one type argument", spec);
                    TypePtr elem = resolve_type(spec->args[0].get(), u);
                    if (is_void(elem)) err_ts("buffer element cannot be void", spec);
                    if (noncopyable_struct(elem)) {
                        err_ts("struct `" + elem->str() +
                                   "` has a destructor and cannot be a buffer element",
                               spec);
                    }
                    return Type::make_slice(elem, spec->is_const, 1);
                }
                if (!spec->args.empty()) {
                    return instantiate_generic(spec, u);
                }
                TypePtr t = resolve_named_type(spec->name, u, spec);
                // C model: const on a prim needs its own type node (the prim factory has a singleton cache, so it cannot be mutated in place)
                if (spec->is_const && t && t->kind == TypeKind::Prim) {
                    auto c = std::make_shared<Type>(*t);
                    c->is_const = true;
                    return c;
                }
                return t;
            }
            case TypeSpec::K::Ptr: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_reference_type(elem)) {
                    err_ts("pointers to class or interface types are not supported (reference types are already handles)", spec);
                }
                return Type::make_ptr(elem, spec->is_const);
            }
            case TypeSpec::K::Slice: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_void(elem)) err_ts("dynamic array element cannot be void", spec);
                if (noncopyable_struct(elem)) {
                    err_ts("struct `" + elem->str() +
                               "` has a destructor and cannot be a dynamic array element",
                           spec);
                }
                return Type::make_slice(elem, spec->is_const, spec->rank);
            }
            case TypeSpec::K::Array: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_void(elem)) err_ts("array element cannot be void", spec);
                TypePtr base = elem;
                while (base && base->kind == TypeKind::Array) base = base->elem;
                if (noncopyable_struct(base)) {
                    err_ts("struct `" + base->str() +
                               "` has a destructor and cannot be an array element (array destruction is not implemented)",
                           spec);
                }
                return Type::make_array(elem, spec->size);
            }
            case TypeSpec::K::Func: {
                std::vector<TypePtr> ps;
                for (auto& p : spec->params) {
                    TypePtr pt = decay_param_type(resolve_type(p.get(), u));
                    if (is_void(pt)) err_ts("a function type parameter cannot be `void`", p.get());
                    if (is_borrow_type(pt)) {
                        err_ts("a function type cannot take a `ref` parameter (a borrow is not a value type)", p.get());
                    }
                    ps.push_back(pt);
                }
                TypePtr rt = resolve_type(spec->ret.get(), u);
                if (is_borrow_type(rt)) {
                    err_ts("a function type cannot return a borrow (`ref T`)", spec);
                }
                return Type::make_func(ps, rt);
            }
        }
        err_ts("unknown type " + spec_text(spec), spec);
    }

    TypePtr Sema::find_type_named(const std::string& unit, const std::string& name,
                            bool require_pub) {
        auto it = infos_.find(unit);
        if (it == infos_.end()) return nullptr;
        const UnitInfo& ui = it->second;
        auto ok = [&](bool is_pub) { return !require_pub || is_pub; };
        if (auto s = ui.structs.find(name); s != ui.structs.end()) {
            return ok(s->second->is_public) ? s->second->ty : nullptr;
        }
        if (auto e = ui.enums.find(name); e != ui.enums.end()) {
            return ok(e->second->is_public) ? e->second->ty : nullptr;
        }
        if (auto c = ui.classes.find(name); c != ui.classes.end()) {
            return ok(c->second->is_public) ? c->second->ty : nullptr;
        }
        if (auto i = ui.interfaces.find(name); i != ui.interfaces.end()) {
            return ok(i->second->is_public) ? i->second->ty : nullptr;
        }
        if (auto a = ui.aliases.find(name); a != ui.aliases.end()) {
            return ok(a->second->is_public) ? resolve_alias(a->second, ui.unit) : nullptr;
        }
        return nullptr;
    }

    TypePtr Sema::resolve_alias(TypeAliasDecl* a, Unit* owner) {
        if (a->resolved) return a->rty;
        if (a->resolving) {
            err("circular type alias definition involving `" + a->name + "`", a);
        }
        struct Guard {
            bool& flag;
            ~Guard() { flag = false; }
        } guard{a->resolving};
        a->resolving = true;
        a->rty = resolve_type(a->spec.get(), owner);
        a->resolved = true;
        return a->rty;
    }

    TypePtr Sema::resolve_named_type(const std::string& name, Unit* u, const TypeSpec* spec) {
        if (name == "void") return t_void();
        if (is_prim_name(name)) return Type::make_prim(name);

        // Multi-segment unit name: match starting from the longest prefix (`lib.a.b.T` → unit `lib.a.b` + type `T`)
        for (std::size_t pos = name.rfind('.'); pos != std::string::npos;) {
            const std::string un = name.substr(0, pos);
            const std::string tn = name.substr(pos + 1);
            auto it = infos_.find(un);
            if (it != infos_.end()) {
                if (un != u->name && !unit_imported(un)) {
                    err_ts("unit `" + un + "` is not imported with `use`", spec);
                }
                if (!unit_visible(un)) {
                    err_ts("unit `" + un + "` is only visible within its library; cross-library use requires `public unit`", spec);
                }
                UnitInfo& ui = it->second;
                auto need_pub = [&](bool is_pub) {
                    if (un != u->name && !is_pub) {
                        err_ts("type `" + un + "." + tn + "` is not declared public", spec);
                    }
                };
                if (auto s = ui.structs.find(tn); s != ui.structs.end()) {
                    need_pub(s->second->is_public);
                    return s->second->ty;
                }
                if (auto e = ui.enums.find(tn); e != ui.enums.end()) {
                    need_pub(e->second->is_public);
                    return e->second->ty;
                }
                if (auto c = ui.classes.find(tn); c != ui.classes.end()) {
                    need_pub(c->second->is_public);
                    return c->second->ty;
                }
                if (auto i = ui.interfaces.find(tn); i != ui.interfaces.end()) {
                    need_pub(i->second->is_public);
                    return i->second->ty;
                }
                if (auto a = ui.aliases.find(tn); a != ui.aliases.end()) {
                    need_pub(a->second->is_public);
                    return resolve_alias(a->second, ui.unit);
                }
                err_ts("unit `" + un + "` has no type `" + tn + "`", spec);
            }
            if (pos == 0) break;
            pos = name.rfind('.', pos - 1);
        }

        // The current unit takes precedence (including private types; local declarations shadow imports)
        if (TypePtr t = find_type_named(u->name, name, false)) return t;

        // Directly `use`d units: public types only; the same name from several sources → ambiguity error at the use site
        TypePtr found;
        std::string found_unit;
        for (auto& use : u->uses) {
            if (!unit_visible(use->unit_name)) continue;
            TypePtr t = find_type_named(use->unit_name, name, true);
            if (!t) continue;
            if (found) {
                err_ts("type `" + name + "` is ambiguous: `" + found_unit + "." + name + "` and `" +
                           use->unit_name + "." + name + "`; use a qualified name",
                       spec);
            }
            found = t;
            found_unit = use->unit_name;
        }
        if (found) return found;
        if (UnitInfo* stdn = std_info()) {
            if (auto s = stdn->structs.find(name); s != stdn->structs.end()) {
                if (!s->second->is_public)
                    err_ts("type `std.string." + name + "` is not declared public", spec);
                return s->second->ty;
            }
            if (auto e = stdn->enums.find(name); e != stdn->enums.end()) {
                if (!e->second->is_public)
                    err_ts("type `std.string." + name + "` is not declared public", spec);
                return e->second->ty;
            }
            if (auto c = stdn->classes.find(name); c != stdn->classes.end()) {
                if (!c->second->is_public)
                    err_ts("type `std.string." + name + "` is not declared public", spec);
                return c->second->ty;
            }
            if (auto i = stdn->interfaces.find(name); i != stdn->interfaces.end()) {
                if (!i->second->is_public)
                    err_ts("type `std.string." + name + "` is not declared public", spec);
                return i->second->ty;
            }
            if (auto a = stdn->aliases.find(name); a != stdn->aliases.end()) {
                if (!a->second->is_public)
                    err_ts("type `std.string." + name + "` is not declared public", spec);
                return resolve_alias(a->second, stdn->unit);
            }
        }
        // Exception is an implicit dependency as well (the root of every throwable class).
        if (UnitInfo* ex = exception_info()) {
            if (auto c = ex->classes.find(name); c != ex->classes.end()) {
                if (!c->second->is_public)
                    err_ts("type `std.exception." + name + "` is not declared public", spec);
                return c->second->ty;
            }
        }
        err_ts("unknown type `" + name + "`", spec);
    }

    TypePtr Sema::find_type_in_expr(const std::string& name, const Node* at) {
        if (TypePtr t = find_type_named(cur_->name, name, false)) return t;
        TypePtr found;
        std::string found_unit;
        for (auto& use : cur_->uses) {
            if (!unit_visible(use->unit_name)) continue;
            TypePtr t = find_type_named(use->unit_name, name, true);
            if (!t) continue;
            if (found && !same_type(found, t)) {
                err("type `" + name + "` is ambiguous: `" + found_unit + "." + name + "` and `" +
                        use->unit_name + "." + name + "`; use a qualified name",
                    at);
            }
            if (!found) {
                found = t;
                found_unit = use->unit_name;
            }
        }
        if (found) return found;
        if (UnitInfo* stdn = std_info()) {
            if (auto e = stdn->enums.find(name); e != stdn->enums.end() && e->second->is_public) {
                return e->second->ty;
            }
        }
        return nullptr;
    }

    EnumConst* Sema::find_enum_member(const TypePtr& et, const std::string& member) {
        if (!et || et->kind != TypeKind::Enum) return nullptr;
        auto it = infos_.find(et->unit);
        if (it == infos_.end() || !it->second.unit) return nullptr;
        for (auto& d : it->second.unit->decls) {
            auto ed = dynamic_cast<EnumDecl*>(d.get());
            if (!ed || ed->name != et->name) continue;
            long long value = 0;
            bool has = false;
            for (auto& [m, v] : ed->members) {
                if (v.has_value()) {
                    value = *v;
                    has = true;
                } else if (!has) {
                    value = 0;
                    has = true;
                } else {
                    value += 1;
                }
                if (m == member) {
                    return intern_enum_const(it->second.unit->name, ed->name, m, value,
                                             ed->ty ? ed->ty : et);
                }
            }
        }
        return nullptr;
    }

    ConstMember* Sema::find_const_member_in(const TypePtr& t, const std::string& name) {
        TypePtr cur = t;
        int guard = 0;
        while (cur && (cur->kind == TypeKind::Struct || cur->kind == TypeKind::Class) &&
               guard++ < 1024) {
            auto it = infos_.find(cur->unit);
            if (it == infos_.end() || !it->second.unit) return nullptr;
            std::vector<std::unique_ptr<ConstMember>>* list = nullptr;
            for (auto& d : it->second.unit->decls) {
                if (cur->kind == TypeKind::Struct) {
                    auto sd = dynamic_cast<StructDecl*>(d.get());
                    if (sd && sd->name == cur->name) {
                        list = &sd->const_members;
                        break;
                    }
                } else {
                    auto cd = dynamic_cast<ClassDecl*>(d.get());
                    if (cd && cd->name == cur->name) {
                        list = &cd->const_members;
                        break;
                    }
                }
            }
            if (!list) return nullptr;
            for (auto& cm : *list) {
                if (cm->name == name) return cm.get();
            }
            if (cur->kind != TypeKind::Class) return nullptr;  // a struct has no base class
            cur = cur->base;
        }
        return nullptr;
    }

    bool Sema::const_member_visible(const ConstMember* cm) const {
        if (!cm) return false;
        if (cm->is_public) return true;
        return cur_ && cm->owner && cur_->name == cm->owner->unit;
    }

    bool Sema::has_any_const_member(const TypePtr& t) {
        TypePtr cur = t;
        int guard = 0;
        while (cur && (cur->kind == TypeKind::Struct || cur->kind == TypeKind::Class) &&
               guard++ < 1024) {
            auto it = infos_.find(cur->unit);
            if (it == infos_.end() || !it->second.unit) return false;
            bool found = false;
            size_t count = 0;
            for (auto& d : it->second.unit->decls) {
                if (cur->kind == TypeKind::Struct) {
                    auto sd = dynamic_cast<StructDecl*>(d.get());
                    if (sd && sd->name == cur->name) {
                        found = true;
                        count = sd->const_members.size();
                        break;
                    }
                } else {
                    auto cd = dynamic_cast<ClassDecl*>(d.get());
                    if (cd && cd->name == cur->name) {
                        found = true;
                        count = cd->const_members.size();
                        break;
                    }
                }
            }
            if (!found) return false;
            if (count > 0) return true;
            if (cur->kind != TypeKind::Class) return false;
            cur = cur->base;
        }
        return false;
    }

    void Sema::push() { scopes_.emplace_back(); }

    void Sema::pop() {
        if (!scopes_.empty()) scopes_.pop_back();
    }

    LocalVar* Sema::lookup_local(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }

    void Sema::declare_local(LocalVar* lv) {
        if (scopes_.empty()) scopes_.emplace_back();
        Scope& sc = scopes_.back();
        if (sc.count(lv->name)) err("duplicate declaration of `" + lv->name + "`", lv);
        sc[lv->name] = lv;
        if (!para_stack_.empty()) para_stack_.back().declared.insert(lv);
    }

    EnumConst* Sema::lookup_enum_const_in(const std::string& unit_name, const std::string& name,
                                    bool require_pub) {
        auto it = infos_.find(unit_name);
        if (it == infos_.end()) return nullptr;
        // Iterate in declaration order (Python's dict preserves insertion order)
        for (auto& d : it->second.unit->decls) {
            auto ed = dynamic_cast<EnumDecl*>(d.get());
            if (!ed) continue;
            if (require_pub && !ed->is_public) continue;
            long long value = 0;
            bool has = false;
            for (auto& [m, v] : ed->members) {
                if (v.has_value()) {
                    value = *v;
                    has = true;
                } else if (!has) {
                    value = 0;
                    has = true;
                } else {
                    value += 1;
                }
                if (m == name) return intern_enum_const(unit_name, ed->name, m, value, ed->ty);
            }
        }
        return nullptr;
    }

    EnumConst* Sema::lookup_enum_const(const std::string& name) {
        return lookup_enum_const_in(cur_->name, name, false);
    }

    void Sema::check_unit(Unit* u) {
        cur_ = u;
        unit_globals_so_far_.clear();
        for (auto& d : u->decls) {
            auto* g = dynamic_cast<GlobalVar*>(d.get());
            if (!g) continue;
            cur_global_ = g;
            recover([&] { check_global(g); });
            cur_global_ = nullptr;
            unit_globals_so_far_.push_back(g);
        }
        // Signatures of same-name functions must not be fully identical (overloads must differ in parameters)
        for (auto& entry : infos_.at(u->name).funcs) {
            auto& vec = entry.second;
            if (entry.first == "main" && vec.size() > 1) {
                err("`main` cannot be overloaded (at most one `main` per unit)", vec[1]);
            }
            if (vec.size() > 1) {
                bool hasExtern = false, hasBody = false;
                for (auto* f : vec) {
                    if (f->is_extern) hasExtern = true;
                    else hasBody = true;
                }
                if (hasExtern && hasBody) {
                    err("function `" + entry.first + "` cannot be declared both `extern` and normal", vec[1]);
                }
                if (hasExtern) err("`extern` function `" + entry.first + "` cannot be overloaded", vec[1]);
            }
            for (size_t i = 0; i < vec.size(); ++i) {
                for (size_t j = i + 1; j < vec.size(); ++j) {
                    bool same = vec[i]->pty.size() == vec[j]->pty.size();
                    if (same) {
                        for (size_t k = 0; k < vec[i]->pty.size(); ++k) {
                            if (!same_type(vec[i]->pty[k], vec[j]->pty[k])) {
                                same = false;
                                break;
                            }
                        }
                    }
                    if (same) err("duplicate signature of function `" + entry.first + "`", vec[j]);
                }
            }
        }
        for (auto& d : u->decls) {
            auto f = dynamic_cast<FuncDecl*>(d.get());
            if (!f || f->is_extern) continue;
            recover([&] { check_func(f); });
        }
        for (auto& d : u->decls) {
            if (auto c = dynamic_cast<ClassDecl*>(d.get())) recover([&] { check_class(c); });
        }
        for (auto& d : u->decls) {
            if (auto sd = dynamic_cast<StructDecl*>(d.get())) recover([&] { check_struct(sd); });
        }
        for (auto& d : u->decls) {
            if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) recover([&] { check_interface(i); });
        }
    }

    void Sema::check_global(GlobalVar* g) {
        if (!g->rty) {
            err("global variable `" + g->name + "` is missing its type", g);
            return;
        }
        if (g->is_extern) {
            if (g->init) err("`extern` variable `" + g->name + "` cannot have an initializer", g);
            return;
        }
        if (!g->init) {
            if (g->is_const) {
                err("`const` variable `" + g->name + "` must be initialized", g);
                return;
            }
            // A struct with a parameterless constructor is constructed by the unit init sequence,
            // exactly like a local `S s;`; everything else stays zero-initialized in the data area.
            if (g->rty->kind == TypeKind::Struct && !g->rty->ctors.empty()) {
                bool has_default = false;
                for (auto& cs : g->rty->ctors) {
                    if (cs.empty()) {
                        has_default = true;
                        break;
                    }
                }
                if (!has_default) {
                    err("struct `" + g->rty->str() +
                            "` has no parameterless constructor and must be initialized with a constructor",
                        g);
                    return;
                }
                g->runtime_init = true;
                return;
            }
            g->zero = true;
            return;
        }
        // Try the new compile-time evaluator first (applies to both const and non-const globals; the result is cached for the backend);
        // fall back to the old is_const_expr when it cannot compute a value (aggregate initialization / string literals, etc.)
        {
            ConstVal v;
            if (eval_global_const(g, cur_->name, v)) return;
        }
        // A data-area constant can only express what `constValue` knows. Reference-holding types need
        // runtime work (RC / constructor calls) even when the expression itself folds, e.g.
        // `String g = "hi";`.
        bool zero_init = false;
        if (auto ia = dynamic_cast<InitAggregate*>(g->init.get())) {
            zero_init = ia->items.empty();
        } else if (auto ie = dynamic_cast<InitExpr*>(g->init.get())) {
            if (auto il = dynamic_cast<IntLit*>(ie->expr.get())) {
                zero_init = il->value == 0;
            } else if (dynamic_cast<NullLit*>(ie->expr.get())) {
                zero_init = true;
            }
        }
        const bool needs_runtime =
            !zero_init && (g->rty->needs_cleanup() ||
                           (g->rty->kind == TypeKind::Struct && !g->rty->ctors.empty()));
        if (g->is_const && g->rty->needs_cleanup()) {
            err("`const` unit-level variable `" + g->name +
                    "` cannot hold a type that needs runtime cleanup (unit-level `const` must be a "
                    "compile-time constant)",
                g);
        }
        const bool foldable = is_const_expr(g->init.get());
        bool func_init = false;
        if (!foldable && g->rty->kind == TypeKind::Func) {
            if (auto ie = dynamic_cast<InitExpr*>(g->init.get())) {
                // A function value is an address constant (3.16): it may initialize a unit-level variable.
                TypePtr t = check_expr(ie->expr.get(), g->rty);
                if (t && t->kind == TypeKind::Func) {
                    FuncDecl* picked = nullptr;
                    if (auto nm = dynamic_cast<Name*>(ie->expr.get())) {
                        if (auto* f = std::get_if<FuncDecl*>(&nm->sym)) picked = *f;
                    } else if (auto mb = dynamic_cast<Member*>(ie->expr.get())) {
                        if (auto* f = std::get_if<FuncDecl*>(&mb->sym)) picked = *f;
                    }
                    if (!picked) {
                        err("initializer of unit-level variable `" + g->name +
                                "` must be a named function (a lambda body cannot be used here)",
                            g);
                    }
                    g->const_func = picked;
                    func_init = true;
                }
            }
        }
        if (!func_init && (!foldable || needs_runtime)) {
            if (g->is_const) {
                if (!g->rty->needs_cleanup()) {
                    err("initializer of `const` unit-level variable `" + g->name +
                            "` must be a compile-time constant",
                        g);
                }
            } else {
                // 9.1: a runtime initializer; the unit init sequence evaluates it before `main`.
                g->runtime_init = true;
            }
        }
        check_decl_init(g->zero, g->rty, g->init.get(), "global initialization", g);
    }

    void Sema::check_global_init_order(GlobalVar* g, Expr* at) {
        if (!cur_global_ || !g) return;
        // Compile-time constants and `extern` storage have no runtime position in the sequence.
        if (g->is_const || g->is_extern) return;
        if (g == cur_global_) {
            err("initializer of unit-level variable `" + g->name + "` cannot reference itself", at);
            return;
        }
        for (auto* earlier : unit_globals_so_far_) {
            if (earlier == g) return;
        }
        err("initializer of unit-level variable `" + cur_global_->name + "` cannot reference `" +
                g->name + "`, which is declared later (it is not initialized yet)",
            at);
    }

    bool Sema::guarantees_return(Stmt* s) {
        if (!s) return false;
        if (dynamic_cast<ReturnStmt*>(s)) return true;
        // `throw` never falls through: it is a terminating path for the return-value check.
        if (dynamic_cast<ThrowStmt*>(s)) return true;
        if (auto b = dynamic_cast<Block*>(s)) {
            if (b->stmts.empty()) return false;
            return guarantees_return(b->stmts.back().get());
        }
        if (auto i = dynamic_cast<IfStmt*>(s)) {
            if (!i->els) return false;
            return guarantees_return(i->then.get()) && guarantees_return(i->els.get());
        }
        // A try guarantees a return when its body and every catch body do; an unmatched exception
        // propagates instead of falling through, so it also counts as a terminating path.
        if (auto tr = dynamic_cast<TryStmt*>(s)) {
            if (!guarantees_return(tr->body.get())) return false;
            for (auto& c : tr->catches) {
                if (!guarantees_return(c->body.get())) return false;
            }
            return true;
        }
        // When the value domain is fully covered, a switch guarantees a return only if all of its arms return
        if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            if (!sw->has_else && !sw->enum_exhaustive) return false;
            if (sw->arms.empty()) return false;
            for (auto& a : sw->arms) {
                if (!guarantees_return(a->body.get())) return false;
            }
            return true;
        }
        return false;
    }

    void Sema::check_operator_decl(FuncDecl* f) {
        const std::string sym = f->name.substr(8);
        const size_t n = f->pty.size();
        const bool unary_or_binary = (sym == "-" || sym == "+");
        if (sym == "~") {
            if (n != 1) err("`operator~` takes 1 parameter", f);
        } else if (sym == "[]") {
            if (n < 2 || n > 4)
                err("`operator[]` takes 2 to 4 parameters (the container plus 1 to 3 indices)", f);
            else {
                for (size_t i = 1; i < n; ++i) {
                    if (!is_integer(f->pty[i]))
                        err("index " + std::to_string(i) + " of `operator[]` must be an integer", f);
                }
            }
        } else if (sym == "[]=") {
            err("`operator[]=` was removed: return `ref T` from `operator[]` so that `c[i] = v` writes through the borrow",
                f);
        } else if (unary_or_binary) {
            if (n != 1 && n != 2) err("`operator" + sym + "` takes 1 or 2 parameters", f);
        } else {
            static const char* allowed[] = {"*", "/", "%", "<", "<=", ">", ">=", "==",
                                            "!=", "&", "|", "^", "<<", ">>"};
            bool ok = false;
            for (auto* a : allowed) {
                if (sym == a) { ok = true; break; }
            }
            if (!ok) err("operator `" + sym + "` cannot be overloaded", f);
            if (n != 2) err("binary operators take 2 parameters", f);
        }

        bool has_user = false;
        for (auto& p : f->pty) {
            if (p->kind == TypeKind::Struct || p->kind == TypeKind::Class) has_user = true;
            if (p->kind == TypeKind::Void) err("operator parameter cannot be void", f);
        }
        if (!has_user) err("operators require at least one struct or class operand", f);

        if (sym == "==" || sym == "!=") {
            for (auto& p : f->pty) {
                if (p->kind == TypeKind::Class || p->kind == TypeKind::Interface) {
                    err("`" + sym + "` on class or interface types is always identity comparison and cannot be overloaded", f);
                }
            }
        }

        if (sym == "<" || sym == "<=" || sym == ">" || sym == ">=" || sym == "==" || sym == "!=") {
            if (!(f->rty && f->rty->kind == TypeKind::Prim && f->rty->name == "bool")) {
                err("comparison operators must return `bool`", f);
            }
        }

        if (sym == "[]" || sym == "[]=") {
            if (!(f->pty[0]->kind == TypeKind::Struct || f->pty[0]->kind == TypeKind::Class)) {
                err("first parameter of `operator" + sym + "` must be a struct or class", f);
            }
            if (!is_integer(f->pty[1])) err("index of `operator[]` must be an integer", f);
        }
    }

    void Sema::check_member_operator_sig(MethodDecl* m, const TypePtr& owner) {
        const std::string sym = m->name.substr(8);
        const size_t n = m->pty.size();
        auto bad = [&](const std::string& msg) { err("struct `" + owner->name + "`: " + msg, m); };
        if (sym == "[]") {
            if (n < 1 || n > 3) {
                bad("`operator[]` takes 1 to 3 parameters (the indices); the receiver is `this`");
            } else {
                for (size_t i = 0; i < n; ++i) {
                    if (!is_integer(m->pty[i])) bad("index of `operator[]` must be an integer");
                }
            }
        } else if (sym == "[]=") {
            bad("`operator[]=` was removed: return `ref T` from `operator[]` so that `c[i] = v` writes through the borrow");
        } else if (sym == "~") {
            if (n != 0) bad("`operator~` takes no parameter (the receiver is `this`)");
        } else if (sym == "-" || sym == "+") {
            if (n != 0 && n != 1) {
                bad("`operator" + sym + "` takes 0 (unary) or 1 (binary) parameter; the receiver is `this`");
            }
        } else {
            static const char* allowed[] = {"*", "/", "%", "<",  "<=", ">",  ">=",
                                            "==", "!=", "&", "|", "^",  "<<", ">>"};
            bool ok = false;
            for (auto* a : allowed) {
                if (sym == a) {
                    ok = true;
                    break;
                }
            }
            if (!ok) bad("operator `" + sym + "` cannot be overloaded");
            if (n != 1) bad("`operator" + sym + "` takes 1 parameter; the receiver is `this`");
        }
        if ((sym == "==" || sym == "!=" || sym == "<" || sym == "<=" || sym == ">" || sym == ">=") &&
            !(m->rty && m->rty->kind == TypeKind::Prim && m->rty->name == "bool")) {
            bad("comparison operators must return `bool`");
        }
        for (auto& p : m->pty) {
            if (is_void(p)) bad("operator parameter cannot be void");
        }
    }

    void Sema::check_func(FuncDecl* f) {
        if (!f->rty) err("function `" + f->name + "` is missing its return type", f);
        if (f->is_operator) check_operator_decl(f);
        cur_func_ = f;
        cur_ret_ = f->rty;
        cur_name_ = f->name;
        cur_class_ = nullptr;
        in_ctor_ = false;
        in_dtor_ = false;
        in_finally_ = false;
        scopes_.clear();
        auto_groups_.clear();
        scopes_.emplace_back();
        loop_depth_ = 0;
        f->param_locals.clear();

        for (size_t i = 0; i < f->params.size() && i < f->pty.size(); ++i) {
            Param* p = f->params[i].get();
            auto lv = std::make_unique<LocalVar>();
            lv->name = p->name;
            lv->rty = f->pty[i];
            lv->is_const = p->is_const;
            lv->by_ref = p->by_ref;
            lv->is_ref_param = p->by_ref;
            lv->line = p->line;
            lv->col = p->col;
            declare_local(lv.get());
            f->param_locals.push_back(std::move(lv));
        }

        if (f->body) check_block(f->body.get());

        // A non-void function must guarantee a return value; main is exempt (falling off the end is equivalent to return 0)
        if (!is_void(f->rty) && !f->is_entry && !guarantees_return(f->body.get())) {
            err("function `" + f->name + "` may reach the end of its body without returning a value", f);
        }

        scopes_.clear();
        auto_groups_.clear();
        cur_func_ = nullptr;
        cur_ret_ = nullptr;
        cur_name_.clear();
        this_local_.clear();
    }
}  // namespace muc_detail
