// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Block and statement checking (definitions of Sema members).
//
// owns:      block/statement bodies — locals and their initializers, if/while/for/switch/return, `para`
//            sections — and the initializer-into-storage helpers shared with declarations
// not:       expression typing, call resolution, declarations, codegen
// pass:      body checking (the control-flow half lives in sema_class2.cpp)
// concept:   statements, locals, initializers

#include "sema_impl.hpp"

#include "ast_clone.hpp"

namespace muc_detail {

    void Sema::check_block(Block* b) {
        if (!b) return;
        push();
        // A statement-list `#if` is expanded in place (P3.3): the unit's constants are complete, so the
        // condition uses the same values ordinary code does.
        for (size_t i = 0; i < b->stmts.size();) {
            if (dynamic_cast<DirectiveStmt*>(b->stmts[i].get())) {
                expand_directive_stmt(b, i);
                continue;
            }
            check_stmt(b->stmts[i].get());
            ++i;
        }
        pop();
    }

    // Splice every statement-list `#if` of this block in place, without checking the statements. The
    // constructor path needs the list expanded before it looks for a leading `super(...)`.
    void Sema::expand_directive_stmts(Block* b) {
        if (!b) return;
        for (size_t i = 0; i < b->stmts.size();) {
            if (dynamic_cast<DirectiveStmt*>(b->stmts[i].get())) {
                expand_directive_stmt(b, i);
                continue;
            }
            ++i;
        }
    }

    void Sema::check_stmt(Stmt* s) {
        if (!s) return;
        if (auto lv = dynamic_cast<LocalVar*>(s)) {
            check_local(lv);
        } else if (auto es = dynamic_cast<ExprStmt*>(s)) {
            check_expr(es->expr.get());
        } else if (auto as = dynamic_cast<AssignStmt*>(s)) {
            check_assign_stmt(as);
        } else if (auto iff = dynamic_cast<IfStmt*>(s)) {
            check_cond(iff->cond.get(), "if condition");
            // `if x is P(v) { ... }`: the bound names are locals of the then-branch only (SPEC 10.1.2).
            IsExpr* ise = dynamic_cast<IsExpr*>(iff->cond.get());
            // A `move` inside either arm leaves the variable moved on that path only; after the `if` the
            // variable counts as moved if *any* arm moved it (may-move analysis).
            const auto movedAtEntry = moved_locals_;
            push();
            if (ise && !ise->binds.empty()) {
                for (size_t k = 0; k < ise->binds.size(); ++k) {
                    auto lv = std::make_unique<LocalVar>();
                    lv->name = ise->binds[k];
                    lv->rty = ise->bind_types[k];
                    lv->line = ise->line;
                    lv->col = ise->col;
                    declare_local(lv.get());
                    ise->bind_locals.push_back(lv.get());
                    if (cur_func_) cur_func_->synthetic_locals.push_back(std::move(lv));
                }
            }
            check_block(iff->then.get());
            pop();
            const auto afterThen = moved_locals_;
            moved_locals_ = movedAtEntry;
            if (iff->els) {
                if (auto b = dynamic_cast<Block*>(iff->els.get())) {
                    check_block(b);
                } else {
                    check_stmt(iff->els.get());
                }
            }
            for (LocalVar* lv : afterThen) moved_locals_.insert(lv);
            for (LocalVar* lv : movedAtEntry) moved_locals_.insert(lv);
        } else if (auto ws = dynamic_cast<WhileStmt*>(s)) {
            check_cond(ws->cond.get(), "while condition");
            ++loop_depth_;
            ++move_loop_count_;
            check_block(ws->body.get());
            --move_loop_count_;
            --loop_depth_;
        } else if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            ++move_loop_count_;
            check_for(fs);
            --move_loop_count_;
        } else if (auto cs = dynamic_cast<ForStmt*>(s)) {
            ++move_loop_count_;
            check_for_c(cs);
            --move_loop_count_;
        } else if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            check_switch(sw);
        } else if (dynamic_cast<BreakStmt*>(s)) {
            if (in_finally_) err("`break` is not allowed inside `finally`", s);
            if (!para_stack_.empty() && loop_depth_ == para_stack_.back().loop_base)
                err("`break` cannot leave a `para` section", s);
            if (in_para_for_body_ && loop_depth_ == para_for_body_depth_)
                err("`break` cannot leave a `para for` loop", s);
            if (loop_depth_ == 0) err("`break` can only appear in a loop", s);
        } else if (dynamic_cast<ContinueStmt*>(s)) {
            if (in_finally_) err("`continue` is not allowed inside `finally`", s);
            if (!para_stack_.empty() && loop_depth_ == para_stack_.back().loop_base)
                err("`continue` cannot leave a `para` section", s);
            if (in_para_for_body_ && loop_depth_ == para_for_body_depth_)
                err("`continue` cannot leave a `para for` loop", s);
            if (loop_depth_ == 0) err("`continue` can only appear in a loop", s);
        } else if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            if (in_finally_) err("`return` is not allowed inside `finally`", rs);
            check_return(rs);
        } else if (auto ts = dynamic_cast<ThrowStmt*>(s)) {
            if (in_finally_) err("`throw` is not allowed inside `finally`; an escaping exception aborts", ts);
            if (in_dtor_) err("a destructor must not throw", ts);
            TypePtr t = check_expr(ts->value.get());
            TypePtr ex = exception_type();
            if (!ex) err("std.exception.Exception is not available", ts);
            if (!t || t->kind != TypeKind::Class || !is_subclass_of(t, ex)) {
                err("`throw` requires an `Exception` subclass, found " + (t ? t->str() : std::string("void")),
                    ts);
            }
        } else if (auto tr = dynamic_cast<TryStmt*>(s)) {
            check_block(tr->body.get());
            TypePtr ex = exception_type();
            if (!ex) err("std.exception.Exception is not available", tr);
            for (auto& c : tr->catches) {
                TypePtr ct = resolve_type(c->spec.get(), cur_);
                c->rty = ct;
                if (!ct || ct->kind != TypeKind::Class || !is_subclass_of(ct, ex)) {
                    err("`catch` type must be an `Exception` subclass, found " +
                            (ct ? ct->str() : std::string("void")),
                        c.get());
                }
                auto lv = std::make_unique<LocalVar>();
                lv->name = c->name;
                lv->rty = ct;
                lv->is_const = true;
                lv->line = c->line;
                lv->col = c->col;
                c->var = std::move(lv);
                push();
                declare_local(c->var.get());
                check_block(c->body.get());
                pop();
            }
            if (tr->finally_body) {
                const bool saved = in_finally_;
                in_finally_ = true;
                check_block(tr->finally_body.get());
                in_finally_ = saved;
            }
        } else if (auto b = dynamic_cast<Block*>(s)) {
            check_block(b);
        } else if (auto pp = dynamic_cast<ParaStmt*>(s)) {
            check_para(pp);
        } else if (auto pf = dynamic_cast<ParaForStmt*>(s)) {
            check_para_for(pf);
        } else if (auto es = dynamic_cast<ErrorStmt*>(s)) {
            // A selected `#error` arm (only reachable through a spliced `#if`).
            err("#error " + es->message, es);
        } else {
            err("unknown statement", s);
        }
    }

    void Sema::check_para(ParaStmt* p) {
        if (single_thread_) {
            // `--single-thread` drops the atomics from reference counting, so a second thread would
            // race on every shared object: reject instead of silently generating a data race.
            err("`para` needs atomic reference counting; it cannot be used with `--single-thread`", p);
            return;
        }
        p->captures.clear();
        for (auto& sec : p->sections) {
            ParaCtx ctx;
            ctx.scope_base = scopes_.size();
            ctx.loop_base = loop_depth_;
            ctx.lambda_base = lambda_stack_.size();
            para_stack_.push_back(std::move(ctx));
            push();
            try {
                check_block(sec.get());
            } catch (...) {
                // `err` throws; the caller may continue with the next declaration, so the para
                // bookkeeping must not leak into it (a stale entry would flag unrelated code).
                pop();
                para_stack_.pop_back();
                throw;
            }
            pop();
            ParaCtx done = std::move(para_stack_.back());
            para_stack_.pop_back();
            std::vector<LocalVar*> caps;
            for (LocalVar* lv : done.seen) {
                if (!done.declared.count(lv)) caps.push_back(lv);
            }
            p->captures.push_back(std::move(caps));
        }
    }

    void Sema::check_para_for(ParaForStmt* p) {
        if (single_thread_) {
            err("`para for` needs atomic reference counting; it cannot be used with `--single-thread`", p);
            return;
        }
        ForInStmt* f = p->loop.get();
        if (!f) return;
        if (f->has_idx) {
            err("`para for i, x` is not supported yet: iterate values and index explicitly", f);
        }
        ParaCtx ctx;
        ctx.scope_base = scopes_.size();
        ctx.loop_base = loop_depth_;
        ctx.lambda_base = lambda_stack_.size();
        para_stack_.push_back(std::move(ctx));
        pending_para_for_ = true;
        try {
            check_for(f);
        } catch (...) {
            pending_para_for_ = false;
            para_stack_.pop_back();
            throw;
        }
        pending_para_for_ = false;
        ParaCtx done = std::move(para_stack_.back());
        para_stack_.pop_back();
        p->captures.clear();
        for (LocalVar* lv : done.seen) {
            if (!done.declared.count(lv)) p->captures.push_back(lv);
        }
    }

    bool Sema::roots_at_outer_value_local(Expr* e) const {
        if (!e || para_stack_.empty()) return false;
        LocalVar* root = nullptr;
        Expr* cur = e;
        while (cur) {
            if (auto n = dynamic_cast<Name*>(cur)) {
                if (auto lv = std::get_if<LocalVar*>(&n->sym)) root = *lv;
                break;
            }
            if (auto m = dynamic_cast<Member*>(cur)) {
                TypePtr ot = m->obj->ty;
                if (ot && (ot->kind == TypeKind::Class || ot->kind == TypeKind::Interface ||
                           ot->kind == TypeKind::Ptr))
                    return false;  // storage belongs to an object / pointed-to memory, not the caller's frame
                cur = m->obj.get();
                continue;
            }
            if (auto ix = dynamic_cast<Index*>(cur)) {
                TypePtr bt = ix->base->ty;
                if (bt && (bt->kind == TypeKind::Class || bt->kind == TypeKind::Interface ||
                           bt->kind == TypeKind::Ptr || bt->kind == TypeKind::Slice))
                    return false;  // heap buffer or explicitly shared memory
                cur = ix->base.get();
                continue;
            }
            return false;
        }
        if (!root) return false;
        if (para_stack_.back().declared.count(root)) return false;
        if (root->by_ref) return false;  // explicit borrow
        if (root->rty && (root->rty->kind == TypeKind::Class || root->rty->kind == TypeKind::Interface))
            return false;  // the handle is a value, but the object it points at is shared explicitly
        return true;
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
            // Several auto declarators in one declaration statement must infer the same type
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

        // A class handle / interface value *refers to* an object, it is not storage for one, so a bare
        // `Dog d;` reads like a stack construction while building nothing (SPEC 4). Requiring the
        // initializer keeps the two apart at the declaration site instead of at the first use.
        if (!lv->init && lv->rty &&
            (lv->rty->kind == TypeKind::Class || lv->rty->kind == TypeKind::Interface)) {
            const bool is_class = lv->rty->kind == TypeKind::Class;
            err("`" + lv->name + "` is a reference to a `" + lv->rty->name +
                    "` object, not an object: initialize it explicitly (`= null`, " +
                    (is_class ? "`= new " + lv->rty->name + "(...)`" : "an existing value") +
                    ", or an existing reference)",
                lv);
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
                if (call->callee_kind == "struct_ctor") return;  // in-place construction
                if (same_type(call->ty, ty)) return;             // same-type returned value: copy initialization
                // Anything else may still be a copy-initialization through an implicit constructor:
                // `string c = s.cstr();` has the same type as `const char* p = s.cstr();` followed by
                // `string b = p;`, so the coercion rules of assignment apply here too (SPEC 3.6).
                check_assign(e, ty, what);
                return;
            }
            // Field-wise construction of a type without a constructor (`Token(5)`, `Token(v = 5)`):
            // a construction is not a copy, so a `nocopy` type may still be built this way (SPEC 3.4).
            if (auto call = dynamic_cast<Call*>(e);
                call && ty->kind == TypeKind::Struct && ty->ctors.empty() &&
                call->callee_kind == "ctor" && same_type(call->ty, ty)) {
                check_expr(e, ty);
                return;
            }
            if (ty->kind == TypeKind::Array && is_char(ty->elem) &&
                dynamic_cast<StringLit*>(e)) {
                // A string literal initializes a fixed-size char/byte array (copied + NUL padded, 3.4).
                const std::string& bytes = static_cast<StringLit*>(e)->value;
                if (bytes.size() >= ty->array_size) {
                    err("string literal (" + std::to_string(bytes.size()) + " bytes) does not fit in `" +
                            ty->str() + "` (it needs room for the terminating NUL)",
                        e);
                }
                check_expr(e);
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
