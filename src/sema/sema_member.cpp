// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Names and member access (definitions of Sema members).
//
// owns:      `check_name` (locals, globals, unit-qualified names, type-level constants, sum alternatives)
//            and `check_member` (fields, `Type.Member`, unit members, built-in sequence accessors)
// not:       method calls (sema_call.cpp), other expressions
// pass:      body checking
// concept:   names, members

#include "sema_impl.hpp"

#include "ast_clone.hpp"

namespace muc_detail {

    TypePtr Sema::check_name(Name* e, const TypePtr& expected) {
        // A unit alternative resolved from the target type (`Option<int> o = None;`) is memoized.
        if (e->kind == "sum_unit" && e->ty) return e->ty;
        if (e->ident == "super") {
            err("`super` can only be used as `super(...)` or `super.method(...)`", e);
        }
        if (e->ident == "this" && lookup_local("this") == nullptr) {
            err("`this` can only appear inside a class or struct method, constructor, or destructor", e);
        }
        if (e->ident == "this" && !para_stack_.empty()) {
            err("`this` cannot be used inside a `para` section in v1: pass the object as an explicit argument",
                e);
        }
        if (LocalVar* lv = lookup_local(e->ident)) {
            // Reading a scalar local that has not been definitely initialized is an error; writing to
            // it is not a read (the assignment target is checked under `write_ctx_`).
            if (!write_ctx_ && maybe_uninit_.count(lv)) {
                if (arg_ctx_) {
                    deferred_uninit_.push_back({e, lv});
                } else {
                    err("`" + e->ident + "` is read before it is initialized; assign it first "
                            "(`" + lv->name + " = ...`) or give it an initializer at the declaration",
                        e);
                }
            }
            // A moved value cannot be touched again until it is reassigned
            // (agents/value-semantics.md §8.4).
            if (moved_locals_.count(lv)) {
                err("`" + e->ident + "` was moved and cannot be used again (assign a new value to it first)",
                    e);
            }
            // `para` sections capture enclosing locals by reference to the (suspended) caller frame;
            // every enclosing section must record the capture so each level's frame can forward it.
            if (!para_stack_.empty()) {
                for (auto& ctx : para_stack_) {
                    bool has = false;
                    for (LocalVar* x : ctx.seen) {
                        if (x == lv) { has = true; break; }
                    }
                    if (!has) ctx.seen.push_back(lv);
                }
            }
            // A lambda captures explicitly (U41): a name from an enclosing scope must appear in the
            // capture list, so the copying (or the borrow) is visible where the lambda is written.
            if (!lambda_stack_.empty()) {
                const std::size_t base = lambda_stack_.back().scope_base;
                for (std::size_t i = 0; i < base && i < scopes_.size(); ++i) {
                    auto it = scopes_[i].find(e->ident);
                    if (it != scopes_[i].end() && it->second == lv) {
                        err("lambda cannot capture `" + e->ident +
                                "` implicitly: add `" + e->ident + "` (a copy) or `ref " + e->ident +
                                "` (writes reach it) to the capture list",
                            e);
                    }
                }
            }
            e->kind = "local";
            e->sym = lv;
            return lv->rty;
        }
        if (cur_class_) {
            if (TypePtr ft = cur_class_->field_type(e->ident)) {
                if (!para_stack_.empty()) {
                    err("field `" + e->ident +
                            "` cannot be accessed implicitly inside a `para` section: use an explicit object",
                        e);
                }
                e->kind = "field";
                e->implicit_this = true;
                e->field_index = cur_class_->field_index(e->ident);
                return ft;
            }
            // Type-level constant: may be written bare inside methods of the declaring type (and its derived classes)
            if (ConstMember* cm = find_const_member_in(cur_class_, e->ident)) {
                if (!const_member_visible(cm)) {
                    err("const member `" + cm->name + "` is not declared public (type `" +
                            cm->owner->str() + "`)",
                        e);
                }
                e->kind = "const_member";
                e->sym = cm;
                return cm->rty;
            }
        }
        UnitInfo& ui = info();
        if (auto g = ui.globals.find(e->ident); g != ui.globals.end()) {
            check_global_init_order(g->second, e);
            e->kind = "global";
            e->sym = g->second;
            return g->second->rty;
        }
        if (auto f = ui.funcs.find(e->ident); f != ui.funcs.end() && !f->second.empty()) {
            e->kind = "func";
            FuncDecl* chosen = nullptr;
            TypePtr ft = func_value_type(f->second, e->ident, expected, &chosen, e);
            e->sym = chosen;
            return ft;
        }
        if (EnumConst* ec = lookup_enum_const(e->ident)) {
            e->kind = "enum_const";
            e->sym = ec;
            return ec->ty;
        }

        // Imported from directly `use`d units (symbols are directly visible; a name from several sources is an ambiguity error requiring a qualified name)
        GlobalVar* imp_g = nullptr;
        std::string imp_g_unit;
        EnumConst* imp_ec = nullptr;
        std::string imp_ec_unit;
        FuncDecl* imp_f = nullptr;
        std::string imp_f_unit;
        for (auto& use : cur_->uses) {
            if (!unit_visible(use->unit_name)) continue;
            auto it = infos_.find(use->unit_name);
            if (it == infos_.end()) continue;
            UnitInfo& o = it->second;
            if (auto g = o.globals.find(e->ident); g != o.globals.end() && g->second->is_public) {
                if (imp_g) {
                    err("`" + e->ident + "` is ambiguous: `" + imp_g_unit + "." + e->ident + "` and `" +
                            use->unit_name + "." + e->ident + "`; use a qualified name",
                        e);
                }
                imp_g = g->second;
                imp_g_unit = use->unit_name;
            }
            if (auto f = o.funcs.find(e->ident);
                f != o.funcs.end() && !f->second.empty() && f->second.front()->is_public) {
                if (!imp_f) {
                    imp_f = f->second.front();
                    imp_f_unit = use->unit_name;
                }
            }
            if (EnumConst* ec = lookup_enum_const_in(use->unit_name, e->ident, true)) {
                if (imp_ec) {
                    err("`" + e->ident + "` is ambiguous: `" + imp_ec_unit + "." + e->ident + "` and `" +
                            use->unit_name + "." + e->ident + "`; use a qualified name",
                        e);
                }
                imp_ec = ec;
                imp_ec_unit = use->unit_name;
            }
        }
        // A name may be a variable in one imported unit and an enum member (or a function) in another:
        // the kind-specific checks above cannot see that, so the cross-kind collision is reported here
        // instead of silently resolving to one of them (#40).
        const int imported_kinds = (imp_g ? 1 : 0) + (imp_f ? 1 : 0) + (imp_ec ? 1 : 0);
        if (imported_kinds > 1) {
            std::string list;
            auto add = [&](const std::string& what) { list += (list.empty() ? "" : ", ") + what; };
            if (imp_g) add(imp_g_unit + "." + e->ident + " (a variable)");
            if (imp_f) add(imp_f_unit + "." + e->ident + " (a function)");
            if (imp_ec) add(imp_ec_unit + "." + e->ident + " (an enum member)");
            err("`" + e->ident + "` is ambiguous between imported declarations: " + list +
                    "; use a qualified name",
                e);
        }
        if (imp_g) {
            e->kind = "global";
            e->sym = imp_g;
            return imp_g->rty;
        }
        if (imp_f) {
            e->kind = "func";
            FuncDecl* chosen = nullptr;
            TypePtr ft = func_value_type({imp_f}, e->ident, expected, &chosen, e);
            e->sym = chosen;
            return ft;
        }
        if (imp_ec) {
            e->kind = "enum_const";
            e->sym = imp_ec;
            return imp_ec->ty;
        }
        // Contextual construction in a target-typed position: the ternary arms of a sum initialization
        // (`Option<int> v = c ? pick : None;`) and every other place that passes an expected type
        // (a `return None;`, an argument). Initialization and assignment resolve the same form in
        // `check_assign`; this catches the positions that only know the target through `expected`.
        if (expected && expected->kind == TypeKind::Sum) {
            for (size_t k = 0; k < expected->sum_names.size(); ++k) {
                if (expected->sum_names[k] != e->ident) continue;
                if (!expected->sum_fields[k].empty()) break;  // a payload alternative needs a call
                e->kind = "sum_unit";
                e->sum_ty = expected;
                e->sum_alt = static_cast<int>(k);
                return (e->ty = expected);
            }
        }
        err("undefined identifier `" + e->ident + "`", e);
    }

    TypePtr Sema::check_member(Member* e, const TypePtr& expected) {
        Expr* obj = e->obj.get();
        if (!obj) err("missing object for member access", e);

        if (auto nm = dynamic_cast<Name*>(obj);
            nm && lookup_local(nm->ident) == nullptr && info().globals.count(nm->ident) == 0) {
            // A unit of the same name is only considered when the name is not a local/global: the
            // bare-name and lvalue paths already give the variable precedence (#196).
            const std::string uname = nm->ident;
            auto it = infos_.find(uname);
            if (it != infos_.end() && uname != cur_->name) {
                if (!unit_imported(uname)) err("unit `" + uname + "` is not imported with `use`", e);
                if (!unit_visible(uname)) {
                    err("unit `" + uname + "` is only visible within its library; cross-library use requires `public unit`", e);
                }
                UnitInfo& ui = it->second;
                nm->kind = "unit";
                nm->ty = Type::make_module(uname);
                if (auto f = ui.funcs.find(e->name); f != ui.funcs.end() && !f->second.empty()) {
                    std::vector<FuncDecl*> pub;
                    for (auto* cand : f->second) {
                        if (cand->is_public) pub.push_back(cand);
                    }
                    if (pub.empty()) {
                        err("function `" + uname + "." + e->name + "` is not declared public", e);
                    }
                    e->kind = "func";
                    FuncDecl* chosen = nullptr;
                    TypePtr ft = func_value_type(pub, uname + "." + e->name, expected, &chosen, e);
                    e->sym = chosen;
                    return ft;
                }
                if (auto g = ui.globals.find(e->name); g != ui.globals.end()) {
                    if (!g->second->is_public) {
                        err("`" + uname + "." + e->name + "` is not declared public", e);
                    }
                    e->kind = "global";
                    e->sym = g->second;
                    return g->second->rty;
                }
                err("unit `" + uname + "` has no member `" + e->name + "`", e);
            }
        }

        // Enum-qualified reference: `Color.Red` / `unit.Color.Red`
        {
            Expr* base = e->obj.get();
            TypePtr et;
            if (auto nm = dynamic_cast<Name*>(base)) {
                if (lookup_local(nm->ident) == nullptr &&
                    info().globals.find(nm->ident) == info().globals.end()) {
                    et = find_type_in_expr(nm->ident, e);
                    if (!et && !e->type_args.empty()) {
                        // a generic sum used as an alternative: `Option<int>.None`
                        TypeSpec spec;
                        spec.kind = TypeSpec::K::Named;
                        spec.name = nm->ident;
                        spec.line = e->line;
                        spec.col = e->col;
                        for (auto& a : e->type_args) spec.args.push_back(clone_type_spec(*a));
                        et = instantiate_generic(&spec, cur_);
                    }
                    if (et) nm->ty = et;
                }
            } else if (auto inner = dynamic_cast<Member*>(base)) {
                auto un = dynamic_cast<Name*>(inner->obj.get());
                if (un && lookup_local(un->ident) == nullptr) {
                    auto it = infos_.find(un->ident);
                    if (it != infos_.end() && un->ident != cur_->name) {
                        if (!unit_imported(un->ident)) {
                            err("unit `" + un->ident + "` is not imported with `use`", e);
                        }
                        if (!unit_visible(un->ident)) {
                            err("unit `" + un->ident +
                                    "` is only visible within its library; cross-library use requires `public unit`",
                                e);
                        }
                        TypePtr t = find_type_named(un->ident, inner->name, false);
                        if (!t) {
                            err("unit `" + un->ident + "` has no type `" + inner->name + "`", e);
                        }
                        if (!type_is_public(t)) {
                            err("type `" + un->ident + "." + inner->name +
                                    "` is not declared public",
                                e);
                        }
                        et = t;
                        inner->ty = et;
                    }
                }
            }
            if (et) {
                if (et->kind == TypeKind::Sum || e->type_args.size() > 0) {
                    // `Value.None` / `Option<int>.None`: a unit alternative is a value; alternatives with
                    // a payload are constructed as `Value.Int(3)` (handled in check_call).
                    TypePtr st = et;
                    std::string base_name;
                    if (auto on = dynamic_cast<Name*>(obj); on) base_name = on->ident;
                    if (e->type_args.size() > 0) {
                        TypeSpec spec;
                        spec.kind = TypeSpec::K::Named;
                        spec.name = base_name;
                        spec.line = e->line;
                        spec.col = e->col;
                        for (auto& a : e->type_args) spec.args.push_back(clone_type_spec(*a));
                        st = instantiate_generic(&spec, cur_);
                    }
                    if (!st || st->kind != TypeKind::Sum) {
                        err("`" + base_name + "` is not a variant", e);
                    }
                    int idx = -1;
                    for (size_t k = 0; k < st->sum_names.size(); ++k) {
                        if (st->sum_names[k] == e->name) { idx = static_cast<int>(k); break; }
                    }
                    if (idx < 0) err("variant `" + st->str() + "` has no alternative `" + e->name + "`", e);
                    if (!st->sum_fields[static_cast<size_t>(idx)].empty()) {
                        err("alternative `" + e->name + "` of `" + st->str() +
                                "` carries a payload: construct it with `" + st->str() + "." + e->name +
                                "(...)`",
                            e);
                    }
                    e->kind = "sum_unit";
                    e->sum_ty = st;
                    e->sum_alt = idx;
                    obj->ty = st;
                    return st;
                }
                if (et->kind == TypeKind::Enum) {
                    EnumConst* ec = find_enum_member(et, e->name);
                    if (!ec) err("enum `" + et->str() + "` has no member `" + e->name + "`", e);
                    e->kind = "enum_const";
                    e->sym = ec;
                    return et;
                }
                if (et->kind == TypeKind::Struct || et->kind == TypeKind::Class) {
                    ConstMember* cm = find_const_member_in(et, e->name);
                    if (!cm) {
                        // Keep the old "not an enum" diagnostic when the type has no type-level constants at all
                        if (!has_any_const_member(et)) {
                            err("`" + et->str() +
                                    "` is not an enum and cannot be referenced as `Type.Member`",
                                e);
                        }
                        err("type `" + et->str() + "` has no const member `" + e->name + "`", e);
                    }
                    if (!const_member_visible(cm)) {
                        err("const member `" + e->name + "` is not declared public (type `" +
                                et->str() + "`)",
                            e);
                    }
                    e->kind = "const_member";
                    e->sym = cm;
                    return cm->rty;
                }
                err("`" + et->str() + "` is not an enum and cannot be referenced as `Type.Member`", e);
            }
        }

        TypePtr ot = check_expr(obj);
        // A dynamic array exposes its data pointer through the method `ptr()` (SPEC 3.3), not a field:
        // the call form keeps `x.ptr` from reading like state, and sema checks the element type there.
        if (ot && ot->kind == TypeKind::Slice && e->name == "ptr") {
            err("`ptr` is a method on a dynamic array: write `x.ptr()`", e);
        }
        if (ot && (ot->kind == TypeKind::Struct || ot->kind == TypeKind::Class)) {
            TypePtr ft = ot->field_type(e->name);
            if (!ft) {
                // Type-level constants do not occupy instance memory: obj.Name / this.Name is always rejected, suggesting Type.Name instead
                if (find_const_member_in(ot, e->name)) {
                    err("const member `" + e->name + "` must be accessed as `" + ot->name + "." +
                            e->name + "` (const members do not occupy instance memory)",
                        e);
                }
                if (ot->kind == TypeKind::Class && has_method_named(ot, e->name)) {
                    err("method `" + e->name + "` must be called (`x." + e->name + "(...)`)", e);
                }
                err("type `" + ot->str() + "` has no field `" + e->name + "`", e);
            }
            for (auto& f : ot->fields) {
                if (f.name != e->name) continue;
                if (!f.is_public && (!cur_ || cur_->name != ot->unit)) {
                    err("field `" + e->name + "` is not declared public (type `" + ot->str() + "`)", e);
                }
                break;
            }
            e->kind = "field";
            e->field_index = ot->field_index(e->name);
            return ft;
        }
        // `delegate.code` / `delegate.env`: the two words of a delegate value, as `void*` (SPEC 3.16.1a).
        // This is the C interoperability escape hatch — the diagnostic for a delegate in an `extern`
        // signature points here, so the members have to exist.
        if (ot && ot->kind == TypeKind::Delegate) {
            if (e->name == "code" || e->name == "env") {
                e->kind = e->name == "code" ? "delegate_code" : "delegate_env";
                return Type::make_ptr(Type::make_void());
            }
            err("type " + ot->str() + " has no member `" + e->name + "` (a delegate exposes `code` and `env`)", e);
        }
        err("type " + (ot ? ot->str() : "void") + " has no member `" + e->name + "`", e);
    }

}  // namespace muc_detail
