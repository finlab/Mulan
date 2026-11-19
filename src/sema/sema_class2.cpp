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

    // Is this expression a value subobject of the object being constructed/destroyed (`this`, or a nested
    // struct / fixed-size array of one), rather than a separate object reached through a class handle
    // field? Constructing and destroying are the writable phases (SPEC 3.7), and they cover only the
    // object itself.
    bool Sema::is_this_value(Expr* e) {
        if (!e) return false;
        if (auto n = dynamic_cast<Name*>(e)) {
            return n->ident == "this" || (n->implicit_this && n->kind != "field");
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            TypePtr ot = m->obj ? m->obj->ty : nullptr;
            if (ot && is_reference_type(ot)) return false;  // a handle: a different object
            return is_this_value(m->obj.get());
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            TypePtr bt = ix->base ? ix->base->ty : nullptr;
            if (!bt || bt->kind != TypeKind::Array) return false;  // a slice points elsewhere
            return is_this_value(ix->base.get());
        }
        return false;
    }

    // True when binding this expression to a mutable `ref` parameter would hand out write access to a
    // read-only object: a `const` binding, a `const` field, or any expression of a `const class` / a
    // `const` handle (SPEC 3.7). `check_writable` is the assignment form of the same rule; a `ref`
    // binding is the second way to obtain a writable view, so it needs the same predicate.
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
            if (auto lv = std::get_if<LocalVar*>(&n->sym); lv && *lv && (*lv)->ref_const) {
                err("cannot assign through a `for const ref` binding", t);
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
            // Any expression of a `const class`, or a `const` handle, is a read-only view: the object's
            // content is fixed after construction, and that covers the fields it inherited as well
            // (SPEC 3.7). The field's own `const` is checked below, but an inherited field is mutable.
            //
            // Constructors and the destructor are the writable phase, so a write through `this` there is
            // allowed (a write to *another* read-only object is not).
            const bool this_write_phase = (in_ctor_ || in_dtor_) && is_this_value(m->obj.get());
            if (ot && read_only_view(ot) && !this_write_phase) {
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
                err("cannot return a mutable `ref` to a readonly (`const`) location; declare the return type as `const ref`",
                    s->value.get());
            }
            return;
        }
        in_return_ = true;
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
        // A unit-level compile-time constant, an arithmetic form or `std.comptime.X` (SPEC 3.3.2: the
        // same evaluator feeds `#if`, `step`, `align(N)`, switch ranges and array lengths).
        long long folded = 0;
        if (eval_const_int(e, folded)) return checked_int_pattern(folded, st, e);
        if (auto* n = dynamic_cast<Name*>(e)) {
            check_expr(e);  // resolve as a variable / enum member to give a more precise error
            if (std::get_if<EnumConst*>(&n->sym)) {
                err("pattern `" + n->ident + "` is an enum member and cannot match " + st->str(), e);
            }
            err("switch case pattern must be a compile-time constant: `" + n->ident +
                    "` is a variable, not a unit-level `const`; use a literal or a compile-time constant",
                e);
        }
        err("switch case pattern must be a compile-time constant (a literal, an enum member, or a "
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

    void Sema::check_switch(SwitchStmt* s) {
        ScopeGuard guard(*this);
        // Case labels of one switch: C++ [implimits] §1.19 sets 16384 (nested switches are separate).
        {
            size_t labels = 0;
            for (auto& arm : s->arms) labels += arm->pats.size();
            if (labels > kMaxSwitchLabels) {
                err("this switch has more than " + std::to_string(kMaxSwitchLabels) +
                        " case labels; split the value space",
                    s);
            }
        }
        TypePtr st = check_expr(s->scrut.get());
        if (!st) err("switch is missing the value to match", s);
        // A sum-type scrutinee dispatches on the tag and may bind payload fields (SPEC 10.1.2).
        if (st->kind == TypeKind::Sum) {
            s->sty = st;
            s->is_sum = true;
            std::vector<int> seen(st->sum_names.size(), 0);
            for (auto& arm : s->arms) {
                if (arm->is_else) {
                    s->has_else = true;
                    push();
                    check_block(arm->body.get());
                    pop();
                    continue;
                }
                if (arm->alt.empty()) {
                    // A bare name label is an alternative (`Some:`); values make no sense on a sum.
                    if (arm->pats.size() == 1 && !arm->pats[0].is_range) {
                        if (auto nm = dynamic_cast<Name*>(arm->pats[0].lo.get())) arm->alt = nm->ident;
                    }
                    if (arm->alt.empty()) {
                        err("a `switch` on a sum type needs alternative labels (`Some(v):` / `None:`)",
                            arm.get());
                    }
                }
                int idx = -1;
                for (size_t k = 0; k < st->sum_names.size(); ++k) {
                    if (st->sum_names[k] == arm->alt) { idx = static_cast<int>(k); break; }
                }
                if (idx < 0) {
                    err("sum type `" + st->str() + "` has no alternative `" + arm->alt + "`", arm.get());
                }
                if (seen[static_cast<size_t>(idx)]++ != 0) {
                    err("alternative `" + arm->alt + "` appears twice in this `switch`", arm.get());
                }
                const auto& fs = st->sum_fields[static_cast<size_t>(idx)];
                if (arm->binds.size() != fs.size()) {
                    err("alternative `" + arm->alt + "` carries " + std::to_string(fs.size()) +
                            " value(s), so the label must bind " + std::to_string(fs.size()) + " name(s)",
                        arm.get());
                }
                arm->alt_index = idx;
                push();
                std::vector<LocalVar*> locals;
                for (size_t k = 0; k < fs.size(); ++k) {
                    auto lv = std::make_unique<LocalVar>();
                    lv->name = arm->binds[k];
                    lv->rty = fs[k];
                    lv->line = arm->line;
                    lv->col = arm->col;
                    declare_local(lv.get());
                    locals.push_back(lv.get());
                    if (cur_func_) cur_func_->synthetic_locals.push_back(std::move(lv));
                }
                check_block(arm->body.get());
                pop();
                arm->bind_locals = std::move(locals);
            }
            if (!s->has_else) {
                for (size_t k = 0; k < seen.size(); ++k) {
                    if (!seen[k]) {
                        err("`switch` does not cover alternative `" + st->sum_names[k] +
                                "`; add that branch or an `else`",
                            s);
                    }
                }
            }
            return;
        }
        const bool enum_sw = st->kind == TypeKind::Enum;
        if (!enum_sw && !(is_prim(st) && is_integer(st))) {
            err("switch value must be an integer, enum or sum type (found " + st->str() + ")", s->scrut.get());
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

}  // namespace muc_detail
