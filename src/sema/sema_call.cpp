// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds call checking and miscellany (definitions of Sema members).
//
// owns:      call resolution — overload choice (choose_overload/choose_method/rank_arg), the method lookup
//            walks, operator lookup (find_member_operator), new/ctor arguments, entry-point checks, the
//            parameter decay and the ref/lvalue rules
// not:       expression typing, statement checking, layout, codegen
// pass:      body checking, before codegen ever sees a callee
// concept:   calls, overloads, operators, new
#include "sema_impl.hpp"

#include "ast_clone.hpp"

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
            ArgGuard ag(call_arg_depth_);
            check_assign(c->args[i].get(), ft->fparams[i], "argument of a call through a function value");
        }
        c->callee_kind = ft->kind == TypeKind::Delegate ? "delegate" : "fnptr";
        c->fn_ty = ft;
        return ft->fret;
    }

    // SPEC 3.18: a method materialized by `extend I with J;` is callable only in units that can see the
    // declaring unit. Dropping the candidates here (before overload resolution) keeps the rule local to
    // the call site; when nothing else provides the name, the diagnostic says which `use` is missing.
    void Sema::filter_extension_visibility(std::vector<const MethodSig*>& ms, const TypePtr& ot, Member* m,
                                           Call* c) {
        if (ms.empty()) return;
        std::vector<const MethodSig*> keep;
        std::string blocked;
        for (auto* s : ms) {
            if (extension_visible(*s)) {
                keep.push_back(s);
            } else if (blocked.empty()) {
                blocked = s->ext_unit;
            }
        }
        if (keep.empty() && !blocked.empty()) {
            err("method `" + m->name + "` on `" + ot->str() + "` comes from an interface extension "
                    "declared in unit `" + blocked + "`, which is not visible here; add `use " + blocked +
                    ";`",
                c);
        }
        ms = std::move(keep);
    }

    // SPEC 3.19: `box.f()` falls back to `box.field.f()` when the wrapper's own, base and interface
    // methods do not provide `f`. The rewrite reuses the ordinary method-call path, so argument
    // checking, const filtering, visibility and codegen need no special case. One level only.
    bool Sema::try_promote_method(Member* m, const TypePtr& ot, Node* at) {
        if (!m || !ot || m->promote_depth > 0 || ot->promotes.empty()) return false;
        int chosen = -1;
        for (size_t i = 0; i < ot->promotes.size(); ++i) {
            const PromoteInfo& pi = ot->promotes[i];
            if (!pi.field_type) continue;
            std::vector<const MethodSig*> hits;
            collect_methods(pi.field_type, m->name, hits);
            const MethodSig* hit = nullptr;
            for (auto* sig : hits) {
                if (sig && sig->is_public) {
                    hit = sig;
                    break;
                }
            }
            if (!hit) continue;
            if (!pi.methods.empty()) {
                bool named = false;
                for (auto& n : pi.methods) {
                    if (n == m->name) named = true;
                }
                if (!named) continue;
            }
            if (chosen >= 0) {
                err("method `" + m->name + "` is promoted by more than one field of `" + ot->str() + "`", at);
            }
            chosen = static_cast<int>(i);
        }
        if (chosen < 0) return false;
        const PromoteInfo& pi = ot->promotes[static_cast<size_t>(chosen)];
        auto inner = std::make_unique<Member>();
        inner->name = ot->fields[static_cast<size_t>(pi.field_index)].name;
        inner->obj = std::move(m->obj);
        inner->line = m->line;
        inner->col = m->col;
        m->obj = std::move(inner);
        m->promote_depth += 1;
        return true;
    }

    // Wrapper: arguments are typed while the callee is still unknown, so a read of a maybe
    // uninitialized scalar is recorded and settled here — a mutable `ref` parameter turns it into a
    // write (an out-parameter), every other parameter makes it a read.
    TypePtr Sema::check_call(Call* c, const TypePtr& expected) {
        const size_t mark = deferred_uninit_.size();
        const bool saved_arg_ctx = arg_ctx_;
        arg_ctx_ = true;
        TypePtr result;
        try {
            result = check_call_impl(c, expected);
        } catch (...) {
            arg_ctx_ = saved_arg_ctx;
            deferred_uninit_.resize(mark);
            throw;
        }
        arg_ctx_ = saved_arg_ctx;
        flush_deferred_uninit(mark, c);
        return result;
    }

    // Settle the reads recorded while typing this call's arguments.
    void Sema::flush_deferred_uninit(size_t mark, Call* c) {
        for (size_t i = mark; i < deferred_uninit_.size(); ++i) {
            const DeferredUninit& d = deferred_uninit_[i];
            size_t param = static_cast<size_t>(-1);
            for (size_t k = 0; k < c->args.size(); ++k) {
                if (c->args[k].get() == d.at) {
                    param = k;
                    break;
                }
            }
            bool mutable_ref = false;
            if (c->callee_kind == "func" && c->func) {
                const FuncDecl* f = c->func;
                mutable_ref = param < f->pty_ref.size() && f->pty_ref[param] &&
                              !(param < f->pty_cref.size() && f->pty_cref[param]);
            } else if (c->method) {
                const MethodSig* m = c->method;
                mutable_ref = param < m->params_ref.size() && m->params_ref[param] &&
                              !(param < m->params_cref.size() && m->params_cref[param]);
            }
            if (mutable_ref) {
                maybe_uninit_.erase(d.lv);   // the callee may write through the borrow
            } else {
                err("`" + d.lv->name + "` is read before it is initialized; assign it first "
                        "(`" + d.lv->name + " = ...`) or give it an initializer at the declaration",
                    d.at);
            }
        }
        deferred_uninit_.resize(mark);
    }

    TypePtr Sema::check_call_impl(Call* c, const TypePtr& expected) {
        Expr* callee = c->callee.get();
        if (!callee) err("call is missing a callee", c);

        if (auto nm = dynamic_cast<Name*>(callee)) {
            if (nm->ident == "alloc" || nm->ident == "realloc" || nm->ident == "free") {
                return check_builtin(c, nm->ident);
            }
            if (nm->ident == "super") return check_super_call(c);
            // A bare sum alternative wins over a user function of the same name when the target type
            // is that sum (SPEC 10.1.1 contextual construction): `Option<int> o = Some(3);` has to
            // construct even when a function `Some(int)` exists, otherwise the alternative is
            // unreachable in that unit (the function still serves every non-sum target).
            if (lookup_local(nm->ident) == nullptr && expected && expected->kind == TypeKind::Sum &&
                c->type_args.empty()) {
                int alt = -1;
                for (size_t k = 0; k < expected->sum_names.size(); ++k) {
                    if (expected->sum_names[k] == nm->ident) {
                        alt = static_cast<int>(k);
                        break;
                    }
                }
                if (alt >= 0) {
                    const auto& fs = expected->sum_fields[static_cast<size_t>(alt)];
                    if (c->args.size() != fs.size()) {
                        err("alternative `" + nm->ident + "` of `" + expected->str() + "` takes " +
                                std::to_string(fs.size()) + " value(s), but " +
                                std::to_string(c->args.size()) + " were given",
                            c);
                    }
                    for (size_t k = 0; k < fs.size(); ++k) {
                        check_assign(c->args[k].get(), fs[k], "sum payload");
                    }
                    nm->kind = "type";
                    nm->ty = expected;
                    c->callee_kind = "sum_ctor";
                    c->sum_ty = expected;
                    c->sum_alt = alt;
                    c->ty = expected;
                    return expected;
                }
            }
            // Unqualified call to one of the enclosing class's own methods
            if (lookup_local(nm->ident) == nullptr && cur_class_) {
                std::vector<const MethodSig*> ms;
                collect_methods(cur_class_, nm->ident, ms);
                if (!ms.empty()) {
                    filter_methods_for_receiver(ms, cur_method_const_);
                    if (ms.empty())
                        err("cannot call a non-const method `" + nm->ident + "` inside a `const` method", c);
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
                    target = instantiate_func(c, nm->ident, cur_, c->type_args);
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
            std::vector<FuncDecl*> cands = collect_funcs_named(nm->ident);

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
                std::string bare_tmpl_unit;
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
                } else if (FuncDecl* gf = [&]() -> FuncDecl* {
                               // The same lookup `instantiate_func` performs for a bare name: this unit
                               // first, otherwise the unique template of that name; two templates of the
                               // same name are ambiguous here exactly as they are for an explicit list.
                               std::string tu, tn;
                               auto own = generic_templates_.find(cur_->name + "." + nm->ident);
                               Decl* d = own != generic_templates_.end() ? own->second : nullptr;
                               std::string unit = cur_->name;
                               if (!d) {
                                   for (const auto& kv : generic_templates_) {
                                       split_qualified(kv.first, tu, tn);
                                       if (tn != nm->ident) continue;
                                       if (d) {
                                           err("generic function `" + nm->ident +
                                                   "` is ambiguous; qualify it with a unit name",
                                               c);
                                       }
                                       d = kv.second;
                                       unit = tu;
                                   }
                               }
                               bare_tmpl_unit = unit;
                               return d ? dynamic_cast<FuncDecl*>(d) : nullptr;
                           }()) {
                    // A bare call of a generic function: the type arguments are inferred from the
                    // argument types (SPEC 3.15.3). Every way inference can fail has its own
                    // diagnostic, so this branch never sends the reader to `<...>` without a reason.
                    const std::string& tmpl_unit = bare_tmpl_unit;
                    std::string where;
                    {
                        auto iu = infos_.find(tmpl_unit);
                        where = ((iu != infos_.end() && iu->second.unit) ? iu->second.unit->filename
                                                                        : tmpl_unit) +
                                ":" + std::to_string(gf->line) + ":" + std::to_string(gf->col);
                    }
                    std::vector<TypeSpecPtr> inferred = infer_func_type_args(
                        c, gf, nm->ident,
                        where.empty() ? std::string()
                                      : "\n  note: the template is declared at " + where);
                    // Qualified with the template's own unit: another unit's same-named generic must
                    // not be instantiated instead of the one that was matched.
                    target = instantiate_func(c,
                                              tmpl_unit.empty() ? nm->ident
                                                                : tmpl_unit + "." + nm->ident,
                                              cur_, inferred);
                    nm->kind = "func";
                    nm->sym = target;
                } else {
                    err("undefined function `" + nm->ident + "`", c);
                }
            }
            }
        } else if (auto m = dynamic_cast<Member*>(callee)) {
            Expr* obj = m->obj.get();
            // `.length()` on a built-in managed sequence (`T[]`): the compiler reads the header.
            // Static arrays are C storage and have no `length()`.
            if (m->name == "length") {
                TypePtr ot = check_expr(obj);
                if (ot && (ot->kind == TypeKind::Array || ot->kind == TypeKind::Slice)) {
                    if (ot->kind == TypeKind::Array) {
                        err("a static array has no `length()`: use `sizeof(a) / sizeof(a[0])`", c);
                    }
                    if (c->args.size() > 1) err("`length()` takes at most one argument (the axis)", c);
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
                    return Type::make_prim("isize");
                }
            }
            // `.ptr()` on a built-in managed sequence: the data pointer (C interop, SPEC 3.3).
            if (m->name == "ptr") {
                TypePtr ot = check_expr(obj);
                if (ot && ot->kind == TypeKind::Slice) {
                    if (!c->args.empty()) err("`ptr()` takes no arguments", c);
                    if (!is_lvalue(obj))
                        err("`ptr()` borrows the buffer; the operand must be a variable or a field (bind the temporary first)", c);
                    if (has_ref_fields(ot->elem) ||
                        ((ot->elem->kind == TypeKind::Struct || ot->elem->kind == TypeKind::Array) &&
                            ot->elem->needs_dtor())) {
                        err("`" + ot->elem->str() +
                                "` is not a trivial element type: `ptr()` is not available, because a raw write "
                                "would bypass the retain/release of the element",
                            c);
                    }
                    c->callee_kind = "slice_ptr";
                    return Type::make_ptr(ot->elem);
                }
            }
            // super.method(...)
            if (auto on = dynamic_cast<Name*>(obj);
                on && on->ident == "super" && lookup_local("super") == nullptr) {
                if (!cur_class_ || !cur_class_->base) err("the current class has no base class", c);
                std::vector<const MethodSig*> ms;
                collect_class_methods(cur_class_->base, m->name, ms);
                if (ms.empty()) err("base class has no method `" + m->name + "`", c);
                filter_methods_for_receiver(ms, cur_method_const_);
                if (ms.empty())
                    err("cannot call a non-const method `" + m->name + "` inside a `const` method", c);
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
                    if (!st && !m->type_args.empty()) {
                        // a generic sum: `Option<int>.Some(3)` — instantiate with the member's type arguments
                        TypeSpec spec;
                        spec.kind = TypeSpec::K::Named;
                        spec.name = nm->ident;
                        spec.line = c->line;
                        spec.col = c->col;
                        for (auto& a : m->type_args) spec.args.push_back(clone_type_spec(*a));
                        st = instantiate_generic(&spec, cur_);
                    }
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
                            err("variant `" + st->str() + "` has no alternative `" + m->name + "`", c);
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
                    filter_extension_visibility(ms, ot, m, c);
                    if (ms.empty()) {
                        TypePtr ft = ot->field_type(m->name);
                        if (ft && is_callable_value(ft)) {
                            TypePtr ct = check_expr(callee);  // resolves the field access and sets the node's type
                            return check_fnptr_args(c, ct);
                        }
                        if (try_promote_method(m, ot, c)) return check_call(c, expected);
                        err("type `" + ot->str() + "` has no method `" + m->name + "`", c);
                    }
                    filter_methods_for_receiver(ms, (ot && (ot->is_const || ot->decl_const)) || root_is_const(obj));
                    if (ms.empty())
                        err(ot && ot->decl_const
                                ? "a non-`const` method `" + m->name +
                                      "` of a `const class` can only be called from its constructors or destructor"
                                : "cannot call a non-const method `" + m->name + "` on a `const` object",
                            c);
                    auto sig = choose_method(ms, raw_args(c->args), "method `" + m->name + "`", c);
                    // `Self` in an interface member denotes the implementor (SPEC 3.15.6); a call through
                    // an interface value has no implementor type, so such a member has no itable slot and
                    // cannot be dispatched dynamically. Inside an interface's own default body the call is
                    // re-resolved when the default is materialized, so it is allowed there.
                    const bool in_iface_body = cur_class_ && cur_class_->kind == TypeKind::Interface;
                    bool sig_self = sig->ret_self;
                    for (bool b : sig->params_self) sig_self = sig_self || b;
                    if (!in_iface_body && sig->owner && sig->owner->kind == TypeKind::Interface &&
                        (sig_self || sig->body_self)) {
                        err("interface method `" + m->name + "` " +
                                (sig_self ? "declares `Self` in its signature"
                                          : "has a default body that reads `this`") +
                                ", so it cannot be called through an interface value: call it on a "
                                "concrete type, or implement it explicitly there",
                            c);
                    }
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
                    filter_extension_visibility(ms, ot, m, c);
                    if (ms.empty()) {
                        TypePtr ft = ot->field_type(m->name);
                        if (ft && is_callable_value(ft)) {
                            TypePtr ct = check_expr(callee);  // resolves the field access and sets the node's type
                            return check_fnptr_args(c, ct);
                        }
                        if (try_promote_method(m, ot, c)) return check_call(c, expected);
                        err("struct `" + ot->str() + "` has no method `" + m->name + "`", c);
                    }
                    filter_methods_for_receiver(ms, (ot && (ot->is_const || ot->decl_const)) || root_is_const(obj));
                    if (ms.empty())
                        err("cannot call a non-const method `" + m->name + "` on a `const` object", c);
                    auto sig = choose_method(ms, raw_args(c->args), "method `" + m->name + "`", c);
                    // A `para` section borrows the suspended caller's frame; writing through a captured
                    // value's non-const method is a data race with the other sections.
                    if (roots_at_outer_value_local(obj) && !sig->is_const) {
                        err("cannot call a non-const method `" + m->name +
                                "` on a value captured by a `para` section: the sections run concurrently, "
                                "so only `const` methods are allowed (copy the value if each section needs its own)",
                            c);
                    }
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
            FuncDecl* chosen = nullptr;
            if (f == ui.funcs.end() || f->second.empty()) {
                // A generic function's template is not an overload entry: it lives in the template
                // table. Qualified calls must reach it, otherwise the "qualify it with a unit name"
                // diagnostic points at a spelling that does not resolve (two stdlib units may both
                // export `unwrap_or`, say).
                auto* gf = dynamic_cast<FuncDecl*>(find_generic_template(uname + "." + m->name, cur_));
                if (!gf) err("unit `" + uname + "` has no function `" + m->name + "`", c);
                if (!gf->is_public) {
                    err("function `" + uname + "." + m->name + "` is not declared public", c);
                }
                std::vector<TypeSpecPtr> targs;
                for (const auto& ta : c->type_args) targs.push_back(clone_type_spec(*ta));
                if (targs.empty()) {
                    std::string where;
                    auto iu = infos_.find(uname);
                    where = ((iu != infos_.end() && iu->second.unit) ? iu->second.unit->filename
                                                                     : uname) +
                            ":" + std::to_string(gf->line) + ":" + std::to_string(gf->col);
                    targs = infer_func_type_args(
                        c, gf, uname + "." + m->name,
                        "\n  note: the template is declared at " + where);
                }
                chosen = instantiate_func(c, uname + "." + m->name, cur_, targs);
            } else {
                chosen = choose_overload(f->second, raw_args(c->args),
                                         "function `" + uname + "." + m->name + "`", c);
                if (!chosen->is_public) {
                    err("function `" + uname + "." + m->name + "` is not declared public", c);
                }
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
                if (!c->named.empty()) {
                    // Named arguments pick the overload whose parameter names they spell out
                    // (`Point(x = 1, y = 2)`); the type checks below then see positional arguments.
                    int hit = -1;
                    int hits = 0;
                    for (size_t i = 0; i < struct_ty->ctors.size(); ++i) {
                        const auto& pn = struct_ty->ctor_param_names.size() > i
                                             ? struct_ty->ctor_param_names[i]
                                             : std::vector<std::string>{};
                        if (named_args_match(pn, c->named)) {
                            if (hit < 0) hit = static_cast<int>(i);
                            hits += 1;
                        }
                    }
                    if (hits == 0) {
                        err("no constructor of `" + struct_ty->str() +
                                "` has parameters matching the named arguments (each parameter name must be "
                                "given exactly once)",
                            c);
                    }
                    if (hits > 1) {
                        err("named arguments match more than one constructor of `" + struct_ty->str() +
                                "`; use positional arguments",
                            c);
                    }
                    reorder_named_args(c->args, c->named, struct_ty->ctor_param_names[hit]);
                }
                if (struct_ty->has_dtor && c != direct_init_expr_ && c != assign_rhs_expr_ &&
                    call_arg_depth_ == 0) {
                    err("a struct with a destructor can only be constructed in a declaration "
                        "initializer, an assignment, or a call argument",
                        c);
                }
                const auto* chosen = choose_ctor(struct_ty->ctors, raw_args(c->args),
                                                 "struct `" + struct_ty->str() + "`", c);
                const size_t ci = static_cast<size_t>(chosen - struct_ty->ctors.data());
                if (ci < struct_ty->ctor_is_public.size() && !struct_ty->ctor_is_public[ci] &&
                    (!cur_ || cur_->name != struct_ty->unit)) {
                    err("constructor is not declared public and cannot be called from another unit", c);
                }
                for (size_t i = 0; i < c->args.size(); ++i) {
                    ArgGuard ag(call_arg_depth_);
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
        if (target->is_builtin) {
            // A compiler-provided compile-time function: fold it to a constant.
            if (!c->args.empty()) {
                err("builtin function `" + target->name + "` does not take arguments yet", c);
            }
            long long v = 0;
            if (!builtin_func(target->name, {}, v)) {
                err("builtin function `" + target->name + "` is not provided by this compiler", c);
            }
            c->callee_kind = "builtin_const";
            c->builtin_value = v;
            return target->rty;
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
                // A mutable `ref` argument may be written by the callee (an out-parameter), so it is a
                // write context: `int x; fill(x);` does not read `x`. A `const ref` argument is a read.
                TypePtr at;
                if (i < target->pty_cref.size() && target->pty_cref[i]) {
                    at = check_expr(a);
                } else {
                    WriteGuard writing(write_ctx_);
                    at = check_expr(a);
                }
                if (roots_at_outer_value_local(a)) {
                    err("cannot pass an enclosing local by `ref` inside a `para` section", a);
                }
                if (!is_lvalue(a) && !is_borrow_expr(a)) {
                    err("argument " + std::to_string(i + 1) + " of `" + target->name +
                            "` must be an addressable variable: a `ref` parameter borrows the caller's storage",
                        a);
                }
                if (!same_type_sig(at, params[i])) {
                    err("argument " + std::to_string(i + 1) + " of `" + target->name + "` must be `" +
                            params[i]->str() + "` (found `" + at->str() +
                            "`): `ref` parameters do not convert",
                        a);
                }
                // A mutable `ref` argument may be written by the callee (an out-parameter), so the
                // variable counts as initialized afterwards; the analysis cannot see through the call.
                if (!(i < target->pty_cref.size() && target->pty_cref[i])) {
                    if (auto* n = dynamic_cast<Name*>(a); n && n->kind == "local") {
                        if (auto lvp = std::get_if<LocalVar*>(&n->sym); lvp && *lvp) {
                            maybe_uninit_.erase(*lvp);
                        }
                    }
                }
                const bool is_cref = i < target->pty_cref.size() && target->pty_cref[i];
                if (!is_cref) {
                    if (ref_target_is_readonly(a)) {
                        err("cannot bind a readonly (`const`) variable to a mutable `ref` parameter", a);
                    }
                    if (auto* bc = dynamic_cast<Call*>(a); bc && bc->by_ref && bc->ref_const) {
                        err("cannot bind a readonly (`const ref`) borrow to a mutable `ref` parameter", a);
                    }
                    if (auto* bi = dynamic_cast<Index*>(a); bi && bi->by_ref && bi->ref_const) {
                        err("cannot bind a readonly (`const ref`) borrow to a mutable `ref` parameter", a);
                    }
                    if (auto* bb = dynamic_cast<Binary*>(a); bb && bb->by_ref && bb->ref_const) {
                        err("cannot bind a readonly (`const ref`) borrow to a mutable `ref` parameter", a);
                    }
                }
                continue;
            }
            ArgGuard ag(call_arg_depth_);
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

    bool Sema::is_user_type(const TypePtr& t) {
        return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Class ||
                     t->kind == TypeKind::Interface);
    }

    TypePtr Sema::decay_param_type(const TypePtr& t) {
        if (!t || t->kind != TypeKind::Array || !t->elem) return t;
        if (t->elem->kind == TypeKind::Class || t->elem->kind == TypeKind::Interface) return t;
        return Type::make_ptr(t->elem);
    }

    // A `[ref x]` capture stores the address in a delegate environment that outlives the frame, so it
    // is an **escaping** borrow: only types whose lifetime needs no bookkeeping may be captured that
    // way (the same family as `&x`). A `ref` parameter is scoped and uses `borrowable_type` instead.
    bool Sema::ref_capturable_type(const TypePtr& t) {
        if (!t) return false;
        switch (t->kind) {
            case TypeKind::Prim:
            case TypeKind::Enum:
            case TypeKind::Ptr: return true;
            case TypeKind::Struct: return !t->is_union && !t->has_dtor && !has_ref_fields(t);
            default: return false;
        }
    }

    bool Sema::borrowable_type(const TypePtr& t) {
        if (!t) return false;
        switch (t->kind) {
            case TypeKind::Prim:
            case TypeKind::Enum:
            case TypeKind::Ptr: return true;
            case TypeKind::Struct:
                // A borrow is scoped (`ref` cannot be stored or rebound), so every non-union value type
                // can be borrowed, including one that owns a resource or holds reference fields: the
                // callee can only reach it through the language's own rules, which keep the counting
                // right. `&` stays refused for such a type — a raw pointer outlives the borrow.
                return !t->is_union;
            default: return false;  // void / string / slice / class / interface / module / null / range / ref
        }
    }

    // Return type resolution for `ref T` / `const ref T` / `T` (shared by methods and free functions).
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
        // A `const` method borrows its receiver read-only, so everything it can hand out must be
        // read-only too; a mutable `ref T` return would be a hole in the deep-readonly rule
        // (`ref int at() const` let a const handle write a field, issue #66). The read-only spelling
        // `const ref T` already exists and is what the method should return.
        const bool ret_const = m->ret_ref && m->ret && m->ret->is_const;
        if (m->is_const && m->ret_ref && !ret_const) {
            err("method `" + m->name +
                    "` is `const`, so it cannot return a mutable borrow; return `const ref " +
                    (m->ret && m->ret->kind == TypeSpec::K::Named ? m->ret->name : std::string("T")) +
                    "` (the caller of a const method cannot write through it)",
                m);
        }
        m->rty = make_borrow_ret(m->ret.get(), m->ret_ref, m, u, "method `" + m->name + "`");
        if (noncopyable_struct(referent_of(m->rty)) && !referent_of(m->rty)->movable()) {
            err("struct `" + referent_of(m->rty)->str() +
                    "` cannot be returned by value: it is not copyable (it is `nocopy`, or it has a "
                    "destructor or contains a value that has one); return a pointer",
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
    void Sema::check_method_args(Call* c, const MethodSig* sig, const std::string& what) {
        if (!c->named.empty()) err("method calls do not support named arguments", c);
        if (c->args.size() != sig->params.size()) {
            err(what + " expects " + std::to_string(sig->params.size()) + " arguments, but " +
                    std::to_string(c->args.size()) + " were given",
                c);
        }
        for (size_t i = 0; i < c->args.size(); ++i) {
            ArgGuard ag(call_arg_depth_);
            const bool by_ref = i < sig->params_ref.size() && sig->params_ref[i];
            if (!by_ref) {
                check_assign(c->args[i].get(), sig->params[i], what + " argument");
                continue;
            }
            // A `ref` parameter borrows the caller's storage (SPEC 3.8, extended to methods in build
            // 270): the argument must be an addressable place of exactly the declared type, a mutable
            // borrow never binds a readonly one, and a mutable borrow may initialize it (out-parameter).
            const bool cref = i < sig->params_cref.size() && sig->params_cref[i];
            Expr* a = c->args[i].get();
            TypePtr at;
            {
                if (cref) {
                    at = check_expr(a);
                } else {
                    WriteGuard writing(write_ctx_);
                    at = check_expr(a);
                }
            }
            if (roots_at_outer_value_local(a)) {
                err("cannot pass an enclosing local by `ref` inside a `para` section", a);
            }
            if (!is_lvalue(a) && !is_borrow_expr(a)) {
                err(what + " argument " + std::to_string(i + 1) +
                        " must be an addressable variable: a `ref` parameter borrows the caller's storage",
                    a);
            }
            if (!same_type_sig(at, sig->params[i])) {
                err(what + " argument " + std::to_string(i + 1) + " must be `" +
                        sig->params[i]->str() + "` (found `" + at->str() +
                        "`): `ref` parameters do not convert",
                    a);
            }
            if (!cref) {
                if (ref_target_is_readonly(a)) {
                    err("cannot bind a readonly (`const`) variable to a mutable `ref` parameter", a);
                }
                bool readonly_borrow = false;
                if (auto* bc = dynamic_cast<Call*>(a); bc && bc->by_ref && bc->ref_const) readonly_borrow = true;
                if (auto* bi = dynamic_cast<Index*>(a); bi && bi->by_ref && bi->ref_const) readonly_borrow = true;
                if (auto* bb = dynamic_cast<Binary*>(a); bb && bb->by_ref && bb->ref_const) readonly_borrow = true;
                if (readonly_borrow) {
                    err("cannot bind a readonly (`const ref`) borrow to a mutable `ref` parameter", a);
                }
            }
        }
    }

    TypePtr Sema::check_super_call(Call* c) {
        // A base-class constructor call is a prologue item, not a statement (SPEC 3.12): the base runs
        // before the members, which a body statement cannot express.
        err("a base-class constructor call is not a statement: write it in the constructor's initializer "
            "list — `Derived(args) : Base(args) { ... }` (SPEC 3.12)",
            c);
    }

    [[noreturn]] void Sema::err_slice_elem_align(const TypePtr& elem, const Node* node) const {
        err("type `" + elem->str() + "` has align(" + std::to_string(type_align(elem)) +
                ") greater than 8: the data area of a dynamic array follows a 24-byte header, so elements can be aligned to at most 8 bytes; "
                "use a fixed-size array or a pointer instead, or lower `align(N)`",
            node);
    }

    // `x is Alt(a, b)` / `x is Cat d` (SPEC 10.1.2): a runtime discrimination test. A sum value tests
    // the alternative (payload bindings follow the declaration); a class/interface value tests the
    // dynamic type (an optional binding is the downcast). Anything else has no runtime discrimination.
    TypePtr Sema::check_is(IsExpr* e) {
        TypePtr st = check_expr(e->obj.get());
        if (!st) err("`is` needs a value on the left", e);
        if (st->kind == TypeKind::Sum) {
            int idx = -1;
            for (size_t k = 0; k < st->sum_names.size(); ++k) {
                if (st->sum_names[k] == e->name) { idx = static_cast<int>(k); break; }
            }
            if (idx < 0) err("variant `" + st->str() + "` has no alternative `" + e->name + "`", e);
            const auto& fs = st->sum_fields[static_cast<size_t>(idx)];
            if (!e->binds.empty() && e->binds.size() != fs.size()) {
                err("alternative `" + e->name + "` carries " + std::to_string(fs.size()) +
                        " value(s), so `is` must bind " + std::to_string(fs.size()) +
                        " name(s) (or none)",
                    e);
            }
            e->is_sum = true;
            e->sum_ty = st;
            e->alt = idx;
            e->bind_types = fs;
            e->ty = t_bool();
            return e->ty;
        }
        if (st->kind == TypeKind::Class || st->kind == TypeKind::Interface) {
            TypePtr t = find_type_in_expr(e->name, e);
            if (!t || t->kind != TypeKind::Class) {
                err("`" + e->name + "` is not a class, so it cannot be tested with `is`", e);
            }
            if (!implicitly_convertible(t, st)) {
                err("class `" + t->str() + "` is not a subtype of `" + st->str() + "`", e);
            }
            if (e->binds.size() > 1) {
                err("`is` binds at most one name when testing a class", e);
            }
            e->test_ty = t;
            if (!e->binds.empty()) e->bind_types = {t};
            e->ty = t_bool();
            return e->ty;
        }
        err("type `" + st->str() +
                "` has no runtime discrimination, so `is` would be a constant; test the value directly",
            e);
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
        bool ok0 = p0 && same_type_sig(p0, t_int());
        bool ok1 = p1 && p1->kind == TypeKind::Ptr && p1->elem &&
                   p1->elem->kind == TypeKind::Ptr && is_char(p1->elem->elem);
        if (!ok0 || !ok1) {
            err("`main` parameters must be `(int argc, char** argv)`", main);
        }
    }
}  // namespace muc_detail
