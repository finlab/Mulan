// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds type resolution, scopes and unit checks (definitions of Sema members).
//
// owns:      type resolution, scopes, unit-level checks and function signature checks; also the check_unit pass that drives body checking
// not:       expression/statement checks, call resolution, codegen
// pass:      pass 12 (check_unit per unit), and type resolution is used everywhere
// concept:   type-resolution, scopes, unit-checks, pass-order
#include "sema_impl.hpp"

namespace muc_detail {

    TypePtr Sema::resolve_type(const TypeSpec* spec, Unit* u) {
        if (!spec) err("missing type");
        if (spec->resolved) {
            // A generic type argument already resolved at the use site (see TypeSpec::resolved).
            if (spec->is_const && !spec->resolved->is_const) {
                auto c = std::make_shared<Type>(*spec->resolved);
                c->is_const = true;
                auto& slot = cv_views_[spec->resolved.get()];
                slot.first = spec->resolved;
                slot.second.push_back(c);
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
                    err_ts("`buffer<T>` is no longer a type: write `T[]` (the built-in fixed-size heap block)", spec);
                }
                if (!spec->args.empty()) {
                    return instantiate_generic(spec, u);
                }
                // A constrained generic body is checked at its definition with each type parameter
                // bound to a witness type (build 156); the binding shadows normal lookup.
                if (!type_bind_.empty()) {
                    auto bound = type_bind_.find(spec->name);
                    if (bound != type_bind_.end()) return bound->second;
                }
                TypePtr t = resolve_named_type(spec->name, u, spec);
                // cv belongs to the type (decided 2026-11-17): a const named type needs its own node.
                // The factories cache prims/singletons, so the flag is applied to a copy.
                if (spec->is_const && t && !t->is_const) {
                    if (t->kind == TypeKind::Module) err_ts("`const` cannot qualify a unit name", spec);
                    auto c = std::make_shared<Type>(*t);
                    c->is_const = true;
                    auto& slot = cv_views_[t.get()];
                    slot.first = t;
                    slot.second.push_back(c);
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
                return Type::make_slice(elem, spec->is_const, spec->rank);
            }
            case TypeSpec::K::Array: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_void(elem)) err_ts("array element cannot be void", spec);
                // A struct with a destructor is a legal element: the element constructor and destructor are
                // recursive (agents/value-semantics.md §5, §6), and copying is what stays banned while the
                // element type is uncopyable.
                size_t n = spec->size;
                if (spec->len) {
                    // A compile-time constant length (`int[SIZE]`): the same evaluator that feeds `step`,
                    // `align(N)` and switch patterns (SPEC 3.3.2). `sizeof` is not part of it — bind it
                    // first (`const int N = sizeof(T) as int; int[N] a;`).
                    long long v = 0;
                    if (!eval_const_int(spec->len.get(), v)) {
                        err_ts("array length must be a compile-time integer constant "
                               "(`const int N = ...; int[N] a;`)",
                               spec);
                    }
                    if (v <= 0) {
                        err_ts("array length must be greater than 0 (got " + std::to_string(v) + ")", spec);
                    }
                    n = static_cast<size_t>(v);
                }
                return Type::make_array(elem, n);
            }
            case TypeSpec::K::Func:
            case TypeSpec::K::Delegate: {
                const bool is_delegate = spec->kind == TypeSpec::K::Delegate;
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
                return is_delegate ? Type::make_delegate(ps, rt) : Type::make_func(ps, rt);
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
        if (auto s = ui.sums.find(name); s != ui.sums.end()) {
            return ok(s->second->is_public) ? s->second->ty : nullptr;
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
                if (auto sm = ui.sums.find(tn); sm != ui.sums.end()) {
                    need_pub(sm->second->is_public);
                    return sm->second->ty;
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
        // Remember which loop nesting level the declaration belongs to: `move` may not outlive the loop it
        // appears in (agents/value-semantics.md §8.4).
        local_loop_level_[lv] = move_loop_count_;
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
        // An enum is backed by `int` (types.cpp) and its members share the unit's unqualified
        // namespace. A value outside the `int` range would be truncated and a repeated member name
        // would silently bind to the first declaration, so both are rejected here (#36 / #37).
        {
            // member name -> declaring enum. A map keeps the check O(1) per member: a linear scan over
            // every member seen so far made a large enum quadratic (a 40k-member enum took 0.66s, 200k
            // never finished).
            std::unordered_map<std::string, std::string> owner;
            for (auto& d : u->decls) {
                auto* ed = dynamic_cast<EnumDecl*>(d.get());
                if (!ed) continue;
                if (ed->members.size() > kMaxEnumMembers) {
                    err("enum `" + ed->name + "` has more than " + std::to_string(kMaxEnumMembers) +
                            " members; split it into several enums",
                        ed);
                }
                long long value = 0;
                bool has = false;
                for (auto& [m, v] : ed->members) {
                    if (v.has_value()) {
                        value = *v;
                        has = true;
                    } else if (has) {
                        value += 1;
                    } else {
                        value = 0;
                        has = true;
                    }
                    if (value < -2147483648LL || value > 2147483647LL) {
                        err("value of enum member `" + m + "` (" + std::to_string(value) +
                                ") does not fit the underlying `int` range", ed);
                    }
                    if (auto it = owner.find(m); it != owner.end()) {
                        err("duplicate enum member `" + m + "` (declared in `" + it->second +
                                "` and `" + ed->name + "`); write `" + ed->name + "." + m + "`",
                            ed);
                    }
                    owner.emplace(m, ed->name);
                }
            }
        }
        for (auto& d : u->decls) {
            auto* g = dynamic_cast<GlobalVar*>(d.get());
            if (!g) continue;
            cur_global_ = g;
            recover([&] { check_global(g); });
            cur_global_ = nullptr;
            unit_globals_so_far_.push_back(g);
        }
        // Signatures of same-name functions must not be fully identical (overloads must differ in
        // parameters). A top-level `const` on a parameter is not part of the signature (SPEC 3.7):
        // `f(int)` and `f(const int)` are the same function, so the second declaration is a duplicate.
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
                    if (same_param_sig(vec[i]->pty, vec[j]->pty)) {
                        err("duplicate signature of function `" + entry.first + "`", vec[j]);
                    }
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

namespace {
// The compiler's type for a builtin constant (`std.comptime` is the declaration of record, SPEC 9.3).
std::string builtin_const_type_name(const std::string& name) {
    if (name == "os") return "OS";
    if (name == "arch") return "Arch";
    if (name == "abi") return "Abi";
    if (name == "endian") return "Endian";
    if (name == "ptr_bits") return "int";
    return "bool";  // debug / bounds / null_check / single_thread
}
}  // namespace

    // SPEC 9.3: a builtin name may be declared once across the whole program, and the declaration must
    // match the compiler's table (name, type/signature).
    void Sema::check_builtin_owner(const std::string& name, const Node* at) {
        auto it = builtin_owner_.find(name);
        if (it == builtin_owner_.end()) {
            builtin_owner_.emplace(name, cur_ ? cur_->name : std::string());
            return;
        }
        // A duplicate inside one unit already has its own declaration diagnostic.
        if (cur_ && it->second == cur_->name) return;
        err("builtin `" + name + "` is already declared in `" + it->second +
                "`; a builtin name may be declared once across the whole program",
            at);
    }

    void Sema::check_builtin_func(FuncDecl* f) {
        long long v = 0;
        if (!builtin_func(f->name, {}, v)) {
            err("builtin function `" + f->name + "` is not provided by this compiler", f);
        }
        check_builtin_owner(f->name, f);
        if (!f->pty.empty()) err("builtin function `" + f->name + "` takes no parameters", f);
        if (!is_prim(f->rty) || f->rty->name != "bool") {
            err("builtin function `" + f->name + "` returns `bool`, not `" +
                    (f->rty ? f->rty->str() : std::string("?")) + "`",
                f);
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
        if (g->is_builtin) {
            // A compiler-provided compile-time constant: no storage, no initializer (9.3).
            long long v = 0;
            if (!builtin_const(g->name, v)) {
                err("builtin `" + g->name + "` is not provided by this compiler", g);
                return;
            }
            check_builtin_owner(g->name, g);
            if (g->init) err("a `builtin` constant cannot have an initializer", g);
            // SPEC 9.3: the declared type must be the type the compiler provides for this name.
            const std::string want = builtin_const_type_name(g->name);
            const TypePtr& ty = g->rty;
            const bool ok = want == "int"    ? (is_prim(ty) && ty->name == "int")
                            : want == "bool" ? (is_prim(ty) && ty->name == "bool")
                                             : (ty && ty->kind == TypeKind::Enum && ty->name == want);
            if (!ok) {
                err("builtin `" + g->name + "` is declared `" + (ty ? ty->str() : std::string("?")) +
                        "` but the compiler provides `" + want + "`",
                    g);
            }
            g->const_evaluated = true;
            g->const_is_float = false;
            g->const_ival = v;
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
        bool const_eval_ok = false;
        {
            ConstVal v;
            const_eval_ok = eval_global_const(g, cur_->name, v);
            if (const_eval_ok) return;
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
            // A syntactically foldable initializer that the evaluator rejected is an invalid constant
            // expression (division by zero / signed overflow), not a runtime initializer.
            if (g->is_const && foldable && !needs_runtime && !const_eval_ok) {
                err("initializer of `const` unit-level variable `" + g->name +
                        "` is not a valid compile-time constant (division by zero or signed overflow)",
                    g);
            }
            // 9.1: a runtime initializer; the unit init sequence evaluates it before `main`. A `const`
            // object may be runtime-initialized: it is still read-only in the language, but its
            // initializer is not a compile-time constant (3.7.2).
            g->runtime_init = true;
        }
        check_decl_init(g->zero, g->rty, g->init.get(), "global initialization", g);
    }

    void Sema::check_global_init_order(GlobalVar* g, Expr* at) {
        if (!cur_global_ || !g) return;
        // Compile-time constants and `extern` storage have no runtime position in the sequence.
        // A runtime-initialized `const` does have one (9.1).
        if ((g->is_const && !g->runtime_init) || g->is_extern) return;
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
            // an enum or sum scrutinee is exhaustive by construction (check_switch rejects a missing
            // alternative/member without `else`)
            if (!sw->has_else && !sw->enum_exhaustive && !sw->is_sum) return false;
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

        // A `ref` return is a borrow of the receiver (chained calls pass it on). That only makes
        // sense for a binary operator; unary and `[]=` forms have no receiver to hand out.
        const bool unary_form = sym == "~" || (unary_or_binary && n == 1);
        if (f->ret_ref && (unary_form || sym == "[]=")) {
            err("a unary operator cannot return a borrow (`ref`); return the value", f);
        }

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
        if (m->ret_ref && (sym == "~" || ((sym == "-" || sym == "+") && m->pty.empty()))) {
            bad("a unary operator cannot return a borrow (`ref`); return the value");
        }
        for (auto& p : m->pty) {
            if (is_void(p)) bad("operator parameter cannot be void");
        }
    }

    void Sema::check_func(FuncDecl* f) {
        if (!f->rty) err("function `" + f->name + "` is missing its return type", f);
        if (f->is_builtin) check_builtin_func(f);
        if (f->is_operator) check_operator_decl(f);
        // A function body is checked in a clean control-flow context. Generic bodies are checked
        // lazily at the instantiation site (sema_generic.cpp), which may sit inside a `para` section
        // or a lambda; without this save/clear/restore every `return` in the body would be judged
        // against the caller's para/lambda context ("return cannot leave a `para` section").
        auto saved_para = std::move(para_stack_);
        auto saved_lambda = std::move(lambda_stack_);
        const bool saved_pf_body = in_para_for_body_;
        const int saved_pf_depth = para_for_body_depth_;
        para_stack_.clear();
        lambda_stack_.clear();
        in_para_for_body_ = false;
        para_for_body_depth_ = 0;
        cur_func_ = f;
        cur_ret_ = f->rty;
        cur_name_ = f->name;
        cur_class_ = nullptr;
        in_ctor_ = false;
        in_dtor_ = false;
        in_finally_ = false;
        scopes_.clear();
        auto_groups_.clear();
        begin_move_scope();
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

        if (f->body && !f->is_builtin) {
            check_block(f->body.get());
            // A non-void function must guarantee a return value; main is exempt (falling off the end is equivalent to return 0)
            if (!is_void(f->rty) && !f->is_entry && !guarantees_return(f->body.get())) {
                err("function `" + f->name + "` may reach the end of its body without returning a value", f);
            }
        }

        scopes_.clear();
        auto_groups_.clear();
        cur_func_ = nullptr;
        cur_ret_ = nullptr;
        cur_name_.clear();
        this_local_.clear();
        para_stack_ = std::move(saved_para);
        lambda_stack_ = std::move(saved_lambda);
        in_para_for_body_ = saved_pf_body;
        para_for_body_depth_ = saved_pf_depth;
    }
}  // namespace muc_detail
