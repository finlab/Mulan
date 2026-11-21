// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// `for-in` over a range, the cursor shape and the index shape (definitions of Sema members).
//
// owns:      the `for-in` check (`check_for`), the C-style `for` check (`check_for_c`) and the two
//            sequence-shape recognizers: `resolve_cursor_shape` (iterator/next/... structural contract)
//            and `resolve_index_shape`
// not:       the rest of the statement pass (sema_class2.cpp), expression typing, call resolution
// pass:      body checking
// concept:   statements, for-in-shape, for-c-shape

#include "sema_impl.hpp"

#include "ast_clone.hpp"

namespace muc_detail {

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
        if (is_borrow_type(cur->ret)) {
            err("cursor `current()` must return a value, not a borrow (`ref " + cur->ret->str() +
                    "`): the loop variable owns the element for the iteration",
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
        TypePtr us = Type::make_prim("isize");
        const MethodSig* len = find_method(st, "length", 0);
        if (!len) {
            err("`for-in` cannot iterate " + st->str() +
                    ": the indexed form requires a method `length() -> isize` (none found)",
                s->seq.get());
        }
        if (!same_type(len->ret, us)) {
            err("`" + st->str() + "::length()` must return `isize` (found `" +
                    (len->ret ? len->ret->str() : "void") + "`）",
                s->seq.get());
        }

        std::string saw_index;
        // A member `operator[](isize)` on the struct takes precedence (borrowed receiver, no copy/RC).
        const MethodSig* picked_m = nullptr;
        for (const auto& m : st->methods) {
            if (!m.is_operator || m.name != "operator[]" || m.params.size() != 1) continue;
            if (!decl_visible(m.owner ? m.owner->unit : std::string(), m.is_public)) continue;
            if (!same_type(m.params[0], us)) {
                if (saw_index.empty() && m.params[0]) saw_index = m.params[0]->str();
                continue;
            }
            if (picked_m) {
                err("`" + st->str() + "` has more than one `operator[](isize)` overload; `for-in` cannot choose one",
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
        {
            // No member `operator[]`: the free-function form was removed in build 161 because it copied
            // its receiver on every element access (7.5 ns vs 0.15 ns per element).
            std::string msg = "`for-in` cannot iterate " + st->str() +
                              ": the indexed form requires a member `operator[](isize) -> T` in `" + st->str() +
                              "`";
            if (!saw_index.empty()) {
                msg += " (the `operator[]` found takes index type `" + saw_index +
                       "`; `isize` is required)";
            }
            err(msg, s->seq.get());
        }
    }

    void Sema::check_for(ForInStmt* s) {
        ScopeGuard guard(*this);
        const auto uninitAtEntry = maybe_uninit_;  // the body may run zero times

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
                            "`: use an index loop (get each dimension's length with `a.length(k)`)",
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
                        " (supported: range, array, slice; user types must provide `length() -> isize` and "
                        "`operator[](isize) -> T`）",
                    s->seq.get());
            }
            s->seq_ty = st;
        }

        if (s->by_ref && s->loop_kind == "string") {
            err("`string` is immutable and cannot be iterated with `for ref`", s);
        }
        // `for ref` needs the member `operator[]` to return a borrow.
        const TypePtr idx_ret = s->idx_method ? s->idx_method->ret : nullptr;
        if (s->by_ref && s->loop_kind == "index" && !is_borrow_type(idx_ret)) {
            err("user types do not support `for ref` unless `operator[]` returns `ref T`; use `for i, x` with `c[i] = ...` instead",
                s->seq.get());
        }
        if (s->by_ref && s->loop_kind == "index" && is_borrow_type(idx_ret) && idx_ret->is_const &&
            !s->ref_const) {
            err("`for ref` requires a mutable borrow: `operator[]` returns `const ref T`; write "
                "`for const ref` for a read-only iteration",
                s->seq.get());
        }
        if (s->by_ref && s->loop_kind == "range") {
            err("`for ref` cannot be used on a range loop", s);
        }
        if (s->by_ref && s->elem_ty && is_reference_type(s->elem_ty)) {
            err("`for ref` is not supported for class or interface elements (bind the handle by value)", s);
        }
        if (s->by_ref && !s->ref_const && s->seq_ty && s->seq_ty->kind == TypeKind::Slice &&
            s->seq_ty->elem && s->seq_ty->elem->is_const) {
            err("`for ref` cannot be used on a `const` slice; write `for const ref` for a read-only "
                "iteration",
                s->seq.get());
        }
        if (s->by_ref && !s->ref_const && root_is_const(s->seq.get())) {
            err("`for ref` cannot be used on a `const` binding; write `for const ref` for a read-only "
                "iteration",
                s->seq.get());
        }
        if (s->by_ref && !simple_seq(s->seq.get())) {
            err("the sequence of `for ref` must be a variable or member access (an lvalue)", s->seq.get());
        }

        TypePtr vt = s->elem_ty;
        auto lv = std::make_unique<LocalVar>();
        lv->name = s->var;
        lv->rty = vt;
        lv->by_ref = s->by_ref;
        lv->ref_const = s->ref_const;
        lv->line = s->line;
        lv->col = s->col;
        declare_local(lv.get());
        s->var_ty = vt;
        s->lvar = std::move(lv);

        if (s->has_idx) {
            TypePtr us = Type::make_prim("isize");
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
        maybe_uninit_ = uninitAtEntry;
    }

    // C-style `for (init; cond; step)`: init, cond and step share one scope with the body (C semantics),
    // so the init declaration is visible in the condition and the body. `continue` runs the step clause,
    // which is why the step is checked inside the loop (a `move` in it counts as a loop move). The init
    // clause itself runs once, before the loop, so it is checked outside that move level.
    void Sema::check_for_c(ForStmt* s) {
        ScopeGuard guard(*this);  // pops the scope and restores the loop/move counters on a throw
        for (auto& st : s->init) check_stmt(st.get());
        if (s->cond) check_cond(s->cond.get(), "for condition");
        const auto uninitBeforeBody = maybe_uninit_;  // init runs once; the body may not run
        ++loop_depth_;
        ++move_loop_count_;
        if (s->step) check_stmt(s->step.get());
        check_block(s->body.get());
        --move_loop_count_;
        --loop_depth_;
        maybe_uninit_ = uninitBeforeBody;
    }
}  // namespace muc_detail
