// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds the rest of the statement checks (definitions of Sema members).
//
// owns:      body checks for statements — aggregate initialization, assignment, return/if/switch, the
//            writability and borrow-root helpers, and the enum-member enumeration for `switch`
// not:       `for-in` and its sequence shapes (sema_for.cpp); declaration checks, expression typing,
//            call resolution
// pass:      body checking (the other half of the statement pass lives in sema_stmt.cpp)
// concept:   statements, aggregates
#include "sema_impl.hpp"

#include "ast_clone.hpp"

namespace muc_detail {

    // Whether re-evaluating the expression is observably free of side effects. Used by the
    // compound-assignment rewrite on user types, which spells the target twice (`x = x op y`).
    static bool pure_expr(const Expr* e) {
        if (!e) return false;
        if (dynamic_cast<const IntLit*>(e) || dynamic_cast<const FloatLit*>(e) ||
            dynamic_cast<const CharLit*>(e) || dynamic_cast<const StringLit*>(e) ||
            dynamic_cast<const BoolLit*>(e) || dynamic_cast<const NullLit*>(e)) {
            return true;
        }
        if (dynamic_cast<const Name*>(e)) return true;
        if (auto* m = dynamic_cast<const Member*>(e)) return pure_expr(m->obj.get());
        if (auto* ix = dynamic_cast<const Index*>(e)) {
            if (!pure_expr(ix->base.get())) return false;
            for (const auto& i : ix->indices) {
                if (!pure_expr(i.get())) return false;
            }
            return true;
        }
        if (auto* u = dynamic_cast<const Unary*>(e)) {
            if (u->op == "-" || u->op == "+" || u->op == "~" || u->op == "*") {
                return pure_expr(u->operand.get());
            }
            return false;
        }
        if (auto* b = dynamic_cast<const Binary*>(e)) {
            return pure_expr(b->left.get()) && pure_expr(b->right.get());
        }
        if (auto* c = dynamic_cast<const Cast*>(e)) return pure_expr(c->expr.get());
        return false;
    }

    static bool pure_lvalue(const Expr* e) {
        if (!e) return false;
        if (dynamic_cast<const Name*>(e)) return true;
        if (auto* m = dynamic_cast<const Member*>(e)) return pure_expr(m->obj.get());
        if (dynamic_cast<const Index*>(e)) return pure_expr(e);
        if (auto* u = dynamic_cast<const Unary*>(e)) return u->op == "*" && pure_expr(u->operand.get());
        return false;
    }

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
            // A type with default member initializers gets a synthesized constructor (SPEC 3.7), so the
            // user never wrote one: say where it comes from instead of "has a constructor".
            if (ty->implicit_ctor) {
                err("struct `" + ty->str() +
                        "` has default member initializers, so it is constructed: write `" + ty->name +
                        "(...)` instead of an aggregate initializer",
                    init);
            }
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

    // True when binding this expression to a mutable `ref` parameter would hand out write access to a
    // read-only object: a `const` binding, a `const` field, or a `const` handle (SPEC 3.7).
    // `check_writable` is the assignment form of the same rule; a `ref` binding is the second way to
    // obtain a writable view, so it needs the same predicate.
    bool Sema::ref_target_is_readonly(Expr* a) {
        if (!a) return false;
        if (root_is_const(a)) return true;
        if (auto n = dynamic_cast<Name*>(a)) {
            if (n->ty && n->ty->is_const) return true;  // `int* const p`: the pointee is writable, the binding is not
            if (n->kind == "field" && n->implicit_this && cur_class_ && n->field_index >= 0 &&
                static_cast<size_t>(n->field_index) < cur_class_->fields.size()) {
                const Field& f = cur_class_->fields[static_cast<size_t>(n->field_index)];
                if (f.type && f.type->is_const) return true;
            }
            return false;
        }
        if (auto m = dynamic_cast<Member*>(a)) {
            TypePtr ot = m->obj ? m->obj->ty : nullptr;
            if (ot && read_only_view(ot)) return true;
            if (ot && m->field_index >= 0 && static_cast<size_t>(m->field_index) < ot->fields.size()) {
                const Field& f = ot->fields[static_cast<size_t>(m->field_index)];
                if (f.type && f.type->is_const) return true;
            }
            return ref_target_is_readonly(m->obj.get());
        }
        if (auto ix = dynamic_cast<Index*>(a)) {
            TypePtr bt = ix->base ? ix->base->ty : nullptr;
            if (bt && bt->elem && bt->elem->is_const &&
                (bt->kind == TypeKind::Slice || bt->kind == TypeKind::Ptr ||
                 bt->kind == TypeKind::Array)) {
                return true;
            }
            return ref_target_is_readonly(ix->base.get());
        }
        if (auto u = dynamic_cast<Unary*>(a); u && u->op == "*") {
            TypePtr ot = u->operand->ty;
            return ot && ot->kind == TypeKind::Ptr && ot->elem && ot->elem->is_const;
        }
        return false;
    }

    bool Sema::root_is_this(Expr* e) {        if (!e) return false;
        if (auto n = dynamic_cast<Name*>(e)) return n->ident == "this" || n->implicit_this;
        if (auto m = dynamic_cast<Member*>(e)) return root_is_this(m->obj.get());
        if (auto ix = dynamic_cast<Index*>(e)) return root_is_this(ix->base.get());
        return false;
    }

    void Sema::check_writable(Expr* t) {
        if (auto n = dynamic_cast<Name*>(t)) {
            if (n->ident == "this") err("cannot assign to `this`", t);
            // A `ref` parameter is a binding to the caller's variable, so assigning to it writes that
            // variable — for every value type, not only scalars. (It cannot "rebind": the language has no
            // references. The old struct carve-out made an out-parameter impossible to write, which is
            // what `std.mem.replace` needs.) The const rules below still apply.
            if (root_is_const(t)) err("`" + n->ident + "` is a `const` binding and cannot be assigned", t);
            if (auto lv = std::get_if<LocalVar*>(&n->sym); lv && *lv && (*lv)->ref_const) {
                err("cannot assign through a `const ref` binding", t);
            }
            // `int* const p`: the cv belongs to the pointer type, not to the binding, so the binding is
            // not `const` yet the object is read-only (`p` cannot be retargeted, `*p` can be written).
            if (n->ty && n->ty->is_const) {
                err("cannot reassign `" + n->ident + "`: its type is read-only (`" + n->ty->str() + "`)", t);
            }
            // An implicit `this.field`: a field declared `const` is read-only outside the constructor.
            if (cur_method_const_ && (n->ident == "this" || n->implicit_this)) {
                err("cannot modify the object inside a `const` method", t);
            }
            if (n->kind == "field" && n->implicit_this && cur_class_ && n->field_index >= 0 &&
                static_cast<size_t>(n->field_index) < cur_class_->fields.size()) {
                const Field& f = cur_class_->fields[static_cast<size_t>(n->field_index)];
                if (f.type && f.type->is_const)
                    err("cannot assign to a read-only (`const`) field `" + n->ident + "`", t);
            }
            return;
        }
        if (auto m = dynamic_cast<Member*>(t)) {
            if (cur_method_const_ && root_is_this(t)) {
                err("cannot modify the object inside a `const` method", t);
            }
            if (root_is_const(t)) err("cannot modify a member of a `const` binding", t);
            TypePtr ot = m->obj ? m->obj->ty : nullptr;
            // A `const` handle is a read-only view of a shared object: that covers the fields it
            // inherited as well. The field's own `const` is checked below, but an inherited field is
            // mutable.
            if (ot && read_only_view(ot)) {
                err("cannot modify a member of a read-only (`const`) value (`" + ot->str() + "`)", t);
            }
            // `arr[0].x = ...` where `arr` is a `const P[N]`: the element is a const object even though the
            // binding is not (SPEC 3.7: a `const T[N]` array has read-only elements).
            if (ot && ot->is_const) {
                err("cannot modify a member of a read-only (`const`) value (`" + ot->str() + "`)", t);
            }
            if (ot &&
                (ot->kind == TypeKind::Struct || ot->kind == TypeKind::Class) && m->field_index >= 0 &&
                static_cast<size_t>(m->field_index) < ot->fields.size()) {
                const Field& f = ot->fields[static_cast<size_t>(m->field_index)];
                if (f.type && f.type->is_const)
                    err("cannot assign to a read-only (`const`) field `" + m->name + "`", t);
            }
            return;
        }
        if (auto ix = dynamic_cast<Index*>(t)) {
            TypePtr bt = ix->base->ty;
            if (bt && bt->kind == TypeKind::Slice && bt->elem && bt->elem->is_const)
                err("cannot write through a `const` slice", t);
            if (bt && bt->kind == TypeKind::Ptr && bt->elem && bt->elem->is_const)
                err("cannot write through a `const` pointer", t);
            // `const T[N]` has read-only elements (SPEC 3.7); the binding itself is not `const`.
            if (bt && bt->kind == TypeKind::Array && bt->elem && bt->elem->is_const)
                err("cannot write a read-only (`const`) array element", t);
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
        // `a = T(args);` builds the value in place and move-assigns it: the construction on the right
        // is not a copy of an existing object (see check_call).
        struct AssignRhsGuard {
            Sema& self;
            Expr* saved;
            explicit AssignRhsGuard(Sema& sema, Expr* e) : self(sema), saved(sema.assign_rhs_expr_) {
                self.assign_rhs_expr_ = e;
            }
            ~AssignRhsGuard() { self.assign_rhs_expr_ = saved; }
        } rhs_guard(*this, s->value.get());
        // `x = ...` makes a moved variable usable again, so revive it before the target is checked.
        if (s->op == "=") {
            if (auto n = dynamic_cast<Name*>(s->target.get())) note_assigned_local(n->ident);
        }
        if (!is_lvalue(s->target.get())) {
            // The result of a `ref T f()` call is also a writable lvalue (write-through borrow)
            if (auto* c = dynamic_cast<Call*>(s->target.get())) {
                check_expr(c);
                if (c->ref_const) err("cannot assign through a readonly (`const ref`) return value", s);
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
                    err("cannot assign through a readonly (`const ref`) subscript", s->target.get());
                check_writable(s->target.get());
                // no return: falls through to the generic assignment/compound-assignment check below
            }
        }

        TypePtr tt;
        {
            WriteGuard writing(write_ctx_);   // `x = 1` writes; it must not count as a read
            tt = check_expr(s->target.get());
        }
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
            if (auto n = dynamic_cast<Name*>(s->target.get()); n && n->kind == "local") {
                if (auto lvp = std::get_if<LocalVar*>(&n->sym); lvp && *lvp) maybe_uninit_.erase(*lvp);
            }
            return;
        }
        if (is_enum_t(tt)) {
            err("enum `" + tt->str() + "` does not support `" + s->op +
                    "` (write `x = (x as int " + s->op.substr(0, s->op.size() - 1) +
                    " y) as " + tt->name + "`）",
                s);
        }
        const std::string op = s->op.empty() ? std::string() : s->op.substr(0, s->op.size() - 1);
        // Compound assignment on a user type is sugar for `x = x op y`: there is no `operator+=`,
        // so resolve `op` exactly as the binary expression would and store the result. The target is
        // rewritten as a *clone* on the right, which is why it must be side-effect free; a numeric
        // target keeps the in-place loop in codegen (evaluated once, no operator lookup).
        if (is_user_type(tt)) {
            if (!pure_lvalue(s->target.get())) {
                err("`" + s->op + "` on `" + tt->str() + "` is rewritten as `x = x " + op +
                        " y`, so the left side must be a name, a field, or a subscript with "
                        "side-effect-free indices; write the update explicitly if it must be "
                        "evaluated once",
                    s);
            }
            auto bin = std::make_unique<Binary>();
            bin->op = op;
            bin->line = s->line;
            bin->col = s->col;
            bin->left = clone_expr(*s->target);
            bin->right = std::move(s->value);
            // Install the rewrite before checking: a diagnostic leaves the statement well-formed
            // (`x = x op y`), never with a null operand, so a later clone/re-check cannot crash.
            s->op = "=";
            s->value = std::move(bin);
            auto* lowered = static_cast<Binary*>(s->value.get());
            lowered->ty = check_binary(lowered);
            check_assign(lowered, tt, "assignment");
            return;
        }
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
        // `x op= y` means `x = x op y`: the arithmetic result has to fit back into `x` without
        // narrowing (SPEC 3.6, design principle 2 "no implicit narrowing"). Small integer types
        // promote to `int`, so `byte b; b += 1;` is an error just like `b = b + 1` — the caller
        // writes `b = (b + 1) as byte`. Unsigned wrap-around stays legal (`u += 1` has a `uint`
        // result), which is what `same_type` accepts.
        TypePtr res = (op == "<<" || op == ">>") ? tt : usual_arith(tt, vt);
        if (!res) err("`" + s->op + "` is not supported for " + tt->str() + " and " + vt->str(), s);
        if (!same_type(res, tt) && !implicit_ok(res, tt)) {
            err("compound assignment `" + s->op + "` would narrow `" + res->str() + "` to `" +
                    tt->str() + "`; write `x = (x " + op + " y) as " + tt->str() + "`",
                s);
        }
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
            TypePtr vt = check_expr(s->value.get(), base);
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
                err("cannot return a mutable `ref` to a readonly (`const`) location; declare the return type as `const ref`",
                    s->value.get());
            }
            return;
        }
        const bool saved_in_return = in_return_;
        in_return_ = true;
        struct ReturnGuard {
            bool& f;
            bool v;
            ~ReturnGuard() { f = v; }
        } return_guard{in_return_, saved_in_return};
        // A `[ref x]` capture stores the address of a frame-owned local, so a returned delegate would
        // always dangle. Caller-owned storage (`this` of a struct method, a `ref` parameter, a `for ref`
        // binding) is the caller's responsibility, exactly like `&x` — those are allowed.
        if (auto lm = dynamic_cast<Lambda*>(s->value.get())) {
            for (size_t i = 0; i < lm->captures.size(); ++i) {
                auto& cap = lm->captures[i];
                if (!cap.by_ref) continue;
                const LocalVar* src = i < lm->env_src.size() ? lm->env_src[i] : nullptr;
                const bool caller_owned = (cap.name == "this") || (src && src->by_ref);
                if (!caller_owned) {
                    err("cannot return a lambda that captures `ref " + cap.name +
                            "`: the captured local dies when this function returns (capture it by "
                            "value, or keep the state in a `class`)",
                        s->value.get());
                }
            }
        }
        // `return x;` for a local or parameter of a movable-but-uncopyable type is a **move**: the name
        // dies with the frame, so transferring it cannot alias anything the caller can observe. Requiring
        // the user to write `move x` there is pure syntax (C++ moves implicitly in this position too).
        // A copyable type keeps the copy path: moving it would silently change copy counts.
        if (auto nm = dynamic_cast<Name*>(s->value.get())) {
            LocalVar* lv = nm->sym.index() == 0 ? nullptr : std::get<LocalVar*>(nm->sym);
            if (!lv) lv = lookup_local(nm->ident);
            if (lv && !lv->by_ref && lv->rty && lv->rty->kind == TypeKind::Struct &&
                !lv->rty->copyable() && lv->rty->movable()) {
                auto mv = std::make_unique<MoveExpr>();
                mv->operand = std::move(s->value);
                mv->line = mv->operand->line;
                mv->col = mv->operand->col;
                s->value = std::move(mv);
            }
        }
        check_assign(s->value.get(), rt, "return value");
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
        if (auto* b = dynamic_cast<Binary*>(e)) return b->by_ref;
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
            err("match pattern value " + std::to_string(v) + " is out of range for " + st->str(), e);
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
                    err("pattern type does not match the matched value: expected " + st->str(), e);
                }
                return (*ec)->value;
            }
            if (auto* bin = dynamic_cast<Binary*>(e); bin && bin->op == "|") {
                err("or-patterns over enum members are written with a comma list (`Red, Green:`); `|` "
                    "is the bitwise-or operator",
                    e);
            }
            err("patterns of an enum match must be members of " + st->str(), e);
        }

        if (st->is_bool()) {
            // `true` / `false`, or a compile-time constant of type bool. An integer literal is not
            // accepted (`1:` is not `true`), matching the "no implicit integer for an enum" rule.
            if (auto* bl = dynamic_cast<BoolLit*>(e)) return bl->value ? 1 : 0;
            if (dynamic_cast<Name*>(e) || dynamic_cast<Member*>(e)) {
                TypePtr t = check_expr(e);
                if (!t || !t->is_bool()) err("patterns of a bool match must be `true` or `false`", e);
                long long folded = 0;
                if (eval_const_int(e, folded)) return folded != 0 ? 1 : 0;
            }
            err("patterns of a bool match must be `true` or `false` (or a unit-level `const bool`)", e);
        }

        if (auto* il = dynamic_cast<IntLit*>(e)) {
            if (il->min_magnitude) {
                err("integer literal is too large (maximum 2^63-1): a 2^63 magnitude is only valid "
                    "directly after unary `-`",
                    e);
            }
            return checked_int_pattern(il->value, st, e);
        }
        if (auto* cl = dynamic_cast<CharLit*>(e)) return checked_int_pattern(cl->value, st, e);
        if (auto* u = dynamic_cast<Unary*>(e); u && (u->op == "-" || u->op == "+")) {
            if (auto* il = dynamic_cast<IntLit*>(u->operand.get())) {
                const long long v = u->op == "-" ? -il->value : il->value;
                return checked_int_pattern(v, st, e);
            }
        }
        // A unit-level compile-time constant, an arithmetic form or `std.comptime.X` (SPEC 3.3.2: the
        // same evaluator feeds `#if`, `step`, `align(N)`, match ranges and array lengths).
        long long folded = 0;
        if (eval_const_int(e, folded)) return checked_int_pattern(folded, st, e);
        if (auto* n = dynamic_cast<Name*>(e)) {
            check_expr(e);  // resolve as a variable / enum member to give a more precise error
            if (std::get_if<EnumConst*>(&n->sym)) {
                err("pattern `" + n->ident + "` is an enum member and cannot match " + st->str(), e);
            }
            err("a match pattern must be a compile-time constant: `" + n->ident +
                    "` is a variable, not a unit-level `const`; use a literal or a compile-time constant",
                e);
        }
        err("a match pattern must be a compile-time constant (a literal, an enum member, or a "
            "unit-level `const`)",
            e);
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

    // One arm's result: the value of an expression arm, or the checked block of a statement arm.
    // Shared by both scrutinee kinds (variant and constant), so the two forms stay in step.
    void Sema::check_match_arm_body(MatchArm* arm, bool expression_form, const TypePtr& expected,
                                    TypePtr& result, bool& result_void) {
        if (expression_form) {
            TypePtr t = check_expr(arm->value.get(), result ? result : expected);
            if (!t) err("match arm has no value", arm);
            if (is_void(t)) result_void = true;
            if (result && !same_type_sig(result, t)) {
                err("match arms must have the same type (found `" + result->str() + "` and `" + t->str() +
                        "`)",
                    arm);
            }
            if (!result) result = t;
        } else {
            check_block(arm->body.get());
        }
    }

    // `match` on an integer, `char`, enum or bool: constant patterns, the rules `switch` used to own
    // (SPEC 6.6, 10.1.2). The overlap and exhaustiveness rules count only **unguarded** patterns,
    // because a guard makes an arm's reachability depend on a runtime value.
    void Sema::check_match_constant(Expr* scrut, std::vector<std::unique_ptr<MatchArm>>& arms,
                                    bool& has_else, bool expression_form, Node* at, TypePtr& value_ty,
                                    const TypePtr& expected) {
        TypePtr st = scrut->ty;
        const bool enum_scrut = st->kind == TypeKind::Enum;
        const bool bool_scrut = st->is_bool();
        if (!enum_scrut && !bool_scrut && !(is_prim(st) && is_integer(st))) {
            err("match needs a `variant`, an enum, `bool` or an integer (found " + st->str() +
                    "); floating-point, pointer, class and string values are not pattern-matched",
                scrut);
        }
        {
            size_t patterns = 0;
            for (auto& arm : arms) patterns += arm->pats.size();
            if (patterns > kMaxMatchPatterns) {
                err("this match has more than " + std::to_string(kMaxMatchPatterns) +
                        " patterns; split the value space",
                    at);
            }
        }
        // Overlap detection: merge [lo, hi) intervals of the unguarded patterns.
        struct Iv {
            long long lo = 0;
            long long hi = 0;
            bool top = false;
            const Node* node = nullptr;
        };
        std::vector<Iv> ivs;
        TypePtr result = nullptr;
        bool result_void = false;
        bool exhaustive = false;
        std::string missing;

        for (auto& up : arms) {
            MatchArm* arm = up.get();
            if (arm->is_else) {
                has_else = true;
                push();
                check_match_arm_body(arm, expression_form, expected, result, result_void);
                pop();
                continue;
            }
            if (!arm->alts.empty()) {
                err("the patterned value is `" + st->str() + "`, so `" + arm->alts[0].name +
                        "(...)` (a variant pattern) cannot be used here",
                    arm);
            }
            if (arm->pats.empty()) err("match arm is missing a pattern", arm);
            for (auto& p : arm->pats) {
                p.vlo = pattern_value(p.lo.get(), st);
                Iv iv;
                iv.lo = p.vlo;
                iv.node = p.lo.get();
                if (p.is_range) {
                    if (enum_scrut) {
                        err("enum matches do not support ranges (enum members have no numeric order); list "
                            "each member explicitly",
                            p.lo.get());
                    }
                    if (bool_scrut) {
                        err("a bool match does not support ranges; list `true` and `false`", p.lo.get());
                    }
                    p.vhi = pattern_value(p.hi.get(), st);
                    if (p.inclusive) {
                        if (p.vhi == LLONG_MAX) {
                            err("match range upper bound is too large for an inclusive range (`..=`)",
                                p.hi.get());
                        }
                        ++p.vhi;  // normalize to the half-open interval [vlo, vhi)
                    }
                    if (p.vlo >= p.vhi) {
                        err("match range `" + std::to_string(p.vlo) + (p.inclusive ? "..=" : "..") +
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
                if (!arm->guard) ivs.push_back(iv);
            }
            push();
            if (arm->guard) check_cond(arm->guard.get(), "match guard");
            check_match_arm_body(arm, expression_form, expected, result, result_void);
            pop();
        }

        std::sort(ivs.begin(), ivs.end(), [](const Iv& a, const Iv& b) { return a.lo < b.lo; });
        for (size_t i = 1; i < ivs.size(); ++i) {
            const Iv& prev = ivs[i - 1];
            const Iv& cur = ivs[i];
            if (prev.top || cur.lo < prev.hi) {
                err("overlapping match patterns: value " + std::to_string(cur.lo) +
                        " is already covered by an earlier pattern (ranges are half-open; `a..b` excludes b)",
                    cur.node);
            }
        }

        // An enum or bool match has a closed value set, so it must cover it unless `else` is written.
        if (enum_scrut || bool_scrut) {
            std::vector<std::pair<std::string, long long>> members;
            if (enum_scrut) {
                members = enum_members_of(st);
            } else {
                members = {{"true", 1}, {"false", 0}};
            }
            for (auto& [name, v] : members) {
                bool covered = false;
                for (auto& up : arms) {
                    if (up->guard) continue;  // a guard does not cover the member
                    for (auto& p : up->pats) {
                        if (!p.is_range && p.vlo == v) covered = true;
                    }
                }
                if (!covered) {
                    if (!missing.empty()) missing += ", ";
                    missing += name;
                }
            }
            if (!has_else) {
                if (!missing.empty()) {
                    if (enum_scrut) {
                        err("match does not cover all members of enum " + st->str() + " (missing " + missing +
                                "); add the missing patterns or write `else:`",
                            at);
                    }
                    err("a bool match must cover `true` and `false` (missing " + missing +
                            "); add the missing pattern or write `else:`",
                        at);
                }
                exhaustive = true;
            } else if (missing.empty()) {
                warn("`else` arm is unreachable: " + (enum_scrut ? "all members of `" + st->str() + "`" :
                                                                  std::string("`true` and `false`")) +
                         " are already covered",
                     at);
            }
        }
        if (expression_form) {
            if (!has_else && !exhaustive) {
                err("an expression match must produce a value for every input: add `else:`", at);
            }
            if (!result) err("match has no arms", at);
            if (result_void && result && !is_void(result)) {
                err("a `void` match arm cannot be mixed with a value arm", at);
            }
        }
        value_ty = expression_form ? result : t_void();
    }

    // `match` (SPEC 10.1.2, decided 2026-11-20). Shared by MatchExpr and MatchStmt: the alternative
    // set, bindings, guards and exhaustiveness are identical; only the arm body differs.
    void Sema::check_match_common(Expr* scrut, std::vector<std::unique_ptr<MatchArm>>& arms,
                                  bool& has_else, bool& match_ref, bool expression_form, Node* at,
                                  TypePtr& value_ty, const TypePtr& expected) {
        ScopeGuard guard(*this);
        TypePtr st = check_expr(scrut);
        if (!st) err("match is missing the value to dispatch on", at);
        if (st->kind != TypeKind::Sum) {
            // Integer and enum scrutinees take constant patterns (SPEC 10.1.2, formerly `switch`).
            check_match_constant(scrut, arms, has_else, expression_form, at, value_ty, expected);
            return;
        }
        // A variant arm written with bare alternative names (`None`, `A | B`) arrives as name patterns;
        // the parser cannot tell them from enum members, so the scrutinee's type settles it here.
        for (auto& arm : arms) {
            if (arm->is_else || !arm->alts.empty()) continue;
            bool all_names = !arm->pats.empty();
            for (auto& p : arm->pats) {
                if (p.is_range || !dynamic_cast<Name*>(p.lo.get())) all_names = false;
            }
            if (!all_names) {
                // `A | B` parses as a bitwise-or of two names; say how to write it here.
                for (auto& p : arm->pats) {
                    if (p.is_range) continue;
                    if (auto* bin = dynamic_cast<Binary*>(p.lo.get()); bin && bin->op == "|") {
                        err("or-patterns over alternatives without payloads are written with a comma "
                            "list (`A, B:`); `|` is the bitwise-or operator",
                            p.lo.get());
                    }
                }
                continue;
            }
            for (auto& p : arm->pats) {
                MatchAlt a;
                a.name = static_cast<Name*>(p.lo.get())->ident;
                arm->alts.push_back(std::move(a));
            }
            arm->pats.clear();
        }
        for (auto& arm : arms) {
            for (auto& a : arm->alts) {
                for (auto& b : a.binds) {
                    if (b.mode != 0) match_ref = true;
                }
            }
        }
        if (match_ref) {
            if (dynamic_cast<MoveExpr*>(scrut)) {
                err("`match move v` cannot use `ref` bindings; bind by value instead", scrut);
            }
            if (!is_lvalue(scrut)) {
                err("a `ref` binding needs an addressable value: match a variable or a field", scrut);
            }
            if (scrut->ty && scrut->ty->is_const) {
                for (auto& arm : arms) {
                    for (auto& a : arm->alts) {
                        for (auto& b : a.binds) {
                            if (b.mode == 1) {
                                err("cannot take a mutable `ref` binding from a `const` value; write "
                                    "`const ref`",
                                    arm.get());
                            }
                        }
                    }
                }
            }
        }
        // A variant whose payload cannot be copied has to be consumed by the match: the scrutinee is
        // moved into the match's temporary, and the bindings move out of it. A plain lvalue would be a
        // copy (and the temporary's destruction would release the payload twice). `ref` bindings work
        // on the scrutinee's own storage and need no move.
        if (!match_ref && !st->copyable()) {
            bool consumed = dynamic_cast<MoveExpr*>(scrut) != nullptr;
            if (auto c = dynamic_cast<Call*>(scrut); c && !c->by_ref) consumed = true;
            if (dynamic_cast<MatchExpr*>(scrut) || dynamic_cast<Ternary*>(scrut)) consumed = true;
            if (!consumed) {
                err("this variant's payload is not copyable; match a moved value (`match move v`) or "
                    "bind by reference (`ref`)",
                    scrut);
            }
        }
        std::vector<int> seen(st->sum_names.size(), 0);
        std::vector<int> unguarded_seen(st->sum_names.size(), 0);
        // A target-typed `match` (`Option<int> o = match r { ... }`) hands its type to every arm, so a
        // bare alternative name (`Some(v)`) resolves without spelling the type out (SPEC 10.1.1).
        TypePtr result = (expected && expected->kind == TypeKind::Sum) ? expected : nullptr;
        bool result_void = false;
        for (auto& arm : arms) {
            if (arm->is_else) {
                // the parser already rejects a second `else` / an `else` that is not last; has_else may
                // also arrive pre-set from the parser, so it is not an error here
                has_else = true;
                push();
                check_match_arm_body(arm.get(), expression_form, expected, result, result_void);
                pop();
                continue;
            }
            if (arm->alts.empty())
                err("a variant match arm needs an alternative name (`Alt` or `Alt(x)`); a constant "
                    "pattern needs an integer, `char`, enum or bool value to match against",
                    arm.get());
            if (!arm->pats.empty()) err("a variant arm cannot carry constant patterns", arm.get());
            // Resolve every alternative of the arm; `A(x) | B(x)` covers both with one body.
            for (auto& a : arm->alts) {
                a.index = -1;
                for (size_t k = 0; k < st->sum_names.size(); ++k) {
                    if (st->sum_names[k] == a.name) {
                        a.index = static_cast<int>(k);
                        break;
                    }
                }
                if (a.index < 0) err("variant `" + st->str() + "` has no alternative `" + a.name + "`", arm.get());
                for (auto& prev : arm->alts) {
                    if (&prev != &a && prev.index == a.index) {
                        err("alternative `" + a.name + "` appears twice in one pattern", arm.get());
                    }
                }
                if (!arm->guard && unguarded_seen[static_cast<size_t>(a.index)]) {
                    err("this arm is unreachable: alternative `" + a.name +
                            "` is already covered by an earlier arm without a guard",
                        arm.get());
                }
                seen[static_cast<size_t>(a.index)] = 1;
                if (!arm->guard) unguarded_seen[static_cast<size_t>(a.index)] = 1;
            }
            // Every alternative binds the same names, with the same modes and the same types, so one
            // set of locals serves the shared body.
            const MatchAlt& first = arm->alts[0];
            const auto& fs = st->sum_fields[static_cast<size_t>(first.index)];
            for (auto& a : arm->alts) {
                const auto& afs = st->sum_fields[static_cast<size_t>(a.index)];
                if (a.binds.size() != afs.size()) {
                    err("alternative `" + a.name + "` carries " + std::to_string(afs.size()) +
                            " value(s), so the pattern must bind " + std::to_string(afs.size()) + " name(s)",
                        arm.get());
                }
                for (size_t k = 0; k < afs.size(); ++k) {
                    if (a.binds[k].name != first.binds[k].name || a.binds[k].mode != first.binds[k].mode) {
                        err("every alternative of an or-pattern must bind the same names in the same "
                            "order with the same binding modes (`" + first.name + "` binds `" +
                                first.binds[k].name + "`)",
                            arm.get());
                    }
                    if (!same_type(fs[k], afs[k])) {
                        err("or-pattern alternatives bind `" + first.binds[k].name + "` with different "
                            "types (`" + first.name + "` gives `" + fs[k]->str() + "`, `" + a.name +
                                "` gives `" + afs[k]->str() + "`)",
                            arm.get());
                    }
                }
            }
            push();
            std::vector<LocalVar*> locals;
            for (size_t k = 0; k < fs.size(); ++k) {
                auto lv = std::make_unique<LocalVar>();
                lv->name = first.binds[k].name;
                lv->rty = fs[k];
                lv->by_ref = first.binds[k].mode != 0;
                lv->ref_const = first.binds[k].mode == 2;
                lv->line = first.binds[k].line;
                lv->col = first.binds[k].col;
                declare_local(lv.get());
                locals.push_back(lv.get());
                if (cur_func_) cur_func_->synthetic_locals.push_back(std::move(lv));
            }
            if (arm->guard) check_cond(arm->guard.get(), "match guard");
            check_match_arm_body(arm.get(), expression_form, expected, result, result_void);
            pop();
            arm->bind_locals = std::move(locals);
        }
        // Coverage counts only **unguarded** arms (SPEC 10.1.2): a failed guard falls through, so a
        // guarded arm alone leaves the value unmatched — and in the expression form the result slot
        // would never be written.
        if (!has_else) {
            for (size_t k = 0; k < seen.size(); ++k) {
                if (unguarded_seen[k]) continue;
                if (seen[k]) {
                    err("`match` covers alternative `" + st->sum_names[k] +
                            "` only with a guarded arm; a guard does not count as coverage: add an arm "
                            "without a guard or an `else`",
                        at);
                }
                err("`match` does not cover alternative `" + st->sum_names[k] +
                        "`; add that arm or an `else`",
                    at);
            }
        }
        if (expression_form && !result) err("match has no arms", at);
        if (expression_form && result_void && result && !is_void(result)) {
            err("a `void` match arm cannot be mixed with a value arm", at);
        }
        value_ty = expression_form ? result : t_void();
    }

    TypePtr Sema::check_match(MatchExpr* m, const TypePtr& expected) {
        TypePtr ty;
        check_match_common(m->scrut.get(), m->arms, m->has_else, m->match_ref,
                           /*expression_form=*/true, m, ty, expected);
        m->sty = m->scrut->ty;
        return ty;
    }

    void Sema::check_match_stmt(MatchStmt* m) {
        TypePtr ty;
        check_match_common(m->scrut.get(), m->arms, m->has_else, m->match_ref,
                           /*expression_form=*/false, m, ty, nullptr);
        m->sty = m->scrut->ty;
    }

}  // namespace muc_detail
