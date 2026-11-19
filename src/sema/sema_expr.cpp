// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Expression typing: the dispatch and the operators (definitions of Sema members).
//
// owns:      `check_expr`/`check_expr_impl`, unary/binary/ternary and the ordering family, conditional
//            types, integer-literal adaptation, `can_cast`, `move`/`swap`, the global-init order check
// not:       names and member access (sema_member.cpp), lambdas (sema_lambda.cpp), call resolution
//            (sema_call.cpp), statement bodies (sema_stmt.cpp)
// pass:      body checking, after declarations are placed
// concept:   expressions, casts
#include "sema_impl.hpp"

#include "ast_clone.hpp"

namespace muc_detail {

    TypePtr Sema::check_expr(Expr* e, const TypePtr& expected) {
        if (!e) err("missing expression");
        TypePtr t = check_expr_impl(e, expected);
        e->ty = t;
        return t;
    }

    TypePtr Sema::check_expr_impl(Expr* e, const TypePtr& expected) {
        if (auto m = dynamic_cast<MatchExpr*>(e)) return check_match(m);
        if (auto il = dynamic_cast<IntLit*>(e)) {
            // A `2^63` magnitude is only meaningful directly after unary `-`; the parser consumes it
            // there, so one reaching this point is out of range.
            if (il->min_magnitude) {
                err("integer literal is too large (maximum 2^63-1): a 2^63 magnitude is only valid "
                    "directly after unary `-`",
                    e);
            }
            // A suffixed literal has a fixed type (C rule) and never adapts to the expected type.
            if (il->isuffix == 1) return Type::make_prim("uint");
            if (il->isuffix == 2) return Type::make_prim("long");
            if (il->isuffix == 3) return Type::make_prim("ulong");
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
                // The free-function form of `operator[]` is a parse error since build 161 (it copied the
                // receiver on every element access), so only a member declaration can match here.
                err("type `" + bt->str() + "` does not support subscripting with " + std::to_string(n) +
                        " index" + (n == 1 ? "" : "es") + ": declare `operator[](…)` inside `" + bt->str() +
                        "` (a subscript is always a member operator)",
                    e);
            }
            err("type " + bt->str() + " cannot be subscripted", e);
        }

        if (auto u = dynamic_cast<Unary*>(e)) return check_unary(u, expected);

        if (auto nt = dynamic_cast<NotExpr*>(e)) {
            TypePtr t = check_expr(nt->operand.get());
            if (!(is_prim(t) && t->name == "bool")) {
                err("`not` requires a `bool` operand, found " + (t ? t->str() : "void"), e);
            }
            return t_bool();
        }

        if (auto mv = dynamic_cast<MoveExpr*>(e)) return check_move(mv);

        if (auto sw = dynamic_cast<SwapExpr*>(e)) return check_swap(sw);

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

        if (auto ie = dynamic_cast<IsExpr*>(e)) return check_is(ie);

        if (auto ne = dynamic_cast<NewExpr*>(e)) return check_new(ne);

        if (auto c = dynamic_cast<Call*>(e)) return check_call(c, expected);

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
            e->ty = Type::make_prim("isize");
            return e->ty;
        }

        if (auto ln = dynamic_cast<Len*>(e)) {
            TypePtr t = check_expr(ln->seq.get());
            if (ln->dim) {
                TypePtr dt = check_expr(ln->dim.get());
                if (!is_integer(dt)) err("dimension of `a.length(k)` must be an integer", ln->dim.get());
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
                    err("`a.length(k)` can only be used on a multidimensional array", ln->dim.get());
                }
            }
            if (t && (t->kind == TypeKind::Array || t->kind == TypeKind::Slice)) {
                return Type::make_prim("isize");
            }
            err("`len` does not support " + (t ? t->str() : "void"), e);
        }

        if (dynamic_cast<RangeExpr*>(e)) err("range `a..b` can only be used in `for-in`", e);

        err("unknown expression", e);
    }



    TypePtr Sema::check_unary(Unary* e, const TypePtr& expected) {
        // The decimal spelling of a signed minimum goes through its magnitude, which is one past the
        // positive range: `-2147483648` is 2147483648 negated, and that magnitude is not an `int` on
        // its own. A negated integer literal whose magnitude is exactly 2^(bits-1) of the target type
        // therefore is that value (C, C++ and Java all make this exception).
        if (e->op == "-" && is_prim(expected) && expected->is_integer() && expected->is_signed() &&
            expected->bits() < 64) {
            if (auto il = dynamic_cast<IntLit*>(e->operand.get());
                il && il->value == (1LL << (expected->bits() - 1))) {
                il->ty = expected;
                return expected;
            }
        }
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
                auto f = choose_overload(unary, {e->operand.get()}, "operator~", e, /*check_ref=*/true);
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
                auto f = choose_overload(unary, {e->operand.get()}, "operator" + e->op, e, /*check_ref=*/true);
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
            // A binding `is` must be the whole condition (SPEC 10.1.1): the binding does not reach the
            // other operand, and "undefined identifier v" would send the reader looking for a scoping
            // bug. Name the rule and the nested rewrite instead.
            auto bind_is = [](Expr* x) -> IsExpr* {
                auto* is = dynamic_cast<IsExpr*>(x);
                return (is && !is->binds.empty()) ? is : nullptr;
            };
            if (IsExpr* bad = bind_is(e->left.get())) {
                err("a binding `is` must be the whole condition; write `if o is Some(v) { if v > 0 { ... } }`",
                    bad);
            }
            if (IsExpr* bad = bind_is(e->right.get())) {
                err("a binding `is` must be the whole condition; write `if o is Some(v) { if v > 0 { ... } }`",
                    bad);
            }
            check_cond_type(e->left.get(), "`and`/`or`");
            check_cond_type(e->right.get(), "`and`/`or`");
            return t_bool();
        }

        // A bare alternative has no type of its own, so on either side of `==` / `!=` the other
        // operand supplies the target type (SPEC 10.1.1 contextual construction): `v == None`,
        // `v == One(7)`. The side that is not a bare alternative is checked first when the left
        // operand needs the target.
        auto bare_alt_call = [&](Expr* x) -> Call* {
            auto* call = dynamic_cast<Call*>(x);
            if (!call || !call->callee_kind.empty() || !call->type_args.empty()) return nullptr;
            auto* nm = dynamic_cast<Name*>(call->callee.get());
            if (!nm || lookup_local(nm->ident) != nullptr) return nullptr;
            // Only a name that is not a visible function is a candidate bare alternative here: the
            // ordering of the two checks below must not be decided in favour of a real call.
            return collect_funcs_named(nm->ident).empty() ? call : nullptr;
        };
        const bool cmp = op == "==" || op == "!=";
        TypePtr lt;
        if (cmp && bare_alt_call(e->left.get()) && !bare_alt_call(e->right.get())) {
            TypePtr prt = check_expr(e->right.get());
            if (prt && prt->kind == TypeKind::Sum) {
                check_assign(e->left.get(), prt, "comparison operand");
                lt = check_expr(e->left.get(), prt);
            } else {
                lt = check_expr(e->left.get());
            }
        } else {
            lt = check_expr(e->left.get());
        }
        // `v == None`: a bare alternative name on the right of a comparison with a sum value is
        // resolved from the left side's type (contextual construction, SPEC 10.1.1).
        bool right_typed = false;
        if (lt && lt->kind == TypeKind::Sum) {
            if (auto nm = dynamic_cast<Name*>(e->right.get());
                nm && nm->kind.empty() && lookup_local(nm->ident) == nullptr) {
                for (size_t k = 0; k < lt->sum_names.size(); ++k) {
                    if (lt->sum_names[k] == nm->ident && lt->sum_fields[k].empty()) {
                        nm->kind = "sum_unit";
                        nm->sum_ty = lt;
                        nm->sum_alt = static_cast<int>(k);
                        nm->ty = lt;
                        break;
                    }
                }
            } else if (cmp && bare_alt_call(e->right.get())) {
                check_assign(e->right.get(), lt, "comparison operand");
                right_typed = true;
            }
        }
        // An operand constructed above keeps the type it was built from: re-checking it without the
        // target would look the bare name up as a function and report "undefined function".
        TypePtr rt = check_expr(e->right.get(), right_typed ? lt : TypePtr());
        adapt_lit(e->left.get(), lt, rt);
        adapt_lit(e->right.get(), rt, lt);
        lt = e->left->ty;
        rt = e->right->ty;
        if (!lt || !rt) err("binary operator is missing an operand", e);

        if (is_user_type(lt) || is_user_type(rt)) {
            if (op != "==" && op != "!=") {
                if (const MethodSig* mm = find_member_operator(lt, "operator" + op, {e->right.get()}, e)) {
                    e->op_method = mm;
                    e->by_ref = is_borrow_type(mm->ret);
                    e->ref_const = e->by_ref && mm->ret->is_const;
                    return referent_of(mm->ret);
                }
                auto cands = collect_operators(op, {lt, rt});
                // Ordering family derived from `operator<`, the same way `!=` is derived from `==`:
                // `a > b` is `b < a`, `a <= b` is `!(b < a)`, `a >= b` is `!(a < b)`. A declared
                // operator always wins (`cands` non-empty), so a type that needs the native
                // semantics — floats with NaN, an asymmetric receiver — keeps declaring it.
                // PERF/NOTE: the receiver of a derived comparison is the other operand, so the two
                // operands are evaluated right-to-left (documented in SPEC 3.11).
                if (cands.empty() && (op == ">" || op == "<=" || op == ">=")) {
                    const bool backwards = op == ">" || op == "<=";  // receiver is the right operand
                    Expr* recv = backwards ? e->right.get() : e->left.get();
                    Expr* arg = backwards ? e->left.get() : e->right.get();
                    TypePtr recvTy = backwards ? rt : lt;
                    TypePtr argTy = backwards ? lt : rt;
                    const bool negate = op != ">";
                    if (const MethodSig* mm = find_member_operator(recvTy, "operator<", {arg}, e)) {
                        e->op_method = mm;
                        e->swap_operands = backwards;
                        e->negate_result = negate;
                        return mm->ret;
                    }
                    auto lesses = collect_operators("<", {recvTy, argTy});
                    if (!lesses.empty()) {
                        e->op_func = choose_overload(lesses, {recv, arg}, "operator<", e, /*check_ref=*/true);
                        e->swap_operands = backwards;
                        e->negate_result = negate;
                        return e->op_func->rty;
                    }
                }
                if (cands.empty()) {
                    std::string hint;
                    if (op == ">" || op == "<=" || op == ">=")
                        hint = " (there is no `operator<` to derive it from either)";
                    err("type `" + lt->str() + "` and `" + rt->str() + "` has no `" + op +
                            "` operator overload" + hint,
                        e);
                }
                auto f = choose_overload(cands, {e->left.get(), e->right.get()},
                                         "operator" + op, e, /*check_ref=*/true);
                e->op_func = f;
                e->by_ref = is_borrow_type(f->rty);
                e->ref_const = e->by_ref && f->rty->is_const;
                return referent_of(f->rty);
            }
        }

        // An enum is a nominal type: only comparable with the same enum type; there is no implicit conversion to or from integers (`as` is required in both directions)
        if (is_enum_t(lt) || is_enum_t(rt)) {
            if (!(is_enum_t(lt) && is_enum_t(rt)) || !same_type_sig(lt, rt)) {
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
                    other->kind == TypeKind::Interface || other->kind == TypeKind::Func ||
                    other->kind == TypeKind::Delegate) {
                    return t_bool();
                }
            }
            if (is_numeric(lt) && is_numeric(rt)) return t_bool();
            if (lt->kind == TypeKind::Sum || rt->kind == TypeKind::Sum) {
                // Value equality: same alternative and equal payload fields. Only payload types with
                // value equality take part; reference payloads would need an identity rule (use `switch`).
                if (!same_type(lt, rt)) {
                    err("sum values of different types cannot be compared (`" + lt->str() + "` vs `" +
                            rt->str() + "`)",
                        e);
                }
                for (auto& fs : lt->sum_fields) {
                    for (auto& f : fs) {
                        const bool ok = f && (f->kind == TypeKind::Prim || f->kind == TypeKind::Enum ||
                                              f->kind == TypeKind::Ptr || f->kind == TypeKind::Func);
                        if (!ok) {
                            err("sum `" + lt->str() + "` cannot be compared: payload type `" +
                                    (f ? f->str() : std::string("void")) +
                                    "` has no value equality (read it with `switch`)",
                                e);
                        }
                    }
                }
                return t_bool();
            }
            if (lt->kind == TypeKind::Func || rt->kind == TypeKind::Func ||
                lt->kind == TypeKind::Delegate || rt->kind == TypeKind::Delegate) {
                if (!same_type(lt, rt)) {
                    err("function values of different types cannot be compared (`" + lt->str() + "` vs `" +
                            rt->str() + "`)",
                        e);
                }
                return t_bool();
            }
            const bool eq = same_type_sig(lt, rt);
            if ((eq && (lt->kind == TypeKind::Ptr || lt->kind == TypeKind::Enum ||
                        lt->kind == TypeKind::Class || lt->kind == TypeKind::Interface)) ||
                (is_prim(lt) && lt->name == "bool" && same_type_sig(rt, lt))) {
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
                        e->op_func = choose_overload(eqs, {e->left.get(), e->right.get()}, "operator==", e, /*check_ref=*/true);
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
                                         "operator" + op, e, /*check_ref=*/true);
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
        // `as` is the explicit (unsafe) escape hatch, including `const` removal (SPEC 3.7: there is no
        // `const_cast`; write `as`). Adding cv to the same type is equally explicit.
        if (same_type_sig(src, dst)) return true;
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

    // `move x`: transfers ownership out of a local variable or parameter
    // (agents/value-semantics.md §8.3-§8.4). Only a whole name can be moved: a partial move out of a field
    // or an element would need field-level state, which is what an ownership checker would have to do.
    TypePtr Sema::check_move(MoveExpr* mv) {
        // The expression may be visited more than once (argument coercion re-checks its arguments), so
        // the second visit must not report the value it moved itself as "already moved".
        if (mv->moved) return mv->ty;
        auto* nm = dynamic_cast<Name*>(mv->operand.get());
        if (!nm) {
            // Partial moves (`move o.field`, `move a[i]`) are not supported; a temporary is already an
            // rvalue, so `move` on it is redundant rather than partial — name the actual case.
            if (dynamic_cast<Member*>(mv->operand.get()) || dynamic_cast<Index*>(mv->operand.get())) {
                err("`move` needs a variable name: partial moves (`move o.field`, `move a[i]`) are not supported",
                    mv);
            }
            err("`move` needs a variable name: this operand is already a temporary value, so pass it "
                "directly (`f(Buf(2))`)",
                mv);
        }
        LocalVar* lv = lookup_local(nm->ident);
        if (!lv) {
            err("`move` can only transfer a local variable or a parameter; `" + nm->ident + "` is not one", mv);
        }
        if (lv->is_const) err("cannot move `" + nm->ident + "`: it is a `const` value; copy it instead", mv);
        if (lv->by_ref) {
            err("cannot move `" + nm->ident + "`: it is a `ref` borrow, and borrowing never transfers ownership",
                mv);
        }
        TypePtr t = check_expr(nm);  // reports "was moved and cannot be used" when it already happened
        if (!t) return t;
        if (!t->movable()) {
            const bool declared = t->kind == TypeKind::Struct && t->nomove;
            err("cannot move `" + nm->ident + "`: `" + t->str() + "` is " +
                    (declared ? "declared `nomove` (its address is part of its value)"
                              : "not movable (a field is `nomove`)"),
                mv);
        }
        auto lvl = local_loop_level_.find(lv);
        if (move_loop_count_ > 0 && lvl != local_loop_level_.end() && lvl->second < move_loop_count_) {
            err("cannot move `" + nm->ident +
                    "`: it was declared outside the enclosing loop, so the next iteration would reuse a "
                    "moved value",
                mv);
        }
        moved_locals_.insert(lv);
        mv->moved = lv;
        mv->ty = t;
        return t;
    }

    void Sema::note_assigned_local(const std::string& name) {
        if (LocalVar* lv = lookup_local(name)) moved_locals_.erase(lv);
    }

    // `swap(a, b)`: exchange the values at two writable places (agents/value-semantics.md §14).
    // Both sides stay initialized, so this never interacts with the moved tracking.
    //
    // A weak field is the exception: the registry is keyed by (target, field address), and a raw byte
    // exchange would leave both registrations pointing at the wrong target, so the operation is
    // rejected instead of lowering to bytes (#29). A struct that contains a weak field inherits the
    // restriction, because swapping it would move the registration the same way.
    static bool type_embeds_weak(const TypePtr& t, int depth = 0) {
        if (!t || depth > 32) return false;
        if (t->kind == TypeKind::Struct) {
            for (const auto& f : t->fields) {
                if (f.weak) return true;
                if (f.type && type_embeds_weak(f.type, depth + 1)) return true;
            }
        }
        if (t->kind == TypeKind::Array) return type_embeds_weak(t->elem, depth + 1);
        return false;
    }

    TypePtr Sema::check_swap(SwapExpr* sw) {
        TypePtr ta = check_expr(sw->a.get());
        TypePtr tb = check_expr(sw->b.get());
        if (!ta || !tb) return t_void();
        if (is_void(ta) || is_void(tb)) err("`swap` cannot exchange `void` values", sw);
        // A const place is reported before the type mismatch (the message names `const`, not the type).
        check_writable(sw->a.get());
        check_writable(sw->b.get());
        if (!same_type_sig(ta, tb)) {
            err("`swap` needs two places of the same type (found `" + ta->str() + "` and `" + tb->str() +
                    "`)",
                sw);
        }
        if (!is_lvalue(sw->a.get()) || !is_lvalue(sw->b.get())) {
            err("`swap` needs two writable places (a variable, a field, an element or a dereference)", sw);
        }
        if (is_weak_field_access(sw->a.get()) || is_weak_field_access(sw->b.get()) || type_embeds_weak(ta)) {
            err("`swap` cannot exchange `weak` fields or a value that contains one: the weak-reference "
                "registry tracks each field address, so a raw exchange would leave the registrations "
                "pointing at the wrong target; assign through the field instead",
                sw);
        }
        check_writable(sw->a.get());
        check_writable(sw->b.get());
        return t_void();
    }
}  // namespace muc_detail
