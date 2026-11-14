// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds expression type checking (definitions of Sema members).
#include "sema_impl.hpp"

namespace muc_detail {

    TypePtr Sema::check_expr(Expr* e, const TypePtr& expected) {
        if (!e) err("missing expression");
        TypePtr t = check_expr_impl(e, expected);
        e->ty = t;
        return t;
    }

    TypePtr Sema::check_expr_impl(Expr* e, const TypePtr& expected) {
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (is_prim(expected) && expected->is_numeric() &&
                promote_int_literal(il->value, expected)) {
                return expected;
            }
            if (il->value >= -(1LL << 31) && il->value < (1LL << 31)) return t_int();
            // long long is already within the int64 range; anything beyond 64 bits is reported by the lexer/parser
            return Type::make_prim("long");
        }

        if (auto fl = dynamic_cast<FloatLit*>(e)) {
            if (is_prim(expected) && (expected->name == "float" || expected->name == "double")) {
                return expected;
            }
            return Type::make_prim(fl->is_float32 ? "float" : "double");
        }

        if (auto cl = dynamic_cast<CharLit*>(e)) {
            (void)cl;
            if (expected && is_prim(expected) && expected->is_integer() &&
                expected->bits() == 8) {
                return expected;
            }
            return Type::make_prim("byte");  // a character literal is a byte by default
        }
        // A string literal is a static, NUL-terminated UTF-8 byte buffer: its type is `const char*`,
        // like C. Converting it to the library `string` goes through `implicit string(const char*)` (3.6).
        // The const sits on the *referent* (elem level), exactly like a written `const char*`
        // (sema_types.cpp); a pointer-level const would be `char* const` and would let `char*` bind
        // to a literal, since the const-relaxation checks look at the elem.
        if (dynamic_cast<StringLit*>(e)) {
            auto ch = std::make_shared<Type>(*Type::make_prim("char"));
            ch->is_const = true;
            return Type::make_ptr(ch);
        }
        if (dynamic_cast<BoolLit*>(e)) return t_bool();

        if (dynamic_cast<NullLit*>(e)) {
            if (expected) {
                const auto k = expected->kind;
                if (k == TypeKind::Ptr || k == TypeKind::Slice ||
                    k == TypeKind::Class || k == TypeKind::Interface) {
                    return expected;
                }
            }
            return t_null();
        }

        if (auto nm = dynamic_cast<Name*>(e)) return check_name(nm, expected);
        if (auto m = dynamic_cast<Member*>(e)) return check_member(m, expected);
        if (auto lm = dynamic_cast<Lambda*>(e)) return check_lambda(lm);

        if (auto ix = dynamic_cast<Index*>(e)) {
            TypePtr bt = check_expr(ix->base.get());
            if (ix->indices.empty()) err("subscript list cannot be empty", e);
            std::vector<TypePtr> its;
            for (auto& idxExpr : ix->indices) {
                TypePtr it = check_expr(idxExpr.get());
                if (is_enum_t(it)) {
                    err("subscript cannot be an enum `" + it->str() + "` (write `x as int`)", idxExpr.get());
                }
                if (!is_integer(it)) {
                    err("subscript must be an integer, found " + (it ? it->str() : "void"),
                        idxExpr.get());
                }
                its.push_back(it);
            }
            if (!bt) err("type cannot be subscripted", e);
            const size_t n = ix->indices.size();
            if (bt->kind == TypeKind::Array) {
                TypePtr cur = bt;
                size_t depth = 0;
                while (cur && cur->kind == TypeKind::Array) {
                    cur = cur->elem;
                    depth++;
                }
                if (n > depth) {
                    err("too many subscripts: `" + bt->str() + "` has only " + std::to_string(depth) +
                            " dimensions",
                        e);
                }
                TypePtr r = bt;
                for (size_t i = 0; i < n; ++i) r = r->elem;
                return r;
            }
            if (bt->kind == TypeKind::Slice) {
                if (static_cast<int>(n) != bt->rank) {
                    err("`" + bt->str() + "` requires " + std::to_string(bt->rank) +
                            " subscripts (got " + std::to_string(n) + ")",
                        e);
                }
                return bt->elem;
            }
            if (bt->kind == TypeKind::Ptr) {
                if (n != 1) err("pointer accepts exactly one subscript", e);
                return bt->elem;
            }
            if (is_user_type(bt)) {
                std::vector<Expr*> idx_args;
                idx_args.reserve(n);
                for (auto& ie : ix->indices) idx_args.push_back(ie.get());
                if (const MethodSig* mm = find_member_operator(bt, "operator[]", idx_args, e)) {
                    ix->get_method = mm;
                    ix->by_ref = is_borrow_type(mm->ret);
                    ix->ref_const = ix->by_ref && mm->ret->is_const;
                    return referent_of(mm->ret);
                }
                std::vector<TypePtr> opnds{bt};
                for (auto& it : its) opnds.push_back(it);
                auto cands = collect_operators("[]", opnds);
                if (cands.empty()) {
                    err("type `" + bt->str() + "` does not support subscripting with " +
                            std::to_string(n) + " index" + (n == 1 ? "" : "es") +
                            " (no matching `operator[]`)",
                        e);
                }
                std::vector<Expr*> call_args{ix->base.get()};
                for (auto& ie : ix->indices) call_args.push_back(ie.get());
                auto f = choose_overload(cands, call_args, "operator[]", e);
                ix->get_func = f;
                ix->by_ref = is_borrow_type(f->rty);
                ix->ref_const = ix->by_ref && f->rty->is_const;
                return referent_of(f->rty);
            }
            err("type " + bt->str() + " cannot be subscripted", e);
        }

        if (auto u = dynamic_cast<Unary*>(e)) return check_unary(u);

        if (auto nt = dynamic_cast<NotExpr*>(e)) {
            TypePtr t = check_expr(nt->operand.get());
            if (!(is_prim(t) && t->name == "bool")) {
                err("`not` requires a `bool` operand, found " + (t ? t->str() : "void"), e);
            }
            return t_bool();
        }

        if (auto b = dynamic_cast<Binary*>(e)) return check_binary(b);

        if (auto te = dynamic_cast<Ternary*>(e)) return check_ternary(te, expected);

        if (auto c = dynamic_cast<Cast*>(e)) {
            TypePtr st = check_expr(c->expr.get());
            TypePtr dt = resolve_type(c->spec.get(), cur_);
            if (!can_cast(st, dt)) {
                err("cannot use `as` to convert " + (st ? st->str() : "void") + " to " + dt->str(), e);
            }
            e->ty = dt;
            return dt;
        }

        if (auto ne = dynamic_cast<NewExpr*>(e)) return check_new(ne);

        if (auto c = dynamic_cast<Call*>(e)) return check_call(c);

        if (auto sz = dynamic_cast<SizeofExpr*>(e)) {
            TypePtr t;
            if (sz->expr) {
                // `sizeof expr`: only the operand's type matters; the operand is never evaluated.
                // A lone name may be a type (an alias imported from another unit, which the parser cannot
                // recognize in this position): values win, then the type, then a normal value check.
                if (auto nm = dynamic_cast<Name*>(sz->expr.get());
                    nm && lookup_local(nm->ident) == nullptr &&
                    info().globals.find(nm->ident) == info().globals.end()) {
                    t = find_type_in_expr(nm->ident, nm);
                }
                if (!t) t = check_expr(sz->expr.get());
                if (t && (is_void(t) || t->kind == TypeKind::Func || t->kind == TypeKind::Null ||
                          t->kind == TypeKind::Range)) {
                    err("`" + sz->which + "` cannot be applied to " + t->str(), sz->expr.get());
                }
            } else {
                t = resolve_type(sz->spec.get(), cur_);
                if (is_void(t)) err("`" + sz->which + "(void)` is invalid", e);
            }
            sz->spec_ty = t;  // codegen uses the folded value / resolved type, never the syntax again
            if (sz->which == "offsetof") {
                if (!t || (t->kind != TypeKind::Struct)) {
                    err("`offsetof` requires a struct type, found " + (t ? t->str() : "void"), e);
                }
                int idx = -1;
                for (size_t i = 0; i < t->fields.size(); ++i) {
                    if (t->fields[i].name == sz->field) idx = static_cast<int>(i);
                }
                if (idx < 0) {
                    err("`offsetof`: " + t->str() + " has no field `" + sz->field + "`", e);
                }
                sz->cval = type_member_offset(t, static_cast<size_t>(idx));
            } else {
                sz->cval = sz->which == "alignof" ? type_align(t) : type_size(t);
            }
            e->ty = Type::make_prim("usize");
            return e->ty;
        }

        if (auto ln = dynamic_cast<Len*>(e)) {
            TypePtr t = check_expr(ln->seq.get());
            if (ln->dim) {
                TypePtr dt = check_expr(ln->dim.get());
                if (!is_integer(dt)) err("dimension of `len(seq, k)` must be an integer", ln->dim.get());
                auto il = dynamic_cast<IntLit*>(ln->dim.get());
                if (t && t->kind == TypeKind::Array) {
                    if (!il) err("dimension argument of a static array must be an integer literal", ln->dim.get());
                    size_t depth = 0;
                    for (TypePtr c = t; c && c->kind == TypeKind::Array; c = c->elem) depth++;
                    if (il && (il->value < 0 ||
                               static_cast<size_t>(il->value) >= depth)) {
                        err("dimension out of bounds: `" + t->str() + "` has only " + std::to_string(depth) +
                                " dimensions",
                            ln->dim.get());
                    }
                } else if (t && t->kind == TypeKind::Slice) {
                    if (il && (il->value < 0 || il->value >= t->rank)) {
                        err("dimension out of bounds: `" + t->str() + "` has only " + std::to_string(t->rank) +
                                " dimensions",
                            ln->dim.get());
                    }
                } else if (t && t->kind != TypeKind::Array) {
                    err("`len(seq, k)` can only be used on a multidimensional array", ln->dim.get());
                }
            }
            if (t && (t->kind == TypeKind::Array || t->kind == TypeKind::Slice)) {
                return Type::make_prim("usize");
            }
            err("`len` does not support " + (t ? t->str() : "void"), e);
        }

        if (dynamic_cast<RangeExpr*>(e)) err("range `a..b` can only be used in `for-in`", e);

        err("unknown expression", e);
    }

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
        if (expected && expected->kind == TypeKind::Func) {
            for (auto* f : ok) {
                if (same_type(Type::make_func(f->pty, f->rty), expected)) {
                    if (chosen) *chosen = f;
                    return expected;
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
        if (e->synth) return Type::make_func(e->synth->pty, e->synth->rty);
        auto fn = std::make_unique<FuncDecl>();
        fn->name = "$lambda$" + std::to_string(++lambda_seq_);
        fn->line = e->line;
        fn->col = e->col;
        for (auto& p : e->params) {
            TypePtr pt = resolve_type(p->spec.get(), cur_);
            if (is_void(pt)) err("lambda parameter `" + p->name + "` cannot be `void`", p.get());
            if (is_borrow_type(pt)) err("a lambda cannot take a `ref` parameter", p.get());
            fn->pty.push_back(pt);
        }
        fn->pty_ref.assign(fn->pty.size(), false);
        fn->pty_cref.assign(fn->pty.size(), false);

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

        for (size_t i = 0; i < e->params.size(); ++i) {
            Param* p = e->params[i].get();
            auto lv = std::make_unique<LocalVar>();
            lv->name = p->name;
            lv->rty = fn->pty[i];
            lv->line = p->line;
            lv->col = p->col;
            declare_local(lv.get());
            fn->param_locals.push_back(std::move(lv));
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
        TypePtr ft = Type::make_func(fn->pty, retty);
        cur_->lambdas.push_back(std::move(fn));
        e->synth = cur_->lambdas.back().get();
        return ft;
    }

    TypePtr Sema::check_name(Name* e, const TypePtr& expected) {
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
            // A lambda has no captures (3.16): a local of an enclosing scope is off limits.
            if (!lambda_stack_.empty()) {
                const std::size_t base = lambda_stack_.back().scope_base;
                for (std::size_t i = 0; i < base && i < scopes_.size(); ++i) {
                    auto it = scopes_[i].find(e->ident);
                    if (it != scopes_[i].end() && it->second == lv) {
                        err("lambda cannot capture `" + e->ident +
                                "` from the enclosing scope; use a class with a single-method interface for a "
                                "stateful callback",
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
                if (!imp_f) imp_f = f->second.front();
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
        err("undefined identifier `" + e->ident + "`", e);
    }

    TypePtr Sema::check_member(Member* e, const TypePtr& expected) {
        Expr* obj = e->obj.get();
        if (!obj) err("missing object for member access", e);

        if (auto nm = dynamic_cast<Name*>(obj); nm && lookup_local(nm->ident) == nullptr) {
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
                    e->kind = "func";
                    FuncDecl* chosen = nullptr;
                    TypePtr ft = func_value_type(f->second, uname + "." + e->name, expected, &chosen, e);
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
        err("type " + (ot ? ot->str() : "void") + " has no member `" + e->name + "`", e);
    }

    TypePtr Sema::check_unary(Unary* e) {
        if (e->op == "&") {
            if (auto nm = dynamic_cast<Name*>(e->operand.get());
                nm && nm->ident == "this" && lookup_local("this") != nullptr) {
                err("cannot take the address of `this`: it is a reference to the current object", e);
            }
            if (!is_lvalue(e->operand.get())) err("`&` requires an addressable object", e);
            TypePtr t = check_expr(e->operand.get());
            if (roots_at_outer_value_local(e->operand.get())) {
                err("cannot take the address of an enclosing local inside a `para` section", e);
            }
            if (t && t->kind == TypeKind::Func) {
                if (auto n = dynamic_cast<Name*>(e->operand.get()); n && n->kind == "func") {
                    err("`" + n->ident + "` is already a function value: write `" + n->ident + "` without `&`", e);
                }
                err("cannot take the address of a function value: pointer-to-function-pointer is not supported", e);
            }
            if (auto m = dynamic_cast<Member*>(e->operand.get()); m && m->kind == "enum_const") {
                err("enum members are not addressable", e);
            }
            if (auto m = dynamic_cast<Member*>(e->operand.get()); m && m->kind == "const_member") {
                err("const members are not addressable", e);
            }
            if (auto n = dynamic_cast<Name*>(e->operand.get()); n && n->kind == "const_member") {
                err("const members are not addressable", e);
            }
            // U21: the RC lifetime cannot be expressed with a raw pointer — taking the address of class/interface handles,
            // or of members/array elements reached through a reference, is forbidden (`&c`, `&c.x`, `&h.items[0]`, `&catArray[i]`).
            if (is_reference_type(t)) {
                err("cannot take the address of a class or interface reference (`" + t->str() +
                        "`): its RC lifetime cannot be expressed with a raw pointer",
                    e);
            }
            if (rooted_at_reference(e->operand.get())) {
                err("cannot take the address of a member or element reached through a class or interface reference: copy it to a local, "
                    "use it, then assign it back (backlog U21)",
                    e);
            }
            // U24: a struct with reference fields is RC-managed field by field by the compiler; a raw pointer could bypass retain/release
            // (and could also carry a weak reference or field address beyond the lifetime), so taking its address is forbidden in the first stage.
            if (t && t->kind == TypeKind::Struct && has_ref_fields(t) && !t->has_dtor) {
                err("cannot take the address of struct `" + t->str() +
                        "` which has reference fields: a raw pointer could bypass RC (backlog U24); pass or return it by value, or use a class"
                        " (structs with a user-defined destructor are still used through pointers and are not affected)",
                    e);
            }
            return Type::make_ptr(t);
        }

        TypePtr t = check_expr(e->operand.get());

        // An enum is not an arithmetic type: unary operations (`~`/`-`/`+`) require an explicit `as int`
        if (is_enum_t(t) && (e->op == "~" || e->op == "-" || e->op == "+")) {
            err("enum `" + t->str() + "` does not support unary `" + e->op + "` (write `x as int`)", e);
        }

        if (e->op == "*") {
            if (t && t->kind == TypeKind::Ptr) return t->elem;
            if (t && (t->kind == TypeKind::Class || t->kind == TypeKind::Interface)) {
                err("`*`/`->` requires a pointer, found " + t->str() +
                        " (a class handle is already a reference: use `.`)",
                    e);
            }
            err("`*` requires a pointer, found " + (t ? t->str() : "void"), e);
        }
        if (e->op == "~") {
            if (is_integer(t)) return t;
            if (is_user_type(t)) {
                if (const MethodSig* mm = find_member_operator(t, "operator~", {}, e)) {
                    e->op_method = mm;
                    return mm->ret;
                }
                auto cands = collect_operators("~", {t});
                std::vector<FuncDecl*> unary;
                for (auto* f : cands) {
                    if (f->pty.size() == 1) unary.push_back(f);
                }
                if (unary.empty()) {
                    err("type `" + t->str() + "` has no unary `~` operator overload", e);
                }
                auto f = choose_overload(unary, {e->operand.get()}, "operator~", e);
                e->op_func = f;
                return f->rty;
            }
            const std::string hint = (is_prim(t) && t->name == "bool")
                                         ? " (use `not` for logical negation of `bool`)"
                                         : "";
            err("`~` is bitwise NOT and requires an integer" + hint, e);
        }
        if (e->op == "-" || e->op == "+") {
            if (is_numeric(t)) return t;
            if (is_user_type(t)) {
                if (const MethodSig* mm = find_member_operator(t, "operator" + e->op, {}, e)) {
                    e->op_method = mm;
                    return mm->ret;
                }
                auto cands = collect_operators(e->op, {t});
                std::vector<FuncDecl*> unary;
                for (auto* f : cands) {
                    if (f->pty.size() == 1) unary.push_back(f);
                }
                if (unary.empty()) {
                    err("type `" + t->str() + "` has no unary `" + e->op + "` operator overload", e);
                }
                auto f = choose_overload(unary, {e->operand.get()}, "operator" + e->op, e);
                e->op_func = f;
                return f->rty;
            }
            err("`" + e->op + "` requires a numeric operand, found " + (t ? t->str() : "void"), e);
        }
        err("unknown unary operator `" + e->op + "`", e);
    }

    TypePtr Sema::check_ternary(Ternary* e, const TypePtr& expected) {
        TypePtr ct = check_expr(e->cond.get());
        if (!(is_prim(ct) && ct->name == "bool")) {
            err("the condition of `?:` must be `bool`, found " + (ct ? ct->str() : "void"),
                e->cond.get());
        }
        // The expected type is passed through to both arms: literals adapt to the context (`char c = pick ? 'a' : 'b';`)
        TypePtr a = check_expr(e->then_e.get(), expected);
        TypePtr b = check_expr(e->else_e.get(), expected);
        if (!a || !b) err("`?:` is missing a branch expression", e);
        adapt_lit(e->then_e.get(), a, b);
        adapt_lit(e->else_e.get(), b, a);
        a = e->then_e->ty;
        b = e->else_e->ty;

        // A null literal adapts to the other side (pointer / class / interface / dynamic array)
        auto null_target = [](const TypePtr& t) {
            return t && (t->kind == TypeKind::Ptr || t->kind == TypeKind::Class ||
                         t->kind == TypeKind::Interface || t->kind == TypeKind::Slice);
        };
        if (a->kind == TypeKind::Null && b->kind != TypeKind::Null && null_target(b)) {
            e->then_e->ty = b;
            a = b;
        } else if (b->kind == TypeKind::Null && a->kind != TypeKind::Null && null_target(a)) {
            e->else_e->ty = a;
            b = a;
        }

        if (same_type(a, b)) return a;
        if (implicit_ok(a, b)) return b;
        if (implicit_ok(b, a)) return a;
        err("the branches of `?:` have different types (" + a->str() + " and " + b->str() +
                "); cast one of them explicitly with `as`",
            e);
    }

    TypePtr Sema::check_binary(Binary* e) {
        const std::string& op = e->op;

        if (op == "and" || op == "or") {
            check_cond_type(e->left.get(), "`and`/`or`");
            check_cond_type(e->right.get(), "`and`/`or`");
            return t_bool();
        }

        TypePtr lt = check_expr(e->left.get());
        TypePtr rt = check_expr(e->right.get());
        adapt_lit(e->left.get(), lt, rt);
        adapt_lit(e->right.get(), rt, lt);
        lt = e->left->ty;
        rt = e->right->ty;
        if (!lt || !rt) err("binary operator is missing an operand", e);

        if (is_user_type(lt) || is_user_type(rt)) {
            if (op != "==" && op != "!=") {
                if (const MethodSig* mm = find_member_operator(lt, "operator" + op, {e->right.get()}, e)) {
                    e->op_method = mm;
                    return mm->ret;
                }
                auto cands = collect_operators(op, {lt, rt});
                if (cands.empty()) {
                    err("type `" + lt->str() + "` and `" + rt->str() + "` has no `" + op +
                            "` operator overload",
                        e);
                }
                auto f = choose_overload(cands, {e->left.get(), e->right.get()},
                                         "operator" + op, e);
                e->op_func = f;
                return f->rty;
            }
        }

        // An enum is a nominal type: only comparable with the same enum type; there is no implicit conversion to or from integers (`as` is required in both directions)
        if (is_enum_t(lt) || is_enum_t(rt)) {
            if (!(is_enum_t(lt) && is_enum_t(rt)) || !same_type(lt, rt)) {
                const TypePtr& en = is_enum_t(lt) ? lt : rt;
                err("enum `" + en->str() + "` can only be compared or combined with values of the same enum type; there is no implicit" +
                        " conversion between enums and integers (write `as`)",
                    e);
            }
            if (op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" &&
                op != ">=") {
                err("enum `" + lt->str() + "` only supports == != < <= > >= (cast to `int` first for arithmetic or bitwise operations)", e);
            }
            return t_bool();
        }

        if (op == "+" || op == "-") {
            if (lt->kind == TypeKind::Ptr && is_integer(rt)) {
                if (lt->elem && lt->elem->kind == TypeKind::Void)
                    err("pointer arithmetic is not allowed on `void*`", e);
                return lt;
            }
            if (op == "+" && is_integer(lt) && rt->kind == TypeKind::Ptr) {
                if (rt->elem && rt->elem->kind == TypeKind::Void)
                    err("pointer arithmetic is not allowed on `void*`", e);
                return rt;
            }
            if (op == "-" && lt->kind == TypeKind::Ptr && rt->kind == TypeKind::Ptr) {
                if (!same_type(lt->elem, rt->elem)) err("pointer subtraction requires the same element type", e);
                if (lt->elem && lt->elem->kind == TypeKind::Void)
                    err("pointer arithmetic is not allowed on `void*`", e);
                return Type::make_prim("isize");
            }
            TypePtr res = usual_arith(lt, rt);
            if (!res) err("`" + op + "` is not supported for " + lt->str() + " and " + rt->str(), e);
            return res;
        }

        if (op == "*" || op == "/" || op == "%") {
            TypePtr res = usual_arith(lt, rt);
            if (!res) err("`" + op + "` is not supported for " + lt->str() + " and " + rt->str(), e);
            return res;
        }

        if (op == "<<" || op == ">>") {
            if (!is_integer(lt) || !is_integer(rt)) err("`both sides of `" + op + "` must be integers", e);
            return lt;
        }

        if (op == "&" || op == "|" || op == "^") {
            TypePtr res = usual_arith(lt, rt);
            if (!res || !is_integer(res)) err("`both sides of `" + op + "` must be integers", e);
            return res;
        }

        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (lt->kind == TypeKind::Ptr && rt->kind == TypeKind::Ptr) return t_bool();
            if (is_numeric(lt) && is_numeric(rt)) return t_bool();
            err("`" + op + "` is not supported for " + lt->str() + " and " + rt->str(), e);
        }

        if (op == "==" || op == "!=") {
            if (lt->kind == TypeKind::Null || rt->kind == TypeKind::Null) {
                const TypePtr& other = (lt->kind == TypeKind::Null) ? rt : lt;
                if (other->kind == TypeKind::Slice) {
                    err("slice cannot be compared with `null` (use `s.length() == 0`)", e);
                }
                if (other->kind == TypeKind::Struct) {
                    err("a struct value (`" + other->str() +
                            "`) cannot be compared with `null`: structs are values, not handles (use a `class` "
                            "for a nullable reference)",
                        e);
                }
                if (other->kind == TypeKind::Ptr || other->kind == TypeKind::Slice ||
                    other->kind == TypeKind::Null || other->kind == TypeKind::Class ||
                    other->kind == TypeKind::Interface || other->kind == TypeKind::Func) {
                    return t_bool();
                }
            }
            if (is_numeric(lt) && is_numeric(rt)) return t_bool();
            if (lt->kind == TypeKind::Func || rt->kind == TypeKind::Func) {
                if (!same_type(lt, rt)) {
                    err("function values of different types cannot be compared (`" + lt->str() + "` vs `" +
                            rt->str() + "`)",
                        e);
                }
                return t_bool();
            }
            const bool eq = same_type(lt, rt);
            if ((eq && (lt->kind == TypeKind::Ptr || lt->kind == TypeKind::Enum ||
                        lt->kind == TypeKind::Class || lt->kind == TypeKind::Interface)) ||
                (is_prim(lt) && lt->name == "bool" && same_type(rt, lt))) {
                return t_bool();
            }
            // Between reference types: identity comparison is allowed when they are convertible to each other (base class/interface)
            if (is_reference_type(lt) && is_reference_type(rt) &&
                (implicitly_convertible(lt, rt) || implicitly_convertible(rt, lt))) {
                return t_bool();
            }
            if (is_user_type(lt) || is_user_type(rt)) {
                if (const MethodSig* mm = find_member_operator(lt, "operator" + op, {e->right.get()}, e)) {
                    e->op_method = mm;
                    return mm->ret;
                }
                if (op == "!=") {
                    // `!=` may be provided by a member `operator==` (negate the result)
                    if (const MethodSig* me = find_member_operator(lt, "operator==", {e->right.get()}, e)) {
                        e->op_method = me;
                        e->negate_result = true;
                        return t_bool();
                    }
                }
                auto cands = collect_operators(op, {lt, rt});
                if (cands.empty() && op == "!=") {
                    auto eqs = collect_operators("==", {lt, rt});
                    if (!eqs.empty()) {
                        e->op_func = choose_overload(eqs, {e->left.get(), e->right.get()}, "operator==", e);
                        e->negate_result = true;
                        return t_bool();
                    }
                }
                if (cands.empty()) {
                    err("type `" + lt->str() + "` and `" + rt->str() + "` has no `" + op +
                            "` operator overload",
                        e);
                }
                auto f = choose_overload(cands, {e->left.get(), e->right.get()},
                                         "operator" + op, e);
                e->op_func = f;
                return f->rty;
            }
            err("`" + op + "` is not supported for " + lt->str() + " and " + rt->str(), e);
        }

        err("unknown binary operator `" + op + "`", e);
    }

    TypePtr Sema::check_cond_type(Expr* e, const std::string& what) {
        TypePtr t = check_expr(e);
        if (!(is_prim(t) && t->name == "bool")) {
            err(what + " must be `bool`, found " + (t ? t->str() : "void"), e);
        }
        return t;
    }

    void Sema::adapt_lit(Expr* e, const TypePtr& self_ty, const TypePtr& other) {
        (void)self_ty;
        if (auto cl = dynamic_cast<CharLit*>(e)) {
            // A character literal is a byte: it adapts to the 8-bit integer type on the other side (char/byte)
            if (is_prim(other) && other->is_integer() && other->bits() == 8) {
                cl->ty = other;
            }
            return;
        }
        auto il = dynamic_cast<IntLit*>(e);
        if (!il) return;
        if (is_prim(other) && other->is_numeric() && promote_int_literal(il->value, other)) {
            il->ty = other;
        }
    }

    bool Sema::can_cast(const TypePtr& src, const TypePtr& dst) {
        if (!src || !dst) return false;
        if (same_type(src, dst)) return true;
        // An enum is a nominal type: explicit conversion only with integers (non-enum), and never with floating point
        if (src->kind == TypeKind::Enum || dst->kind == TypeKind::Enum) {
            if (src->kind == TypeKind::Enum && dst->kind == TypeKind::Enum) return false;
            return src->kind == TypeKind::Enum ? is_plain_int(dst) : is_plain_int(src);
        }
        if (is_numeric(src) && is_numeric(dst)) return true;
        // Reference types: upcast/downcast (downcasts are checked at run time and yield null on failure)
        if (is_class_type(src) && is_class_type(dst)) {
            return is_subclass_of(src, dst) || is_subclass_of(dst, src);
        }
        if (is_interface_type(src) && is_interface_type(dst)) {
            return implements_interface(src, dst);  // upcast only; downcast is deferred
        }
        if (is_interface_type(src) && is_class_type(dst)) return true;
        if (src->kind == TypeKind::Ptr && dst->kind == TypeKind::Ptr) return true;
        if (src->kind == TypeKind::Slice && dst->kind == TypeKind::Slice) {
            return src->rank == dst->rank && same_type(src->elem, dst->elem);
        }
        if (src->kind == TypeKind::Ptr && is_integer(dst) && dst->bits() >= 64) return true;
        if (dst->kind == TypeKind::Ptr && is_integer(src) && src->bits() >= 64) return true;
        // 3.16: a function value is a pointer; `void*` → function type requires `as`, and a function value → `void*` too
        if (dst->kind == TypeKind::Func && src->kind == TypeKind::Ptr) return true;
        if (src->kind == TypeKind::Func && dst->kind == TypeKind::Ptr) return true;
        return false;
    }
}  // namespace muc_detail
