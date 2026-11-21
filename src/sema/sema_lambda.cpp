// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Lambdas and function values (definitions of Sema members).
//
// owns:      `check_lambda` (parameter types, the synthesized environment class, captures) and
//            `func_value_type` (turning an overload set into a function value)
// not:       call checking through a function value (sema_call.cpp), other expressions
// pass:      body checking
// concept:   lambdas

#include "sema_impl.hpp"

#include "ast_clone.hpp"

namespace muc_detail {

    TypePtr Sema::func_value_type(const std::vector<FuncDecl*>& cands, const std::string& what,
                                  const TypePtr& expected, FuncDecl** chosen, Node* at) {
        std::vector<FuncDecl*> ok;
        bool has_ref = false;
        for (auto* f : cands) {
            if (!f || !f->rty) continue;
            bool refp = false;
            for (bool b : f->pty_ref) refp = refp || b;
            if (refp || is_borrow_type(f->rty)) {
                has_ref = true;
                continue;
            }
            if (f->is_varargs) continue;
            ok.push_back(f);
        }
        if (ok.empty()) {
            if (has_ref) {
                err("function `" + what +
                        "` cannot be used as a value: `ref` parameters and `ref` returns have no function-value form",
                    at);
            }
            err("function `" + what + "` cannot be used as a value", at);
        }
        if (ok.size() == 1) {
            if (chosen) *chosen = ok[0];
            return Type::make_func(ok[0]->pty, ok[0]->rty);
        }
        if (expected && (expected->kind == TypeKind::Func || expected->kind == TypeKind::Delegate)) {
            // A delegate target carries the same parameter/return types; a plain function value
            // converts to a delegate at the coercion site (#192).
            TypePtr want = expected->kind == TypeKind::Delegate
                               ? Type::make_func(expected->fparams, expected->fret)
                               : expected;
            for (auto* f : ok) {
                TypePtr ft = Type::make_func(f->pty, f->rty);
                if (same_type(ft, want)) {
                    if (chosen) *chosen = f;
                    return ft;
                }
            }
            err("no overload of `" + what + "` matches the target type `" + expected->str() + "`", at);
        }
        err("overloaded function `" + what + "` needs a target type to be used as a value", at);
        return nullptr;
    }

    TypePtr Sema::check_lambda(Lambda* e) {
        // The node may be checked twice (`auto` deduces the type and then the initializer is converted again):
        // the lambda is lowered only once.
        if (e->synth) {
            // A capturing lambda's value is a delegate; its signature excludes the environment.
            const auto& p = e->synth->pty;
            std::vector<TypePtr> ps = e->env_ty ? std::vector<TypePtr>(p.begin() + 1, p.end()) : p;
            return e->env_ty ? Type::make_delegate(ps, e->synth->rty)
                             : Type::make_func(ps, e->synth->rty);
        }

        // ---- capture list (U41) ---------------------------------------------------------------
        // `[x]` copies the value into the environment object; `[ref x]` stores its address, so writes
        // reach the captured variable (the caller keeps it alive; escaping the delegate is UB);
        // `[weak x]` stores an uncounted reference, so the environment does not keep the object alive.
        std::vector<const LocalVar*> cap_src;
        std::vector<bool> cap_ref;
        std::vector<bool> cap_weak;
        TypePtr env_ty;
        int this_cap_index = -1;
        TypePtr this_cap_ty;
        bool this_cap_ref = false;
        if (!e->captures.empty()) {
            std::vector<TypePtr> field_types;
            for (auto& cap : e->captures) {
                if (cap.by_weak && cap.by_ref) {
                    err("a capture cannot be both `weak` and `ref`: `[weak " + cap.name +
                            "]` already stores a reference, `[ref …]` hands out storage",
                        e);
                }
                // `para` sections may read enclosing locals but not write them (SPEC 3.17.2), so a
                // capture that hands out storage (`[ref …]`) is rejected there; `this` is not usable
                // inside a section at all in v1.
                if (!para_stack_.empty()) {
                    if (cap.by_ref) {
                        err("a `para` section cannot capture `ref " + cap.name +
                                "`: the section must not write enclosing locals (pass a `class` object or "
                                "copy the value with `[" + cap.name + "]`)",
                            e);
                    }
                    if (cap.name == "this") {
                        err("`this` cannot be used inside a `para` section in v1: pass the object as an "
                            "explicit argument",
                            e);
                    }
                }
                if (cap.name == "this") {
                    // `[this]`: the enclosing method's receiver. A class captures the handle (the
                    // environment keeps the object alive); a struct captures a copy, or its address
                    // with `[ref this]` so writes reach the original object (U41 decision).
                    // `[weak this]` stores the handle uncounted, which is how a callback stored on the
                    // object itself avoids an RC cycle.
                    if (!cur_class_) {
                        err("`[this]` can only appear in a method, constructor or destructor", e);
                    }
                    if (this_cap_index >= 0) err("`this` is captured more than once", e);
                    TypePtr vt = cur_class_;
                    TypePtr ft = vt;
                    if (cap.by_weak && vt->kind != TypeKind::Class) {
                        err("`[weak this]` needs a class receiver: a struct `this` is a value copy and "
                            "cannot form an RC cycle",
                            e);
                    }
                    if (cap.by_ref) {
                        if (vt->kind == TypeKind::Class) {
                            err("`[ref this]` is meaningless for a class: `this` is already a reference; "
                                "write `[this]`",
                                e);
                        }
                        if (!ref_capturable_type(vt)) {
                            err("`[ref this]`: struct `" + vt->str() +
                                    "` cannot be captured by reference (it has reference-counted fields or "
                                    "a destructor)",
                                e);
                        }
                        ft = Type::make_ptr(vt);
                    } else if (vt->kind == TypeKind::Struct && noncopyable_struct(vt)) {
                        err("`[this]`: struct `" + vt->str() +
                                "` has a destructor and cannot be captured by value (use `[ref this]`)",
                            e);
                    }
                    field_types.push_back(ft);
                    cap_src.push_back(nullptr);  // `this` has no enclosing LocalVar to read from
                    cap_ref.push_back(cap.by_ref);
                    cap_weak.push_back(cap.by_weak);
                    this_cap_index = static_cast<int>(field_types.size() - 1);
                    this_cap_ty = vt;
                    this_cap_ref = cap.by_ref;
                    continue;
                }
                LocalVar* lv = lookup_local(cap.name);
                if (!lv) err("`" + cap.name + "` is not a local variable, so it cannot be captured", e);
                // A capture may only name a variable of the *immediately* enclosing function: a variable
                // belonging to an outer function would not survive (or would silently read a fresh
                // slot), so it must be captured by the enclosing lambda first.
                if (!lambda_stack_.empty()) {
                    const std::size_t base = lambda_stack_.back().scope_base;
                    std::size_t owner = scopes_.size();
                    for (std::size_t i = 0; i < scopes_.size(); ++i) {
                        auto it = scopes_[i].find(cap.name);
                        if (it != scopes_[i].end() && it->second == lv) { owner = i; break; }
                    }
                    if (owner < base) {
                        err("`" + cap.name + "` belongs to an outer function: capture it in the enclosing "
                                "lambda first (`[" + cap.name + "]` there), then capture the copy here",
                            e);
                    }
                }
                for (auto* prev : cap_src) {
                    if (prev == lv) err("`" + cap.name + "` is captured more than once", e);
                }
                const TypePtr& vt = lv->rty;
                TypePtr ft = vt;
                if (cap.by_weak && vt->kind != TypeKind::Class) {
                    err("`[weak " + cap.name + "]`: `" + vt->str() +
                            "` is not a class handle; a weak capture needs a reference-counted class "
                            "(interfaces do not support `weak` yet)",
                        e);
                }
                if (cap.by_ref) {
                    if (!ref_capturable_type(vt)) {
                        err("`[ref " + cap.name + "]`: `" + vt->str() +
                                "` cannot be captured by reference (only scalars, enums, pointers and "
                                "value structs; a borrow cannot hold reference-counted fields)",
                            e);
                    }
                    ft = Type::make_ptr(vt);
                } else if (noncopyable_struct(vt)) {
                    err("`[" + cap.name + "]`: struct `" + vt->str() +
                            "` has a destructor and cannot be captured by value (use `[ref " + cap.name +
                            "]` or keep it in a `class`)",
                        e);
                }
                field_types.push_back(ft);
                cap_src.push_back(lv);
                cap_ref.push_back(cap.by_ref);
                cap_weak.push_back(cap.by_weak);
            }
            // The environment is a synthesized class with one field per capture: the existing class
            // machinery gives it layout, a descriptor and a destructor that releases RC fields.
            std::string env_name = "$env$" + std::to_string(++lambda_seq_);
            env_ty = Type::make_class(cur_->name, env_name);
            for (size_t i = 0; i < field_types.size(); ++i) {
                Field f;
                f.name = "c" + std::to_string(i);
                f.type = field_types[i];
                f.is_public = false;
                // `[weak x]`: the environment must not own the object, so the field is a weak reference
                // (the class destructor only unregisters it, and the target's destroy clears it).
                f.weak = cap_weak[i];
                env_ty->fields.push_back(std::move(f));
            }
            auto cd = std::make_unique<ClassDecl>();
            cd->name = env_name;
            cd->line = e->line;
            cd->col = e->col;
            cd->ty = env_ty;
            synth_decls_.push_back(std::move(cd));
        }

        auto fn = std::make_unique<FuncDecl>();
        fn->name = "$lambda$" + std::to_string(++lambda_seq_);
        fn->line = e->line;
        fn->col = e->col;
        for (auto& p : e->params) {
            TypePtr pt = decay_param_type(resolve_type(p->spec.get(), cur_));
            if (is_void(pt)) err("lambda parameter `" + p->name + "` cannot be `void`", p.get());
            if (is_borrow_type(pt)) err("a lambda cannot take a `ref` parameter", p.get());
            fn->pty.push_back(pt);
        }
        fn->pty_ref.assign(fn->pty.size(), false);
        fn->pty_cref.assign(fn->pty.size(), false);

        // The environment is the hidden first argument of the generated function; the delegate's own
        // signature starts after it.
        if (env_ty) {
            fn->pty.insert(fn->pty.begin(), env_ty);
            fn->pty_ref.insert(fn->pty_ref.begin(), false);
            fn->pty_cref.insert(fn->pty_cref.begin(), false);
        }
        e->env_ty = env_ty;
        e->env_src = cap_src;
        e->env_src_ref = cap_ref;
        e->env_src_weak = cap_weak;
        for (size_t i = 0; i < cap_weak.size(); ++i) {
            if (cap_weak[i]) fn->weak_capture_indices.push_back(static_cast<int>(i));
        }

        // A lambda is its own function: no `this`, no outer loop, no enclosing `finally` (and no captures, checked in check_name).
        FuncDecl* saved_func = cur_func_;
        TypePtr saved_ret = cur_ret_;
        std::string saved_name = cur_name_;
        TypePtr saved_class = cur_class_;
        bool saved_ctor = in_ctor_, saved_dtor = in_dtor_, saved_finally = in_finally_;
        int saved_loop = loop_depth_;
        auto saved_this = std::move(this_local_);
        this_local_.clear();

        cur_func_ = fn.get();
        cur_ret_ = nullptr;
        cur_name_ = "lambda";
        cur_class_ = nullptr;
        in_ctor_ = false;
        in_dtor_ = false;
        in_finally_ = false;
        loop_depth_ = 0;

        push();
        LambdaCtx ctx;
        ctx.scope_base = scopes_.size() - 1;
        lambda_stack_.push_back(ctx);

        struct Defer {
            Sema* s;
            FuncDecl* func;
            TypePtr ret;
            std::string name;
            TypePtr cls;
            bool ctor, dtor, fin;
            int loop;
            std::vector<std::unique_ptr<LocalVar>> thisv;
            ~Defer() {
                s->cur_func_ = func;
                s->cur_ret_ = ret;
                s->cur_name_ = name;
                s->cur_class_ = cls;
                s->in_ctor_ = ctor;
                s->in_dtor_ = dtor;
                s->in_finally_ = fin;
                s->loop_depth_ = loop;
                s->this_local_ = std::move(thisv);
                if (!s->lambda_stack_.empty()) s->lambda_stack_.pop_back();
                s->pop();
            }
        } defer{this,
                 saved_func,
                 saved_ret,
                 saved_name,
                 saved_class,
                 saved_ctor,
                 saved_dtor,
                 saved_finally,
                 saved_loop,
                 std::move(saved_this)};

        if (env_ty) {
            auto ev = std::make_unique<LocalVar>();
            ev->name = "$env";
            ev->rty = env_ty;
            ev->line = e->line;
            ev->col = e->col;
            declare_local(ev.get());
            fn->param_locals.push_back(std::move(ev));
        }
        for (size_t i = 0; i < e->params.size(); ++i) {
            Param* p = e->params[i].get();
            auto lv = std::make_unique<LocalVar>();
            lv->name = p->name;
            // The generated function's parameter 0 is the environment, so shift by one.
            lv->rty = fn->pty[env_ty ? i + 1 : i];
            lv->line = p->line;
            lv->col = p->col;
            declare_local(lv.get());
            fn->param_locals.push_back(std::move(lv));
        }
        // Captures are ordinary locals of the lambda body whose storage is the environment field;
        // codegen maps `env_index` to `env->field` (U41).
        for (size_t i = 0; i < cap_src.size(); ++i) {
            bool is_this = e->captures[i].name == "this";
            auto lv = std::make_unique<LocalVar>();
            lv->name = e->captures[i].name;
            lv->rty = is_this ? this_cap_ty : cap_src[i]->rty;
            lv->by_ref = cap_ref[i];
            lv->env_index = static_cast<int>(i);
            lv->line = e->captures[i].line;
            lv->col = e->captures[i].col;
            declare_local(lv.get());
            if (is_this) {
                // The body may use `this` and implicit field access: keep a this-local pointing at the
                // environment field (`Defer` restores the enclosing method's `this_local_`).
                this_local_.clear();
                this_local_.push_back(std::move(lv));
                fn->this_capture_ty = this_cap_ty;
                fn->this_capture_index = this_cap_index;
                fn->this_capture_ref = this_cap_ref;
                cur_class_ = this_cap_ty;
            } else {
                fn->capture_locals.push_back(std::move(lv));
            }
        }

        TypePtr retty;
        if (e->expr_body) {
            TypePtr bt = check_expr(e->expr_body.get());
            auto blk = std::make_unique<Block>();
            blk->line = e->line;
            blk->col = e->col;
            if (bt && is_void(bt)) {
                auto st = std::make_unique<ExprStmt>();
                st->expr = std::move(e->expr_body);
                st->line = e->line;
                st->col = e->col;
                blk->stmts.push_back(std::move(st));
                retty = t_void();
            } else {
                auto st = std::make_unique<ReturnStmt>();
                st->value = std::move(e->expr_body);
                st->line = e->line;
                st->col = e->col;
                blk->stmts.push_back(std::move(st));
                retty = bt;
            }
            fn->body = std::move(blk);
        } else {
            check_block(e->block_body.get());
            LambdaCtx& lc = lambda_stack_.back();
            if (lc.rets.empty()) {
                retty = t_void();
            } else {
                if (!lc.bare.empty()) {
                    err("lambda mixes `return;` with `return <value>;`", lc.bare.front());
                }
                retty = lc.rets.front().second;
                for (auto& [node, t] : lc.rets) {
                    if (!same_type(t, retty)) {
                        err("lambda return types differ: `" + retty->str() + "` and `" +
                                (t ? t->str() : std::string("void")) + "`; cast one with `as`",
                            node);
                    }
                }
            }
            if (!is_void(retty) && !guarantees_return(e->block_body.get())) {
                err("lambda may reach the end of its body without returning a value", e);
            }
            fn->body = std::move(e->block_body);
        }

        fn->rty = retty;
        fn->cname = func_cname(cur_->name, fn.get());
        // The synthesized name must not collide with a user function whose identifier sanitizes to the same symbol
        // (`$lambda$1` and `_lambda_1` both become `_lambda_1`).
        auto cname_taken = [&](const std::string& cn) {
            for (auto& d : cur_->decls) {
                auto* fd = dynamic_cast<FuncDecl*>(d.get());
                if (fd && fd->cname == cn) return true;
            }
            for (auto& l : cur_->lambdas) {
                if (l->cname == cn) return true;
            }
            return false;
        };
        while (cname_taken(fn->cname)) {
            fn->name = "$lambda$" + std::to_string(++lambda_seq_);
            fn->cname = func_cname(cur_->name, fn.get());
        }
        std::vector<TypePtr> sig = fn->pty;
        if (env_ty) sig.erase(sig.begin());  // the environment is not part of the delegate signature
        TypePtr ft = env_ty ? Type::make_delegate(sig, retty) : Type::make_func(sig, retty);
        cur_->lambdas.push_back(std::move(fn));
        e->synth = cur_->lambdas.back().get();
        return ft;
    }

}  // namespace muc_detail
