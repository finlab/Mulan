// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds call checking and miscellany (definitions of Sema members).
#include "sema_impl.hpp"

namespace muc_detail {

    TypePtr Sema::check_fnptr_args(Call* c, const TypePtr& ft) {
        if (!c->type_args.empty()) err("a call through a function value cannot take type arguments", c);
        if (!c->named.empty()) {
            err("a call through a function value does not support named arguments", c);
        }
        if (c->args.size() != ft->fparams.size()) {
            err("this function value expects " + std::to_string(ft->fparams.size()) + " arguments, but " +
                    std::to_string(c->args.size()) + " were given",
                c);
        }
        for (size_t i = 0; i < ft->fparams.size() && i < c->args.size(); ++i) {
            check_assign(c->args[i].get(), ft->fparams[i], "argument of a call through a function value");
        }
        c->callee_kind = ft->kind == TypeKind::Delegate ? "delegate" : "fnptr";
        c->fn_ty = ft;
        return ft->fret;
    }

    TypePtr Sema::check_call(Call* c) {
        Expr* callee = c->callee.get();
        if (!callee) err("call is missing a callee", c);

        if (auto nm = dynamic_cast<Name*>(callee)) {
            if (nm->ident == "alloc" || nm->ident == "realloc" || nm->ident == "free") {
                return check_builtin(c, nm->ident);
            }
            if (nm->ident == "super") return check_super_call(c);
            // Unqualified call to one of the enclosing class's own methods
            if (lookup_local(nm->ident) == nullptr && cur_class_) {
                std::vector<const MethodSig*> ms;
                collect_methods(cur_class_, nm->ident, ms);
                if (!ms.empty()) {
                    auto sig = choose_method(ms, raw_args(c->args), "method `" + nm->ident + "`", c);
                    check_method_args(c, sig, "method `" + nm->ident + "`");
                    c->callee_kind =
                        cur_class_->kind == TypeKind::Struct ? "struct_method" : "method";
                    c->method = sig;
                    c->by_ref = is_borrow_type(sig->ret);
                    c->ref_const = c->by_ref && sig->ret->is_const;
                    return referent_of(sig->ret);
                }
            }
        }

        FuncDecl* target = nullptr;
        TypePtr struct_ty = nullptr;

        if (auto nm = dynamic_cast<Name*>(callee)) {
            if (lookup_local(nm->ident) != nullptr) {
                TypePtr vt = check_expr(callee);
                if (vt && is_callable_value(vt)) return check_fnptr_args(c, vt);
                err("`" + nm->ident + "` is a variable, not a function", c);
            }
            // Candidates: current unit + directly `use`d units (public only) + std.string (implicit)
            if (!c->type_args.empty()) {
                // Generic function takes precedence; otherwise it is treated as value construction of a generic struct
                Decl* tdecl = find_generic_template(nm->ident, cur_);
                if (dynamic_cast<FuncDecl*>(tdecl)) {
                    target = instantiate_func(c, nm->ident, cur_);
                    nm->kind = "func";
                    nm->sym = target;
                } else {
                    TypePtr sty = instantiate_generic_named(nm->ident, c->type_args, cur_);
                    if (!sty || sty->kind != TypeKind::Struct) {
                        err("`" + nm->ident + "<...>` is not a generic function or struct", c);
                    }
                    struct_ty = sty;
                    nm->kind = "type";
                }
            } else {
            std::vector<FuncDecl*> cands;
            auto add_funcs = [&](UnitInfo& o, bool require_pub) {
                auto f = o.funcs.find(nm->ident);
                if (f == o.funcs.end()) return;
                for (auto* fd : f->second) {
                    if (!require_pub || fd->is_public) cands.push_back(fd);
                }
            };
            UnitInfo& ui = info();
            add_funcs(ui, false);
            // A local function with the same name fully shadows imports (consistent with local types/variables)
            if (cands.empty()) {
                for (auto& use : cur_->uses) {
                    if (use->unit_name == "std.string") continue;  // added below uniformly as an implicit import
                    if (!unit_visible(use->unit_name)) continue;
                    auto it = infos_.find(use->unit_name);
                    if (it != infos_.end()) add_funcs(it->second, true);
                }
                if (UnitInfo* stdn = std_info(); stdn && cur_->name != "std.string") {
                    add_funcs(*stdn, true);
                }
            }

            if (!cands.empty()) {
                FuncDecl* chosen = choose_overload(cands, raw_args(c->args),
                                                   "function `" + nm->ident + "`", c);
                nm->kind = "func";
                nm->sym = chosen;
                target = chosen;
            } else {
                // a unit-level variable of function type: `G(...)`
                bool is_global = info().globals.count(nm->ident) != 0;
                if (!is_global) {
                    for (auto& use : cur_->uses) {
                        if (!unit_visible(use->unit_name)) continue;
                        auto it = infos_.find(use->unit_name);
                        if (it != infos_.end() && it->second.globals.count(nm->ident) != 0) {
                            is_global = true;
                            break;
                        }
                    }
                }
                if (is_global) {
                    TypePtr vt = check_expr(callee);
                    if (vt && is_callable_value(vt)) return check_fnptr_args(c, vt);
                    err("`" + nm->ident + "` is a variable, not a function", c);
                }
                // struct construction: current unit first, then public types of `use`d units
                TypePtr st = find_type_named(cur_->name, nm->ident, false);
                if (!st) {
                    std::string src;
                    for (auto& use : cur_->uses) {
                        if (!unit_visible(use->unit_name)) continue;
                        TypePtr t = find_type_named(use->unit_name, nm->ident, true);
                        if (!t) continue;
                        if (st) {
                            err("type `" + nm->ident + "` is ambiguous: `" + src + "` and `" +
                                    use->unit_name + "`; use a qualified name",
                                c);
                        }
                        st = t;
                        src = use->unit_name;
                    }
                }
                if (!st && std_info()) st = find_type_named("std.string", nm->ident, true);
                if (st) {
                    if (st->kind != TypeKind::Struct) {
                        err("`" + nm->ident + "` is not a struct and cannot be constructed by value", c);
                    }
                    struct_ty = st;
                } else if (lookup_enum_const(nm->ident) != nullptr) {
                    err("`" + nm->ident + "` is an enum member and cannot be called", c);
                } else {
                    err("undefined function `" + nm->ident + "`", c);
                }
            }
            }
        } else if (auto m = dynamic_cast<Member*>(callee)) {
            Expr* obj = m->obj.get();
            // `.length()` on a built-in managed sequence (`T[]` / `buffer<T>`): the compiler reads the header.
            // Static arrays are C storage and have no `length()`.
            if (m->name == "length") {
                TypePtr ot = check_expr(obj);
                if (ot && (ot->kind == TypeKind::Array || ot->kind == TypeKind::Slice)) {
                    if (ot->kind == TypeKind::Array) {
                        err("a static array has no `length()`: use `sizeof(a) / sizeof(a[0])`", c);
                    }
                    if (c->args.size() > 1) err("`length()` takes at most one argument (the axis)", c);
                    if (ot->rank == 1 && !c->args.empty())
                        err("a one-dimensional sequence has no axis: write `x.length()`", c);
                    if (!c->args.empty()) {
                        TypePtr at = check_expr(c->args[0].get());
                        if (!is_integer(at))
                            err("the axis of `length(axis)` must be an integer", c->args[0].get());
                        // A constant axis is checked here; a runtime axis aborts in `emitSliceDimDynamic`.
                        if (const_ok(c->args[0].get())) {
                            ConstVal cv;
                            if (eval_const(c->args[0].get(), nullptr, cv) && !cv.is_float) {
                                const long long a = cv.i;
                                if (a < 0 || a >= ot->rank) {
                                    err("axis " + std::to_string(a) + " is out of range for a rank-" +
                                            std::to_string(ot->rank) + " sequence (valid axes: 0.." +
                                            std::to_string(ot->rank - 1) + ")",
                                        c->args[0].get());
                                }
                            }
                        }
                    }
                    c->callee_kind = "slice_length";
                    return Type::make_prim("usize");
                }
            }
            // super.method(...)
            if (auto on = dynamic_cast<Name*>(obj);
                on && on->ident == "super" && lookup_local("super") == nullptr) {
                if (!cur_class_ || !cur_class_->base) err("the current class has no base class", c);
                std::vector<const MethodSig*> ms;
                collect_class_methods(cur_class_->base, m->name, ms);
                if (ms.empty()) err("base class has no method `" + m->name + "`", c);
                auto sig = choose_method(ms, raw_args(c->args), "method `" + m->name + "`", c);
                if (sig->is_abstract) {
                    err("abstract method `" + m->name + "` cannot be called via `super`", c);
                }
                if (!decl_visible(sig->owner ? sig->owner->unit : std::string(), sig->is_public)) {
                    err("method `" + m->name + "` is not declared public and cannot be called from another unit", c);
                }
                check_method_args(c, sig, "method `" + m->name + "`");
                m->kind = "method";
                c->callee_kind = "super";
                c->method = sig;
                c->by_ref = is_borrow_type(sig->ret);
                c->ref_const = c->by_ref && sig->ret->is_const;
                return referent_of(sig->ret);
            }

            std::string uname;
            if (auto nm = dynamic_cast<Name*>(obj)) {
                if (lookup_local(nm->ident) == nullptr && infos_.count(nm->ident)) {
                    uname = nm->ident;
                }
            }
            if (uname.empty()) {
                // sum-type construction: `Value.Int(3)` — the callee is `Sum.Alternative(args)`.
                // Checked before `check_expr(obj)`, because a type name is not an expression.
                if (auto nm = dynamic_cast<Name*>(obj); nm && lookup_local(nm->ident) == nullptr) {
                    TypePtr st = find_type_named(cur_->name, nm->ident, false);
                    if (!st) {
                        for (auto& use : cur_->uses) {
                            if (!unit_visible(use->unit_name)) continue;
                            if (TypePtr t = find_type_named(use->unit_name, nm->ident, true)) { st = t; break; }
                        }
                    }
                    if (st && st->kind == TypeKind::Sum) {
                        int idx = -1;
                        for (size_t k = 0; k < st->sum_names.size(); ++k) {
                            if (st->sum_names[k] == m->name) { idx = static_cast<int>(k); break; }
                        }
                        if (idx < 0) {
                            err("sum type `" + st->str() + "` has no alternative `" + m->name + "`", c);
                        }
                        const auto& fs = st->sum_fields[static_cast<size_t>(idx)];
                        if (c->args.size() != fs.size()) {
                            err("alternative `" + m->name + "` of `" + st->str() + "` takes " +
                                    std::to_string(fs.size()) + " value(s), but " +
                                    std::to_string(c->args.size()) + " were given",
                                c);
                        }
                        for (size_t k = 0; k < fs.size(); ++k) {
                            check_assign(c->args[k].get(), fs[k], "sum payload");
                        }
                        nm->kind = "type";
                        nm->ty = st;
                        m->kind = "sum_alt";
                        c->callee_kind = "sum_ctor";
                        c->sum_ty = st;
                        c->sum_alt = idx;
                        return st;
                    }
                }
                TypePtr ot = check_expr(obj);
                if (is_reference_type(ot)) {
                    std::vector<const MethodSig*> ms;
                    collect_methods(ot, m->name, ms);
                    if (ms.empty()) {
                        TypePtr ft = ot->field_type(m->name);
                        if (ft && is_callable_value(ft)) {
                            TypePtr ct = check_expr(callee);  // resolves the field access and sets the node's type
                            return check_fnptr_args(c, ct);
                        }
                        err("type `" + ot->str() + "` has no method `" + m->name + "`", c);
                    }
                    auto sig = choose_method(ms, raw_args(c->args), "method `" + m->name + "`", c);
                    if (!decl_visible(sig->owner ? sig->owner->unit : std::string(), sig->is_public)) {
                        err("method `" + m->name + "` is not declared public and cannot be called from another unit", c);
                    }
                    check_method_args(c, sig, "method `" + m->name + "`");
                    m->kind = "method";
                    c->callee_kind = "method";
                    c->method = sig;
                    c->by_ref = is_borrow_type(sig->ret);
                    c->ref_const = c->by_ref && sig->ret->is_const;
                    return referent_of(sig->ret);
                }
                if (ot && ot->kind == TypeKind::Struct) {
                    std::vector<const MethodSig*> ms;
                    collect_methods(ot, m->name, ms);
                    if (ms.empty()) {
                        TypePtr ft = ot->field_type(m->name);
                        if (ft && is_callable_value(ft)) {
                            TypePtr ct = check_expr(callee);  // resolves the field access and sets the node's type
                            return check_fnptr_args(c, ct);
                        }
                        err("struct `" + ot->str() + "` has no method `" + m->name + "`", c);
                    }
                    auto sig = choose_method(ms, raw_args(c->args), "method `" + m->name + "`", c);
                    if (!decl_visible(sig->owner ? sig->owner->unit : std::string(), sig->is_public)) {
                        err("method `" + m->name + "` is not declared public and cannot be called from another unit", c);
                    }
                    check_method_args(c, sig, "method `" + m->name + "`");
                    m->kind = "method";
                    c->callee_kind = "struct_method";
                    c->method = sig;
                    c->by_ref = is_borrow_type(sig->ret);
                    c->ref_const = c->by_ref && sig->ret->is_const;
                    return referent_of(sig->ret);
                }
                // any other callee expression: `arr[i](...)` / `getfn()(...)`
                {
                    TypePtr ft = check_expr(callee);
                    if (ft && is_callable_value(ft)) return check_fnptr_args(c, ft);
                }
                err("`" + m->name + "` is not a function", c);
            }
            if (!unit_imported(uname)) err("unit `" + uname + "` is not imported with `use`", c);
            if (!unit_visible(uname)) {
                err("unit `" + uname + "` is only visible within its library; cross-library use requires `public unit`", c);
            }

            UnitInfo& ui = infos_.at(uname);
            auto f = ui.funcs.find(m->name);
            if (f == ui.funcs.end() || f->second.empty()) {
                err("unit `" + uname + "` has no function `" + m->name + "`", c);
            }
            FuncDecl* chosen = choose_overload(f->second, raw_args(c->args),
                                               "function `" + uname + "." + m->name + "`", c);
            if (!chosen->is_public) {
                err("function `" + uname + "." + m->name + "` is not declared public", c);
            }
            auto nm = dynamic_cast<Name*>(obj);
            nm->kind = "unit";
            nm->ty = Type::make_module(uname);
            m->kind = "func";
            m->sym = chosen;
            target = chosen;
        } else {
            // any callee expression of function type: `fp(...)`, `arr[i](...)`, `getfn()(...)`
            TypePtr ft = check_expr(callee);
            if (ft && is_callable_value(ft)) return check_fnptr_args(c, ft);
            err("only functions and struct constructors can be called", c);
        }

        if (struct_ty) {
            if (struct_ty->is_union) {
                err("union `" + struct_ty->str() + "` cannot be constructed with `T(...)`; use `{ member = value }`", c);
            }
            if (!struct_ty->ctors.empty()) {
                if (!c->named.empty()) err("struct constructors do not support named arguments", c);
                if (struct_ty->has_dtor && c != direct_init_expr_) {
                    err("a struct with a destructor can only be constructed in a declaration initializer", c);
                }
                const auto* chosen = choose_ctor(struct_ty->ctors, raw_args(c->args),
                                                 "struct `" + struct_ty->str() + "`", c);
                const size_t ci = static_cast<size_t>(chosen - struct_ty->ctors.data());
                if (ci < struct_ty->ctor_is_public.size() && !struct_ty->ctor_is_public[ci] &&
                    (!cur_ || cur_->name != struct_ty->unit)) {
                    err("constructor is not declared public and cannot be called from another unit", c);
                }
                for (size_t i = 0; i < c->args.size(); ++i) {
                    check_assign(c->args[i].get(), (*chosen)[i], "constructor argument");
                }
                c->callee_kind = "struct_ctor";
                c->ctor = struct_ty;
                c->ctor_pty = chosen;
                return struct_ty;
            }
            c->callee_kind = "ctor";
            c->ctor = struct_ty;
            check_ctor_args(c, struct_ty);
            return struct_ty;
        }

        if (!target) err("only functions and struct constructors can be called", c);
        c->callee_kind = "func";
        c->func = target;
        c->by_ref = is_borrow_type(target->rty);
        c->ref_const = c->by_ref && target->rty->is_const;
        const auto& params = target->pty;

        if (!c->named.empty()) {
            err("function calls do not support named arguments (only struct construction does)", c);
        }
        if (c->args.size() != params.size() && !target->is_varargs) {
            err("function `" + target->name + "` expects " + std::to_string(params.size()) +
                    " arguments, but " + std::to_string(c->args.size()) + " were given",
                c);
        }
        if (target->is_varargs && c->args.size() < params.size()) {
            err("variadic function `" + target->name + "` expects at least " +
                    std::to_string(params.size()) + " arguments, but " +
                    std::to_string(c->args.size()) + " were given",
                c);
        }
        for (size_t i = 0; i < params.size() && i < c->args.size(); ++i) {
            const bool is_ref = i < target->pty_ref.size() && target->pty_ref[i];
            if (is_ref) {
                Expr* a = c->args[i].get();
                TypePtr at = check_expr(a);
                if (roots_at_outer_value_local(a)) {
                    err("cannot pass an enclosing local by `ref` inside a `para` section", a);
                }
                if (!is_lvalue(a) && !is_borrow_expr(a)) {
                    err("argument " + std::to_string(i + 1) + " of `" + target->name +
                            "` must be an addressable variable: a `ref` parameter borrows the caller's storage",
                        a);
                }
                if (!same_type(at, params[i])) {
                    err("argument " + std::to_string(i + 1) + " of `" + target->name + "` must be `" +
                            params[i]->str() + "` (found `" + at->str() +
                            "`): `ref` parameters do not convert",
                        a);
                }
                const bool is_cref = i < target->pty_cref.size() && target->pty_cref[i];
                if (!is_cref) {
                    if (root_is_const(a)) {
                        err("cannot bind a readonly (`const`) variable to a mutable `ref` parameter", a);
                    }
                    if (auto* bc = dynamic_cast<Call*>(a); bc && bc->by_ref && bc->ref_const) {
                        err("cannot bind a readonly (`ref const`) borrow to a mutable `ref` parameter", a);
                    }
                    if (auto* bi = dynamic_cast<Index*>(a); bi && bi->by_ref && bi->ref_const) {
                        err("cannot bind a readonly (`ref const`) borrow to a mutable `ref` parameter", a);
                    }
                }
                continue;
            }
            check_assign(c->args[i].get(), params[i], "argument of call to `" + target->name + "`");
        }
        if (target->is_varargs) {
            // Arguments of `...` have no static type: only ABI passability is required (scalar / pointer / POD).
            for (size_t i = params.size(); i < c->args.size(); ++i) {
                TypePtr at = c->args[i]->ty;
                if (!at) continue;
                if (at->needs_cleanup() || is_reference_type(at)) {
                    err("variadic argument " + std::to_string(i + 1) + " cannot be `" + at->str() +
                            "`: `...` cannot carry values that need reference counting",
                        c->args[i].get());
                }
            }
        }
        return referent_of(target->rty);
    }

    int Sema::rank_arg(Expr* e, const TypePtr& aty, const TypePtr& p) {
        if (!p || !aty) return -1;
        // Implicit struct construction is the weakest match: exact > widening > implicit conversion.
        auto implicit_rank = [&]() { return find_implicit_conv(p, aty) ? 2 : -1; };
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (same_type(aty, p)) return 0;
            if (is_numeric(p) && promote_int_literal(il->value, p)) return 1;
            return implicit_rank();
        }
        if (auto fl = dynamic_cast<FloatLit*>(e)) {
            (void)fl;
            if (same_type(aty, p)) return 0;
            if (p->kind == TypeKind::Prim && (p->name == "float" || p->name == "double")) return 1;
            return implicit_rank();
        }
        if (dynamic_cast<NullLit*>(e)) {
            if (p->kind == TypeKind::Ptr || p->kind == TypeKind::Slice ||
                p->kind == TypeKind::Class || p->kind == TypeKind::Interface) {
                return 0;
            }
            return implicit_rank();
        }
        if (same_type(aty, p)) return 0;
        if (implicit_ok(aty, p)) return 1;
        return implicit_rank();
    }

    bool Sema::dominates(const std::vector<int>& a, const std::vector<int>& b) {
        bool strict = false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] > b[i]) return false;
            if (a[i] < b[i]) strict = true;
        }
        return strict;
    }

    bool Sema::is_user_type(const TypePtr& t) {
        return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Class ||
                     t->kind == TypeKind::Interface);
    }

    TypePtr Sema::decay_param_type(const TypePtr& t) {
        if (!t || t->kind != TypeKind::Array || !t->elem) return t;
        if (t->elem->kind == TypeKind::Class || t->elem->kind == TypeKind::Interface) return t;
        return Type::make_ptr(t->elem);
    }

    bool Sema::borrowable_type(const TypePtr& t) {
        if (!t) return false;
        switch (t->kind) {
            case TypeKind::Prim:
            case TypeKind::Enum:
            case TypeKind::Ptr: return true;
            case TypeKind::Struct:
                return !t->is_union && !t->has_dtor && !has_ref_fields(t);
            default: return false;  // void / string / slice / class / interface / module / null / range / ref
        }
    }

    // Return type resolution for `ref T` / `ref const T` / `T` (shared by methods and free functions).
    TypePtr Sema::make_borrow_ret(TypeSpec* spec, bool ret_ref, Node* at, Unit* u, const std::string& what) {
        const bool ret_const = ret_ref && spec && spec->is_const;
        if (ret_ref && spec) spec->is_const = false;
        TypePtr rt = resolve_type(spec, u);
        if (!ret_ref) return rt;
        if (is_void(rt)) err("`" + what + "`: `ref` return type cannot be `void`", at);
        if (!borrowable_type(rt)) {
            err("`" + what + "`: `" + rt->str() +
                    "` cannot be returned by `ref` (borrows are limited to non-reference-counted values)",
                at);
        }
        return Type::make_ref(rt, ret_const);
    }

    void Sema::resolve_method_ret(MethodDecl* m, Unit* u) {
        m->rty = make_borrow_ret(m->ret.get(), m->ret_ref, m, u, "method `" + m->name + "`");
        if (noncopyable_struct(referent_of(m->rty))) {
            err("struct `" + referent_of(m->rty)->str() +
                    "` has a destructor and cannot be returned by value (use a pointer)",
                m);
        }
    }

    bool Sema::has_ref_fields(const TypePtr& t) {
        if (!t) return false;
        switch (t->kind) {
            case TypeKind::Class:
            case TypeKind::Interface:
            case TypeKind::Slice: return true;
            case TypeKind::Array: return has_ref_fields(t->elem);
            case TypeKind::Struct:
                for (const auto& f : t->fields) {
                    if (f.weak) continue;  // a weak field does not hold a reference
                    if (has_ref_fields(f.type)) return true;
                }
                return false;
            default: return false;
        }
    }

    std::vector<FuncDecl*> Sema::collect_operators(const std::string& sym,
                                             const std::vector<TypePtr>& operands) {
        std::unordered_set<std::string> scope;
        if (cur_) {
            scope.insert(cur_->name);
            for (auto& u : cur_->uses) scope.insert(u->unit_name);
        }
        for (auto& t : operands) {
            if (t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Class ||
                      t->kind == TypeKind::Enum || t->kind == TypeKind::Interface)) {
                if (!t->unit.empty()) scope.insert(t->unit);
            }
        }

        std::vector<FuncDecl*> out;
        const std::string name = "operator" + sym;
        for (auto& entry : infos_) {
            if (!scope.empty() && !scope.count(entry.first)) continue;
            auto it = entry.second.funcs.find(name);
            if (it != entry.second.funcs.end()) {
                out.insert(out.end(), it->second.begin(), it->second.end());
            }
        }
        return out;
    }

    std::vector<Expr*> Sema::raw_args(const std::vector<ExprPtr>& v) {
        std::vector<Expr*> out;
        out.reserve(v.size());
        for (auto& e : v) out.push_back(e.get());
        return out;
    }

    FuncDecl* Sema::choose_overload(const std::vector<FuncDecl*>& cands,
                              const std::vector<Expr*>& args, const std::string& what,
                              Node* at) {
        std::vector<TypePtr> atys;
        for (auto* a : args) atys.push_back(check_expr(a));

        std::vector<FuncDecl*> ok;
        std::vector<std::vector<int>> ranks;
        for (auto* f : cands) {
            if (f->is_varargs ? f->pty.size() > args.size() : f->pty.size() != args.size()) continue;
            std::vector<int> r;
            bool good = true;
            for (size_t i = 0; i < f->pty.size(); ++i) {
                int k = rank_arg(args[i], atys[i], f->pty[i]);
                if (k < 0) { good = false; break; }
                r.push_back(k);
            }
            if (good) { ok.push_back(f); ranks.push_back(std::move(r)); }
        }
        if (ok.empty()) {
            if (cands.size() == 1 && cands[0]->pty.size() == args.size()) {
                for (size_t i = 0; i < args.size(); ++i) {
                    check_assign(args[i], cands[0]->pty[i], what + " argument");
                }
            }
            err("no matching overload of " + what, at);
        }

        int best = -1;
        for (size_t i = 0; i < ok.size(); ++i) {
            bool unique = true;
            for (size_t j = 0; j < ok.size(); ++j) {
                if (i == j) continue;
                if (dominates(ranks[j], ranks[i]) || ranks[j] == ranks[i]) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                if (best >= 0) { best = -1; break; }
                best = static_cast<int>(i);
            }
        }
        if (best < 0) err("ambiguous overload call of `" + what + "`", at);
        return ok[static_cast<size_t>(best)];
    }

    void Sema::collect_class_methods(const TypePtr& t, const std::string& name,
                               std::vector<const MethodSig*>& out, int depth) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (m.name != name) continue;
            bool dup = false;
            for (auto* e : out) {
                if (same_sig(*e, m)) { dup = true; break; }
            }
            if (!dup) out.push_back(&m);
        }
        if (t->kind == TypeKind::Class && t->base) {
            collect_class_methods(t->base, name, out, depth + 1);
        }
    }

    void Sema::collect_methods(const TypePtr& t, const std::string& name,
                         std::vector<const MethodSig*>& out, int depth) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (m.name != name) continue;
            bool dup = false;
            for (auto* e : out) {
                if (same_sig(*e, m)) { dup = true; break; }
            }
            if (!dup) out.push_back(&m);  // derived class / first visitor wins, overriding same-signature entries from the base class
        }
        if (t->kind == TypeKind::Class && t->base) {
            collect_methods(t->base, name, out, depth + 1);
        }
        for (auto& i : t->interfaces) {
            collect_methods(i, name, out, depth + 1);
        }
    }

    const MethodSig* Sema::find_member_operator(const TypePtr& recv, const std::string& name,
                                                const std::vector<Expr*>& args, Node* at) {
        // Member operators live on value types only: a class operator would be virtual (operators are
        // static-dispatch and `==` on a class is identity), so the parser does not accept them there.
        if (!recv || recv->kind != TypeKind::Struct) return nullptr;
        std::vector<const MethodSig*> cands;
        for (const auto& m : recv->methods) {
            if (!m.is_operator || m.name != name) continue;
            cands.push_back(&m);
        }
        if (cands.empty()) return nullptr;
        for (const MethodSig* c : cands) {
            if (!decl_visible(c->owner ? c->owner->unit : std::string(), c->is_public)) {
                err("operator `" + name + "` of `" + recv->str() + "` is not declared public", at);
            }
        }
        // Only take over when an overload actually accepts the arguments: `-s` and `s - t` share one symbol,
        // so a non-matching member must fall back to the free-function lookup instead of erroring here.
        std::vector<TypePtr> atys;
        for (auto* a : args) atys.push_back(a->ty ? a->ty : check_expr(a));
        std::vector<const MethodSig*> ok;
        for (const MethodSig* c : cands) {
            if (c->params.size() != args.size()) continue;
            bool good = true;
            for (size_t i = 0; i < args.size(); ++i) {
                if (rank_arg(args[i], atys[i], c->params[i]) < 0) {
                    good = false;
                    break;
                }
            }
            if (good) ok.push_back(c);
        }
        if (ok.empty()) return nullptr;
        return choose_method(ok, args, "`" + name + "` of `" + recv->str() + "`", at);
    }

    const MethodSig* Sema::choose_method(const std::vector<const MethodSig*>& cands,
                                   const std::vector<Expr*>& args,
                                   const std::string& what, Node* at) {
        std::vector<TypePtr> atys;
        for (auto* a : args) atys.push_back(check_expr(a));

        std::vector<const MethodSig*> ok;
        std::vector<std::vector<int>> ranks;
        for (auto* sig : cands) {
            if (sig->params.size() != args.size()) continue;
            std::vector<int> r;
            bool good = true;
            for (size_t i = 0; i < args.size(); ++i) {
                int k = rank_arg(args[i], atys[i], sig->params[i]);
                if (k < 0) { good = false; break; }
                r.push_back(k);
            }
            if (good) { ok.push_back(sig); ranks.push_back(std::move(r)); }
        }
        if (ok.empty()) err("no matching overload of " + what, at);

        int best = -1;
        for (size_t i = 0; i < ok.size(); ++i) {
            bool unique = true;
            for (size_t j = 0; j < ok.size(); ++j) {
                if (i == j) continue;
                if (dominates(ranks[j], ranks[i]) || ranks[j] == ranks[i]) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                if (best >= 0) { best = -1; break; }
                best = static_cast<int>(i);
            }
        }
        if (best < 0) err("ambiguous overload call of `" + what + "`", at);
        return ok[static_cast<size_t>(best)];
    }

    const std::vector<TypePtr>* Sema::choose_ctor(const std::vector<std::vector<TypePtr>>& ctors,
                                            const std::vector<Expr*>& args,
                                            const std::string& what, Node* at) {
        std::vector<TypePtr> atys;
        for (auto* a : args) atys.push_back(check_expr(a));

        std::vector<const std::vector<TypePtr>*> ok;
        std::vector<std::vector<int>> ranks;
        for (auto& cs : ctors) {
            if (cs.size() != args.size()) continue;
            std::vector<int> r;
            bool good = true;
            for (size_t i = 0; i < args.size(); ++i) {
                int k = rank_arg(args[i], atys[i], cs[i]);
                if (k < 0) { good = false; break; }
                r.push_back(k);
            }
            if (good) { ok.push_back(&cs); ranks.push_back(std::move(r)); }
        }
        if (ok.empty()) err("no matching constructor for " + what, at);

        int best = -1;
        for (size_t i = 0; i < ok.size(); ++i) {
            bool unique = true;
            for (size_t j = 0; j < ok.size(); ++j) {
                if (i == j) continue;
                if (dominates(ranks[j], ranks[i]) || ranks[j] == ranks[i]) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                if (best >= 0) { best = -1; break; }
                best = static_cast<int>(i);
            }
        }
        if (best < 0) err("ambiguous constructor call of `" + what + "`", at);
        return ok[static_cast<size_t>(best)];
    }

    void Sema::check_method_args(Call* c, const MethodSig* sig, const std::string& what) {
        if (!c->named.empty()) err("method calls do not support named arguments", c);
        if (c->args.size() != sig->params.size()) {
            err(what + " expects " + std::to_string(sig->params.size()) + " arguments, but " +
                    std::to_string(c->args.size()) + " were given",
                c);
        }
        for (size_t i = 0; i < c->args.size(); ++i) {
            check_assign(c->args[i].get(), sig->params[i], what + " argument");
        }
    }

    TypePtr Sema::check_super_call(Call* c) {
        if (!in_ctor_ || !super_ok_) {
            err("`super(...)` must be the first statement of a constructor", c);
        }
        if (!cur_class_ || !cur_class_->base) err("the current class has no base class", c);
        if (!c->named.empty()) err("constructor calls do not support named arguments", c);
        const auto& ctors = cur_class_->base->ctors;
        if (ctors.empty()) {
            if (!c->args.empty()) err("base class has no matching constructor", c);
        } else {
            const auto* chosen = choose_ctor(ctors, raw_args(c->args), "base class", c);
            for (size_t i = 0; i < c->args.size(); ++i) {
                check_assign(c->args[i].get(), (*chosen)[i], "super argument");
            }
            c->ctor_pty = chosen;
        }
        c->callee_kind = "super";
        return t_void();
    }

    [[noreturn]] void Sema::err_slice_elem_align(const TypePtr& elem, const Node* node) const {
        err("type `" + elem->str() + "` has align(" + std::to_string(type_align(elem)) +
                ") greater than 8: the data area of a dynamic array follows a 24-byte header, so elements can be aligned to at most 8 bytes; "
                "use a fixed-size array or a pointer instead, or lower `align(N)`",
            node);
    }

    TypePtr Sema::check_new(NewExpr* ne) {
        TypePtr t = resolve_type(ne->type.get(), cur_);
        if (ne->is_array) {
            if (is_void(t)) err("dynamic array element cannot be void", ne);
            if (ne->dims.empty()) err("dynamic array is missing its length", ne);
            for (auto& d : ne->dims) {
                TypePtr ct = check_expr(d.get());
                if (!(ct && ct->is_integer())) {
                    err("dynamic array length must be an integer", d.get());
                }
            }
            if (t->kind == TypeKind::Struct && t->has_dtor) {
                err("struct `" + t->str() +
                        "` has a destructor and cannot be a dynamic array element (element construction and destruction are not implemented)",
                    ne);
            }
            if (!ne->args.empty() || !ne->named.empty()) {
                err("dynamic arrays cannot take constructor arguments", ne);
            }
            ne->class_ty = t;
            if (ne->jagged_rank > 0) {
                if (ne->dims.size() != 1) {
                    err("a jagged array `new T[m][]` creates one dimension at a time; "
                        "fill rows in a loop for two dimensions, or use `new T[m,n]` for a rectangular array",
                        ne);
                }
                TypePtr row = t;
                for (int i = 0; i < ne->jagged_rank; ++i) row = Type::make_slice(row);
                ne->elem_ty = row;  // element of the outer array: T[]/T[][]/...
                return Type::make_slice(row);
            }
            if (type_align(t) > 8) err_slice_elem_align(t, ne);
            ne->elem_ty = t;
            return Type::make_slice(t, false, static_cast<int>(ne->dims.size()));
        }
        if (t->kind != TypeKind::Class) err("`new` can only be used with a class or a dynamic array", ne);
        if (t->is_abstract) err("abstract class `" + t->str() + "` cannot be instantiated with `new`", ne);
        if (!ne->named.empty()) err("constructors do not support named arguments yet", ne);

        const auto& ctors = t->ctors;
        ne->has_ctor = !ctors.empty();
        if (ctors.empty()) {
            if (!ne->args.empty()) err("class `" + t->str() + "` has no matching constructor", ne);
        } else {
            const auto* chosen = choose_ctor(ctors, raw_args(ne->args), "class `" + t->str() + "`", ne);
            const size_t idx = static_cast<size_t>(chosen - ctors.data());
            if (idx < t->ctor_is_public.size() && !t->ctor_is_public[idx] &&
                (!cur_ || cur_->name != t->unit)) {
                err("constructor is not declared public and cannot be invoked with `new` from another unit", ne);
            }
            for (size_t i = 0; i < ne->args.size(); ++i) {
                check_assign(ne->args[i].get(), (*chosen)[i], "constructor argument");
            }
            ne->ctor_pty = *chosen;
        }
        ne->class_ty = t;
        return t;
    }

    void Sema::check_ctor_args(Call* c, const TypePtr& ty) {
        const auto& fields = ty->fields;

        // A cross-unit `S(...)` construction requires all fields to be public
        if (cur_ && cur_->name != ty->unit) {
            for (auto& f : fields) {
                if (!f.is_public) {
                    err("field `" + f.name + "` of struct `" + ty->str() +
                            "` is not declared public and cannot be constructed from another unit",
                        c);
                }
            }
        }

        if (c->args.empty() && c->named.empty()) {
            c->field_order.clear();
            return;
        }

        if (!c->named.empty()) {
            std::unordered_map<std::string, size_t> fmap;
            for (size_t i = 0; i < fields.size(); ++i) fmap.emplace(fields[i].name, i);

            std::vector<int> order;
            std::unordered_set<std::string> seen;
            for (auto& [n, e] : c->named) {
                auto it = fmap.find(n);
                if (it == fmap.end()) {
                    err("struct `" + ty->str() + "` has no field `" + n + "`", c);
                }
                if (!seen.insert(n).second) err("field `" + n + "` is initialized more than once", c);
                const size_t idx = it->second;
                order.push_back(static_cast<int>(idx));
                check_assign(e.get(), fields[idx].type, "construction of `" + ty->str() + "`");
            }
            if (order.size() != fields.size()) {
                err("construction of `" + ty->str() + "` must initialize every field", c);
            }
            c->field_order = std::move(order);
            return;
        }

        if (c->args.size() != fields.size()) {
            err("construction of `" + ty->str() + "` expects " + std::to_string(fields.size()) +
                    " arguments, but " + std::to_string(c->args.size()) + " were given",
                c);
        }
        c->field_order.clear();
        for (size_t i = 0; i < fields.size(); ++i) {
            check_assign(c->args[i].get(), fields[i].type, "construction of `" + ty->str() + "`");
            c->field_order.push_back(static_cast<int>(i));
        }
    }

    TypePtr Sema::check_builtin(Call* c, const std::string& name) {
        c->callee_kind = "builtin";
        c->builtin = name;
        if (!c->named.empty()) err("builtin function `" + name + "` does not support named arguments", c);

        if (name == "alloc") {
            if (c->args.size() != 1) err("`alloc(bytes)` takes 1 argument", c);
            if (!is_integer(check_expr(c->args[0].get()))) {
                err("size passed to `alloc` must be an integer", c);
            }
            return Type::make_ptr(t_void());
        }
        if (name == "realloc") {
            if (c->args.size() != 2) err("`realloc(p, bytes)` takes 2 arguments", c);
            TypePtr p0 = check_expr(c->args[0].get());
            if (!p0 || p0->kind != TypeKind::Ptr) {
                err("first argument of `realloc` must be a pointer", c);
            }
            if (!is_integer(check_expr(c->args[1].get()))) {
                err("size passed to `realloc` must be an integer", c);
            }
            return Type::make_ptr(t_void());
        }
        if (name == "free") {
            if (c->args.size() != 1) err("`free(p)` takes 1 argument", c);
            TypePtr p0 = check_expr(c->args[0].get());
            if (!p0 || (p0->kind != TypeKind::Ptr && p0->kind != TypeKind::Null)) {
                err("argument of `free` must be a pointer", c);
            }
            return t_void();
        }
        err("unknown builtin function `" + name + "`", c);
    }

    bool Sema::is_lvalue(Expr* e) {
        if (!e) return false;
        if (auto nm = dynamic_cast<Name*>(e)) {
            return nm->ident != "true" && nm->ident != "false" && nm->ident != "null";
        }
        if (dynamic_cast<Index*>(e)) return true;
        if (auto m = dynamic_cast<Member*>(e)) {
            auto nm = dynamic_cast<Name*>(m->obj.get());
            if (!nm) return true;
            // Local variables / this unit's globals take precedence over a unit of the same name (in `t.c = x`, t is a variable, not a unit)
            if (lookup_local(nm->ident)) return true;
            if (info().globals.count(nm->ident)) return true;
            return !infos_.count(nm->ident);
        }
        if (auto u = dynamic_cast<Unary*>(e)) return u->op == "*";
        return false;
    }

    bool Sema::rooted_at_reference(Expr* e) {
        while (e) {
            if (auto m = dynamic_cast<Member*>(e)) {
                e = m->obj.get();
                continue;
            }
            if (auto ix = dynamic_cast<Index*>(e)) {
                e = ix->base.get();
                continue;
            }
            break;
        }
        if (!e || !e->ty) return false;
        TypePtr t = e->ty;
        if (is_reference_type(t)) return true;
        if (t->kind == TypeKind::Array || t->kind == TypeKind::Slice) {
            TypePtr el = t->elem;
            while (el && (el->kind == TypeKind::Array || el->kind == TypeKind::Slice)) {
                el = el->elem;
            }
            return is_reference_type(el);
        }
        return false;
    }

    void Sema::check_entry() {
        auto it = infos_.find(entry_);
        if (it == infos_.end()) throw SemaError("cannot find entry unit `" + entry_ + "`");
        UnitInfo& ui = it->second;
        // The Python version took the filename from self.cur (the unit checked last at this point),
        // which could point at the wrong file; here it points explicitly at the entry unit.
        cur_ = ui.unit;

        auto f = ui.funcs.find("main");
        if (f == ui.funcs.end() || f->second.empty()) {
            throw SemaError("unit `" + entry_ + "` has no `main` function");
        }
        FuncDecl* main = nullptr;
        for (auto* cand : f->second) {
            if (cand->params.empty()) {
                main = cand;
                break;
            }
        }
        if (!main) {
            // `int main(int argc, char** argv)`
            for (auto* cand : f->second) {
                if (cand->params.size() == 2) {
                    main = cand;
                    break;
                }
            }
        }
        if (!main) main = f->second.front();
        if (!main->rty) err("`main` is missing its return type", main);
        if (main->rty->str() != "int") err("`main` must return `int`", main);
        if (main->params.empty()) return;
        if (main->params.size() != 2) {
            err("`main` must have either no parameters or exactly 2: `(int argc, char** argv)`", main);
        }
        // The second parameter must be `char**` (argv is provided by the CRT, the last element is null)
        static auto is_char = [](const TypePtr& t) {
            return t && t->kind == TypeKind::Prim && t->name == "char";
        };
        const TypePtr& p0 = main->pty[0];
        const TypePtr& p1 = main->pty[1];
        bool ok0 = p0 && same_type(p0, t_int());
        bool ok1 = p1 && p1->kind == TypeKind::Ptr && p1->elem &&
                   p1->elem->kind == TypeKind::Ptr && is_char(p1->elem->elem);
        if (!ok0 || !ok1) {
            err("`main` parameters must be `(int argc, char** argv)`", main);
        }
    }
}  // namespace muc_detail
