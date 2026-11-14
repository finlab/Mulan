// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds class / interface checking (part 2) (definitions of Sema members).
#include "sema_impl.hpp"

namespace muc_detail {

    void Sema::check_aggregate(InitAggregate* init, const TypePtr& ty, const std::string& what) {
        auto& items = init->items;

        if (ty->kind == TypeKind::Struct && ty->is_union) {
            // union: exactly one named member is allowed; positional initialization is not provided (explicit over implicit)
            if (items.size() != 1 || items[0].first.empty()) {
                err("union `" + ty->str() +
                        "` must be initialized with exactly one named member, e.g. `{ member = value }`",
                    init);
            }
            const Field* target = nullptr;
            for (auto& f : ty->fields) {
                if (f.name == items[0].first) target = &f;
            }
            if (!target) {
                err("union `" + ty->str() + "` has no member `" + items[0].first + "`", init);
            }
            check_init_into(target->type, items[0].second.get(), what, init);
            return;
        }

        if (ty->kind == TypeKind::Struct && !ty->ctors.empty()) {
            err("struct `" + ty->str() + "` has a constructor and cannot use aggregate initialization", init);
        }

        if (ty->kind == TypeKind::Struct) {
            const auto& fields = ty->fields;
            // A cross-unit aggregate initialization requires all fields to be public
            if (cur_ && cur_->name != ty->unit) {
                for (auto& f : fields) {
                    if (!f.is_public) {
                        err("field `" + f.name + "` of struct `" + ty->str() +
                                "` is not declared public and cannot be aggregate-initialized from another unit",
                            init);
                    }
                }
            }
            bool any_positional = false;
            bool any_named = false;
            for (auto& item : items) {
                if (item.first.empty()) {
                    any_positional = true;
                } else {
                    any_named = true;
                }
            }
            if (any_positional && any_named) err("cannot mix named and positional initializers", init);
            if (!any_named) {
                if (items.size() != fields.size()) {
                    err("struct `" + ty->str() + "` expects " + std::to_string(fields.size()) +
                            " initializers, but " + std::to_string(items.size()) + " were given",
                        init);
                }
                for (size_t i = 0; i < items.size(); ++i) {
                    check_init_into(fields[i].type, items[i].second.get(), what, init);
                }
                return;
            }

            std::unordered_map<std::string, size_t> fmap;
            for (size_t i = 0; i < fields.size(); ++i) fmap.emplace(fields[i].name, i);

            std::unordered_set<std::string> seen;
            for (auto& [n, sub] : items) {
                auto it = fmap.find(n);
                if (it == fmap.end()) {
                    err("struct `" + ty->str() + "` has no field `" + n + "`", init);
                }
                if (!seen.insert(n).second) err("field `" + n + "` is initialized more than once", init);
                check_init_into(fields[it->second].type, sub.get(), what, init);
            }

            std::string missing;
            for (auto& f : fields) {
                if (seen.count(f.name)) continue;
                if (!missing.empty()) missing += ", ";
                missing += f.name;
            }
            if (!missing.empty()) {
                err("struct `" + ty->str() + "` is missing initializers for: " + missing, init);
            }
            return;
        }

        if (ty->kind == TypeKind::Array) {
            for (auto& item : items) {
                if (!item.first.empty()) err("array initializers cannot use named fields", init);
            }
            // Range fill: `{1, 2, 5..9, 10}` (endpoints must be compile-time constants, integer element types only)
            size_t total = 0;
            for (auto& item : items) {
                if (auto rg = dynamic_cast<InitRange*>(item.second.get())) {
                    if (!is_integer(ty->elem)) {
                        err("range fill requires an integer element type (found `" + ty->elem->str() + "`)", rg);
                    }
                    long long lo = 0, hi = 0;
                    if (!eval_const_int(rg->lo.get(), lo) || !eval_const_int(rg->hi.get(), hi)) {
                        err("range endpoints in an array initializer must be compile-time integer constants", rg);
                    }
                    rg->lo_val = lo;
                    rg->count = hi > lo ? hi - lo : 0;  // an empty range (lo >= hi) is valid = 0 elements
                    total += static_cast<size_t>(rg->count);
                    continue;
                }
                total += 1;
            }
            if (total > ty->array_size) {
                err("array of length " + std::to_string(ty->array_size) + " was given " +
                        std::to_string(total) + " initializers",
                    init);
            }
            for (auto& item : items) {
                if (dynamic_cast<InitRange*>(item.second.get())) continue;
                check_init_into(ty->elem, item.second.get(), what, init);
            }
            return;
        }

        if (ty->kind == TypeKind::Slice) {
            if (type_align(ty->elem) > 8) err_slice_elem_align(ty->elem, init);
            if (ty->rank != 1) {
                err("dynamic rectangular array `" + ty->str() + "` must be created with `new T[m,n]`", init);
            }
            for (auto& item : items) {
                if (!item.first.empty()) err("slice initializers cannot use named fields", init);
                if (dynamic_cast<InitRange*>(item.second.get())) {
                    err("range fill (`a..b`) is only allowed in fixed-size array initializers", item.second.get());
                }
            }
            for (auto& item : items) check_init_into(ty->elem, item.second.get(), what, init);
            return;
        }

        err("type " + ty->str() + " cannot be brace-initialized", init);
    }

    bool Sema::simple_seq(Expr* e) {
        if (dynamic_cast<Name*>(e)) return true;
        if (auto m = dynamic_cast<Member*>(e)) return simple_seq(m->obj.get());
        return false;
    }

    bool Sema::root_is_const(Expr* e) {
        if (!e) return false;
        if (auto n = dynamic_cast<Name*>(e)) {
            if (auto lv = std::get_if<LocalVar*>(&n->sym)) return *lv && (*lv)->is_const;
            if (auto gv = std::get_if<GlobalVar*>(&n->sym)) return *gv && (*gv)->is_const;
            return false;
        }
        if (auto m = dynamic_cast<Member*>(e)) return root_is_const(m->obj.get());
        if (auto ix = dynamic_cast<Index*>(e)) return root_is_const(ix->base.get());
        return false;
    }

    void Sema::check_writable(Expr* t) {
        if (auto n = dynamic_cast<Name*>(t)) {
            if (n->ident == "this") err("cannot assign to `this`", t);
            if (auto lv = std::get_if<LocalVar*>(&n->sym); lv && *lv && (*lv)->is_ref_param) {
                // Scalar/enum/pointer borrow: assignment writes through to the caller's variable (struct borrows still use field assignment, see SPEC "no rebinding")
                TypePtr rt = (*lv)->rty;
                const bool scalar = rt && (rt->kind == TypeKind::Prim ||
                                           rt->kind == TypeKind::Enum ||
                                           rt->kind == TypeKind::Ptr);
                if (!scalar) {
                    err("cannot rebind a `ref` parameter: it borrows the caller's variable", t);
                }
            }
            if (root_is_const(t)) err("`" + n->ident + "` is a `const` binding and cannot be assigned", t);
            return;
        }
        if (dynamic_cast<Member*>(t)) {
            if (root_is_const(t)) err("cannot modify a member of a `const` binding", t);
            return;
        }
        if (auto ix = dynamic_cast<Index*>(t)) {
            TypePtr bt = ix->base->ty;
            if (bt && bt->kind == TypeKind::Slice && bt->elem && bt->elem->is_const)
                err("cannot write through a `const` slice", t);
            if (bt && bt->kind == TypeKind::Ptr && bt->elem && bt->elem->is_const)
                err("cannot write through a `const` pointer", t);
            if (root_is_const(ix->base.get())) err("cannot modify an element of a `const` binding", t);
            return;
        }
        if (auto u = dynamic_cast<Unary*>(t)) {
            if (u->op == "*") {
                TypePtr ot = u->operand->ty;
                if (ot && ot->kind == TypeKind::Ptr && ot->elem && ot->elem->is_const)
                    err("cannot write through a `const` pointer", t);
            }
        }
    }

    bool Sema::is_weak_field_access(Expr* e) {
        if (auto n = dynamic_cast<Name*>(e)) {
            if (n->kind == "field" && n->implicit_this && cur_class_ && n->field_index >= 0 &&
                n->field_index < static_cast<int>(cur_class_->fields.size())) {
                return cur_class_->fields[n->field_index].weak;
            }
            return false;
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            TypePtr ot = m->obj->ty;
            if (ot && ot->kind == TypeKind::Class && m->field_index >= 0 &&
                m->field_index < static_cast<int>(ot->fields.size())) {
                return ot->fields[m->field_index].weak;
            }
        }
        return false;
    }

    void Sema::check_assign_stmt(AssignStmt* s) {
        if (!is_lvalue(s->target.get())) {
            // The result of a `ref T f()` call is also a writable lvalue (write-through borrow)
            if (auto* c = dynamic_cast<Call*>(s->target.get())) {
                check_expr(c);
                if (c->ref_const) err("cannot assign through a readonly (`ref const`) return value", s);
                if (!c->by_ref) err("left side of an assignment must be assignable", s);
            } else {
                err("left side of an assignment must be assignable", s);
            }
        }

        if (auto ix = dynamic_cast<Index*>(s->target.get())) {
            TypePtr bt = check_expr(ix->base.get());
            if (is_user_type(bt)) {
                for (auto& ie : ix->indices) {
                    TypePtr it = check_expr(ie.get());
                    if (!is_integer(it)) err("index must be an integer", ie.get());
                }
                // Only a borrow-returning subscript is writable (`operator[]=` was removed).
                check_expr(ix);
                if (!ix->by_ref) {
                    err("cannot assign through a subscript: `operator[]` must return `ref T`",
                        s->target.get());
                }
                if (ix->ref_const)
                    err("cannot assign through a readonly (`ref const`) subscript", s->target.get());
                check_writable(s->target.get());
                // no return: falls through to the generic assignment/compound-assignment check below
            }
        }

        TypePtr tt = check_expr(s->target.get());
        if (roots_at_outer_value_local(s->target.get())) {
            err("cannot assign to an enclosing local inside a `para` section: pass the result through a "
                "shared object (a class field or a channel)",
                s->target.get());
        }
        if (auto m = dynamic_cast<Member*>(s->target.get()); m && m->kind == "enum_const") {
            err("enum members are constants and cannot be assigned", s);
        }
        if (auto m = dynamic_cast<Member*>(s->target.get()); m && m->kind == "const_member") {
            err("const members are constants and cannot be assigned", s);
        }
        if (auto n = dynamic_cast<Name*>(s->target.get()); n && n->kind == "const_member") {
            err("const members are constants and cannot be assigned", s);
        }
        check_writable(s->target.get());
        if (s->op == "=") {
            check_assign(s->value.get(), tt, "assignment");
            return;
        }
        if (is_enum_t(tt)) {
            err("enum `" + tt->str() + "` does not support `" + s->op +
                    "` (write `x = (x as int " + s->op.substr(0, s->op.size() - 1) +
                    " y) as " + tt->name + "`）",
                s);
        }
        const std::string op = s->op.empty() ? std::string() : s->op.substr(0, s->op.size() - 1);
        TypePtr vt = check_expr(s->value.get(), tt);
        if (op == "<<" || op == ">>") {
            if (!is_integer(tt) || !is_integer(vt)) err("`both sides of `" + s->op + "` must be integers", s);
            return;
        }
        if (op == "&" || op == "|" || op == "^") {
            if (!is_integer(tt) || !is_integer(vt)) err("`both sides of `" + s->op + "` must be integers", s);
            return;
        }
        if ((op == "+" || op == "-") && tt->kind == TypeKind::Ptr) {
            if (tt->elem && tt->elem->kind == TypeKind::Void) err("pointer arithmetic is not allowed on `void*`", s);
            if (!is_integer(vt)) err("pointer arithmetic requires an integer operand", s);
            return;
        }
        if (!(is_numeric(tt) && is_numeric(vt))) err("`" + s->op + "` requires numeric operands", s);
    }

    void Sema::check_return(ReturnStmt* s) {
        if (!para_stack_.empty() && lambda_stack_.size() == para_stack_.back().lambda_base) {
            err("`return` cannot leave a `para` section", s);
        }
        if (in_dtor_) err("`return` is not allowed in a destructor (fields and the base class are destroyed at the end of the function)", s);
        // Inside a lambda the return type is inferred (3.16): collect the types and unify after the body.
        if (!lambda_stack_.empty()) {
            LambdaCtx& ctx = lambda_stack_.back();
            if (!s->value) {
                ctx.bare.push_back(s);
                return;
            }
            TypePtr t = check_expr(s->value.get());
            ctx.rets.emplace_back(s, t);
            return;
        }
        if (!cur_ret_) err("`return` can only appear in a function or method", s);
        TypePtr rt = cur_ret_;
        if (!s->value) {
            if (!is_void(rt)) {
                err("`" + cur_name_ + "` must return a value of type " + (rt ? rt->str() : "void"), s);
            }
            return;
        }
        if (is_void(rt)) err("`void` function or method `" + cur_name_ + "` cannot return a value", s);
        if (is_borrow_type(rt)) {
            TypePtr base = rt->elem;
            TypePtr vt = check_expr(s->value.get());
            if (!same_type(vt, base)) {
                err("return value must be `" + base->str() + "` (found `" +
                        (vt ? vt->str() : "void") + "`) for a `ref` return type",
                    s->value.get());
            }
            if (!is_lvalue(s->value.get())) {
                if (!is_borrow_expr(s->value.get())) {
                    err("a `ref` return value must borrow a variable (a temporary cannot be returned by reference)",
                        s->value.get());
                }
            }
            if (!borrow_root_ok(s->value.get())) {
                err("returned reference must borrow a `ref` parameter, `this`, a global, or pointer/slice-backed storage",
                    s->value.get());
            }
            if (!rt->is_const && root_is_const(s->value.get())) {
                err("cannot return a mutable `ref` to a readonly (`const`) location; declare the return type as `ref const`",
                    s->value.get());
            }
            return;
        }
        in_return_ = true;
        check_assign(s->value.get(), rt, "return value");
        in_return_ = false;
    }

    // Whether the origin of a returned borrow is stable: ref parameter / this / global / a path through a pointer or slice (heap storage) / forwarded borrow.
    bool Sema::borrow_root_ok(Expr* e) {
        bool heap = false;
        while (e) {
            if (auto* m = dynamic_cast<Member*>(e)) {
                TypePtr ot = m->obj->ty;
                if (ot && (ot->kind == TypeKind::Ptr || ot->kind == TypeKind::Slice)) heap = true;
                e = m->obj.get();
                continue;
            }
            if (auto* ix = dynamic_cast<Index*>(e)) {
                TypePtr bt = ix->base->ty;
                if (bt && (bt->kind == TypeKind::Ptr || bt->kind == TypeKind::Slice)) heap = true;
                if (ix->by_ref) heap = true;  // the borrowed element itself points into stable storage
                e = ix->base.get();
                continue;
            }
            if (auto* u = dynamic_cast<Unary*>(e)) {
                if (u->op == "*") { heap = true; e = u->operand.get(); continue; }
                break;
            }
            break;
        }
        if (heap) return true;
        if (!e) return false;
        if (auto* n = dynamic_cast<Name*>(e)) {
            if (n->ident == "this") return true;
            if (n->kind == "field" && n->implicit_this) return true;  // implicit this field
            if (auto* lv = std::get_if<LocalVar*>(&n->sym); lv && *lv) return (*lv)->by_ref;
            if (std::get_if<GlobalVar*>(&n->sym)) return true;
            return false;
        }
        if (auto* c = dynamic_cast<Call*>(e)) return c->by_ref;
        return false;
    }

    bool Sema::is_borrow_expr(Expr* e) {
        if (auto* c = dynamic_cast<Call*>(e)) return c->by_ref;
        if (auto* ix = dynamic_cast<Index*>(e)) return ix->by_ref;
        return false;
    }

    void Sema::check_cond(Expr* e, const std::string& what) {
        TypePtr t = check_expr(e);
        if (!(is_prim(t) && t->name == "bool")) {
            err(what + " must be `bool`, found " + (t ? t->str() : "void"), e);
        }
    }

    long long Sema::checked_int_pattern(long long v, const TypePtr& st, Expr* e) {
        if (!promote_int_literal(v, st)) {
            err("switch case value " + std::to_string(v) + " is out of range for " + st->str(), e);
        }
        return v;
    }

    long long Sema::pattern_value(Expr* e, const TypePtr& st) {
        if (st->kind == TypeKind::Enum) {
            // The member name is resolved directly against the matched enum (`Red` needs no prefix)
            if (auto* n = dynamic_cast<Name*>(e)) {
                if (EnumConst* ec = find_enum_member(st, n->ident)) {
                    n->kind = "enum_const";
                    n->sym = ec;
                    n->ty = st;
                    return ec->value;
                }
                err("enum `" + st->str() + "` has no member `" + n->ident + "`", e);
            }
            if (auto* m = dynamic_cast<Member*>(e)) {
                TypePtr pt = check_expr(e);
                auto* ec = std::get_if<EnumConst*>(&m->sym);
                if (!ec || !*ec || !pt || !same_type(pt, st)) {
                    err("switch case type does not match the value being switched on: expected " + st->str(), e);
                }
                return (*ec)->value;
            }
            err("cases of an enum switch must be members of " + st->str(), e);
        }

        if (auto* il = dynamic_cast<IntLit*>(e)) return checked_int_pattern(il->value, st, e);
        if (auto* cl = dynamic_cast<CharLit*>(e)) return checked_int_pattern(cl->value, st, e);
        if (auto* u = dynamic_cast<Unary*>(e); u && (u->op == "-" || u->op == "+")) {
            if (auto* il = dynamic_cast<IntLit*>(u->operand.get())) {
                const long long v = u->op == "-" ? -il->value : il->value;
                return checked_int_pattern(v, st, e);
            }
        }
        if (auto* n = dynamic_cast<Name*>(e)) {
            check_expr(e);  // resolve as a variable / enum member to give a more precise error
            if (std::get_if<EnumConst*>(&n->sym)) {
                err("pattern `" + n->ident + "` is an enum member and cannot match " + st->str(), e);
            }
            err("switch case pattern must be a compile-time constant: `" + n->ident +
                    "` is a variable; use a literal or an enum member",
                e);
        }
        err("switch case pattern must be a compile-time constant (integer literal, character literal, or enum member)", e);
    }

    std::vector<std::pair<std::string, long long>> Sema::enum_members_of(const TypePtr& et) {
        std::vector<std::pair<std::string, long long>> out;
        auto it = infos_.find(et->unit);
        if (it == infos_.end() || !it->second.unit) return out;
        for (auto& d : it->second.unit->decls) {
            auto* ed = dynamic_cast<EnumDecl*>(d.get());
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
                out.emplace_back(m, value);
            }
        }
        return out;
    }

    void Sema::check_switch(SwitchStmt* s) {
        ScopeGuard guard(*this);
        TypePtr st = check_expr(s->scrut.get());
        if (!st) err("switch is missing the value to match", s);
        const bool enum_sw = st->kind == TypeKind::Enum;
        if (!enum_sw && !(is_prim(st) && is_integer(st))) {
            err("switch value must be an integer or enum (found " + st->str() + ")", s->scrut.get());
        }
        s->sty = st;

        // Overlap detection: merge [lo, hi) intervals, top = covers up to the maximum value of the type
        struct Iv {
            long long lo = 0;
            long long hi = 0;
            bool top = false;
            const Node* node = nullptr;
        };
        std::vector<Iv> ivs;

        for (auto& up : s->arms) {
            SwitchArm* arm = up.get();
            if (arm->is_else) {
                if (s->has_else) err("switch can have at most one `else` branch", s);
                s->has_else = true;
            }
            if (arm->pats.empty() && !arm->is_else) {
                err("switch case is missing a pattern", s);
            }
            for (auto& p : arm->pats) {
                p.vlo = pattern_value(p.lo.get(), st);
                Iv iv;
                iv.lo = p.vlo;
                iv.node = p.lo.get();
                if (p.is_range) {
                    if (enum_sw) {
                        err("enum switches do not support range cases (enum members have no numeric order); list each member explicitly",
                            p.lo.get());
                    }
                    p.vhi = pattern_value(p.hi.get(), st);
                    if (p.inclusive) {
                        if (p.vhi == LLONG_MAX) {
                            err("switch range upper bound is too large for an inclusive range (`..=`)", p.hi.get());
                        }
                        ++p.vhi;  // normalize to the half-open interval [vlo, vhi)
                    }
                    if (p.vlo >= p.vhi) {
                        err("switch range `" + std::to_string(p.vlo) + (p.inclusive ? "..=" : "..") +
                                std::to_string(p.inclusive ? p.vhi - 1 : p.vhi) + "` is empty",
                            p.lo.get());
                    }
                    iv.hi = p.vhi;
                } else {
                    if (p.vlo == LLONG_MAX) {
                        iv.hi = p.vlo;
                        iv.top = true;
                        p.vhi = p.vlo;  // internal use only; +1 would overflow
                    } else {
                        iv.hi = p.vlo + 1;
                        p.vhi = p.vlo + 1;  // internal use only (p.vlo is the only matching value)
                    }
                }
                ivs.push_back(iv);
            }
            check_block(arm->body.get());
        }

        std::sort(ivs.begin(), ivs.end(),
                  [](const Iv& a, const Iv& b) { return a.lo < b.lo; });
        for (size_t i = 1; i < ivs.size(); ++i) {
            const Iv& prev = ivs[i - 1];
            const Iv& cur = ivs[i];
            if (prev.top || cur.lo < prev.hi) {
                err("overlapping switch cases: value " + std::to_string(cur.lo) +
                        " is already covered by an earlier case (ranges are half-open; `a..b` excludes b)",
                    cur.node);
            }
        }

        if (enum_sw) {
            auto members = enum_members_of(st);
            std::string missing;
            for (auto& [name, v] : members) {
                bool covered = false;
                for (auto& up : s->arms) {
                    for (auto& p : up->pats) {
                        if (!p.is_range && p.vlo == v) covered = true;
                    }
                }
                if (!covered) {
                    if (!missing.empty()) missing += ", ";
                    missing += name;
                }
            }
            if (!s->has_else) {
                if (!missing.empty()) {
                    err("switch does not cover all members of enum " + st->str() + " (missing " + missing +
                            "); add the missing cases or write `else:`",
                        s);
                }
                s->enum_exhaustive = true;
            } else if (missing.empty()) {
                warn("`else` branch is unreachable: all members of enum `" + st->str() + "` are already covered", s);
            }
        }
    }

    // Cursor form (6.4.2): the contract is declared in `libs/std/interfaces.mu` (std.interfaces.IIterator<T>);
    // the compiler checks it structurally (no vtable, no boxing).
    void Sema::resolve_cursor_shape(ForInStmt* s, const TypePtr& st) {
        const MethodSig* it = nullptr;
        for (auto& m : st->methods) {
            if (m.name == "iterator" && m.params.empty()) {
                it = &m;
                break;
            }
        }
        if (!it) err("`iterator()` must take no parameters", s->seq.get());
        TypePtr cursor = it->ret;
        // A struct cursor is iterated statically (zero cost); an interface cursor (`IIterator<T>`,
        // the shape `IEnumerable<T>` returns) is iterated through the itable, which is the dynamic
        // channel. Anything else has no known `next()`/`current()`.
        if (!cursor || (cursor->kind != TypeKind::Struct && cursor->kind != TypeKind::Interface)) {
            err("`iterator()` must return a struct or `IIterator<T>` cursor (got " +
                    (cursor ? cursor->str() : std::string("void")) + "); see std.interfaces.IIterator",
                s->seq.get());
        }
        const MethodSig* nxt = nullptr;
        const MethodSig* cur = nullptr;
        for (auto& m : cursor->methods) {
            if (m.name == "next" && m.params.empty()) nxt = &m;
            if (m.name == "current" && m.params.empty()) cur = &m;
        }
        if (!nxt || !(is_prim(nxt->ret) && nxt->ret->name == "bool")) {
            err("the cursor type `" + cursor->str() + "` must provide `bool next()` (std.interfaces.IIterator)",
                s->seq.get());
        }
        if (!cur) {
            err("the cursor type `" + cursor->str() + "` must provide `current()` (std.interfaces.IIterator)",
                s->seq.get());
        }
        if (s->by_ref) err("`for ref` is not supported for cursor sources (SPEC 6.4.2)", s);
        s->loop_kind = "cursor";
        s->it_ty = cursor;
        s->iter_sig = it;
        s->next_sig = nxt;
        s->cur_sig = cur;
        s->elem_ty = cur->ret;
    }

    void Sema::resolve_index_shape(ForInStmt* s, const TypePtr& st) {
        TypePtr us = Type::make_prim("usize");
        const MethodSig* len = find_method(st, "length", 0);
        if (!len) {
            err("`for-in` cannot iterate " + st->str() +
                    ": the indexed form requires a method `length() -> usize` (none found)",
                s->seq.get());
        }
        if (!same_type(len->ret, us)) {
            err("`" + st->str() + "::length()` must return `usize` (found `" +
                    (len->ret ? len->ret->str() : "void") + "`）",
                s->seq.get());
        }

        FuncDecl* picked = nullptr;
        std::string saw_index;
        // A member `operator[](usize)` on the struct takes precedence (borrowed receiver, no copy/RC).
        const MethodSig* picked_m = nullptr;
        for (const auto& m : st->methods) {
            if (!m.is_operator || m.name != "operator[]" || m.params.size() != 1) continue;
            if (!decl_visible(m.owner ? m.owner->unit : std::string(), m.is_public)) continue;
            if (!same_type(m.params[0], us)) {
                if (saw_index.empty() && m.params[0]) saw_index = m.params[0]->str();
                continue;
            }
            if (picked_m) {
                err("`" + st->str() + "` has more than one `operator[](usize)` overload; `for-in` cannot choose one",
                    s->seq.get());
            }
            picked_m = &m;
        }
        if (picked_m) {
            s->loop_kind = "index";
            s->seq_ty = st;
            s->elem_ty = referent_of(picked_m->ret);
            s->len_sig = len;
            s->idx_method = picked_m;
            return;
        }
        for (auto* c : collect_operators("[]", {st, us})) {
            if (c->pty.size() != 2) continue;
            bool recv_ok = same_type(c->pty[0], st) ||
                           (c->pty[0] && c->pty[0]->kind == TypeKind::Ptr &&
                            same_type(c->pty[0]->elem, st));
            if (!recv_ok) continue;
            if (!same_type(c->pty[1], us)) {
                if (saw_index.empty() && c->pty[1]) saw_index = c->pty[1]->str();
                continue;
            }
            if (picked) {
                err("`" + st->str() + "` has more than one `operator[](`" + st->str() +
                        ", usize)` overload; `for-in` cannot choose one",
                    s->seq.get());
            }
            picked = c;
        }
        if (!picked) {
            std::string msg = "`for-in` cannot iterate " + st->str() + ": the indexed form also requires `operator[](" +
                              st->str() + ", usize) -> T`";
            if (!saw_index.empty()) {
                msg += "(the `operator[]` found takes index type `" + saw_index + "`; `usize` is required)";
            }
            err(msg, s->seq.get());
        }

        s->loop_kind = "index";
        s->seq_ty = st;
        s->elem_ty = referent_of(picked->rty);
        s->len_sig = len;
        s->idx_fn = picked;
    }

    void Sema::check_for(ForInStmt* s) {
        ScopeGuard guard(*this);

        if (auto r = dynamic_cast<RangeExpr*>(s->seq.get())) {
            TypePtr lt = check_expr(r->lo.get());
            TypePtr ht = check_expr(r->hi.get());
            if (is_enum_t(lt) || is_enum_t(ht)) {
                err("range endpoints cannot be enum values (write `x as int`)", r);
            }
            if (!is_integer(lt) || !is_integer(ht)) err("range bounds must be integers", r);
            TypePtr vt = usual_arith(lt, ht);
            if (!vt) vt = t_int();
            if (!is_integer(vt)) err("the loop variable of a range must be an integer", r);
            s->loop_kind = "range";
            s->seq_ty = t_range();
            s->elem_ty = vt;
            if (s->step) {
                TypePtr st = check_expr(s->step.get());
                if (!is_integer(st)) err("`step` must be an integer", s->step.get());
                long long v = 0;
                if (!eval_const_int(s->step.get(), v)) {
                    err("`step` must be a compile-time integer constant", s->step.get());
                }
                if (v == 0) err("`step` must not be zero", s->step.get());
                s->step_val = v;
            }
        } else {
            TypePtr st = check_expr(s->seq.get());
            if (!st) err("`for-in` is missing a sequence", s);
            if (st->kind == TypeKind::Array) {
                err("a static array is C storage and does not support `for-in`: use an index loop "
                    "(`sizeof(a) / sizeof(a[0])` gives the count), or copy it into a managed sequence",
                    s->seq.get());
            } else if (st->kind == TypeKind::Slice) {
                if (st->rank > 1) {
                    err("`for-in` does not support multidimensional arrays `" + st->str() +
                            "`: use an index loop (get each dimension's length with `len(a, k)`)",
                        s->seq.get());
                }
                s->loop_kind = "slice";
                s->elem_ty = st->elem;
            } else if (st->kind == TypeKind::Struct || st->kind == TypeKind::Class) {
                if (has_method_named(st, "iterator")) {
                    resolve_cursor_shape(s, st);
                } else {
                    resolve_index_shape(s, st);
                }
            } else {
                err("`for-in` cannot iterate " + st->str() +
                        " (supported: range, array, slice; user types must provide `length() -> usize` and "
                        "`operator[](usize) -> T`）",
                    s->seq.get());
            }
            s->seq_ty = st;
        }

        if (s->by_ref && s->loop_kind == "string") {
            err("`string` is immutable and cannot be iterated with `for ref`", s);
        }
        if (s->by_ref && s->loop_kind == "index" &&
            !(s->idx_fn && is_borrow_type(s->idx_fn->rty))) {
            err("user types do not support `for ref` unless `operator[]` returns `ref T`; use `for i, x` with `c[i] = ...` instead",
                s->seq.get());
        }
        if (s->by_ref && s->loop_kind == "index" && s->idx_fn && is_borrow_type(s->idx_fn->rty) &&
            s->idx_fn->rty->is_const) {
            err("`for ref` requires a mutable borrow: `operator[]` returns `ref const T`", s->seq.get());
        }
        if (s->by_ref && s->loop_kind == "range") {
            err("`for ref` cannot be used on a range loop", s);
        }
        if (s->by_ref && s->elem_ty && is_reference_type(s->elem_ty)) {
            err("`for ref` is not supported for class or interface elements (bind the handle by value)", s);
        }
        if (s->by_ref && s->seq_ty && s->seq_ty->kind == TypeKind::Slice && s->seq_ty->elem &&
            s->seq_ty->elem->is_const) {
            err("`for ref` cannot be used on a `const` slice", s->seq.get());
        }
        if (s->by_ref && root_is_const(s->seq.get())) {
            err("`for ref` cannot be used on a `const` binding", s->seq.get());
        }
        if (s->by_ref && !simple_seq(s->seq.get())) {
            err("the sequence of `for ref` must be a variable or member access (an lvalue)", s->seq.get());
        }

        TypePtr vt = s->elem_ty;
        auto lv = std::make_unique<LocalVar>();
        lv->name = s->var;
        lv->rty = vt;
        lv->by_ref = s->by_ref;
        lv->line = s->line;
        lv->col = s->col;
        declare_local(lv.get());
        s->var_ty = vt;
        s->lvar = std::move(lv);

        if (s->has_idx) {
            TypePtr us = Type::make_prim("usize");
            auto ilv = std::make_unique<LocalVar>();
            ilv->name = s->idx_name;
            ilv->rty = us;
            ilv->is_const = true;
            ilv->line = s->line;
            ilv->col = s->col;
            declare_local(ilv.get());
            s->idx_ty = us;
            s->idx_lvar = std::move(ilv);
        }

        ++loop_depth_;
        const bool saved_pf_body = in_para_for_body_;
        const int saved_pf_depth = para_for_body_depth_;
        if (pending_para_for_) {
            in_para_for_body_ = true;
            para_for_body_depth_ = loop_depth_;
        }
        check_block(s->body.get());
        in_para_for_body_ = saved_pf_body;
        para_for_body_depth_ = saved_pf_depth;
        --loop_depth_;
    }
}  // namespace muc_detail
