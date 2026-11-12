// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds assignment compatibility and compile-time constant evaluation (definitions of Sema members).
#include "sema_impl.hpp"

namespace muc_detail {

    bool Sema::contains_reference_type(const TypePtr& t) {
        if (!t) return false;
        if (is_reference_type(t)) return true;
        if (t->kind == TypeKind::Array || t->kind == TypeKind::Slice) {
            return contains_reference_type(t->elem);
        }
        if (t->kind == TypeKind::Struct) {
            for (const auto& f : t->fields) {
                if (f.weak) continue;
                if (contains_reference_type(f.type)) return true;
            }
        }
        return false;
    }

    bool Sema::contains_slice_type(const TypePtr& t) {
        if (!t) return false;
        if (t->kind == TypeKind::Slice) return true;
        if (t->kind == TypeKind::Array) return contains_slice_type(t->elem);
        if (t->kind == TypeKind::Struct) {
            for (const auto& f : t->fields) {
                if (f.weak) continue;
                if (contains_slice_type(f.type)) return true;
            }
        }
        return false;
    }

    bool Sema::is_zeroable(const TypePtr& t) {
        if (!t) return false;
        return t->kind == TypeKind::Struct || t->kind == TypeKind::Array ||
               t->kind == TypeKind::Slice;
    }

    bool Sema::is_char(const TypePtr& t) {
        if (!is_prim(t)) return false;
        return t->name == "char" || t->name == "byte";
    }

    bool Sema::noncopyable_struct(const TypePtr& t) {
        return t && t->kind == TypeKind::Struct && t->has_dtor;
    }

    TypePtr Sema::check_assign(Expr* e, const TypePtr& dst, const std::string& what) {
        if (noncopyable_struct(dst)) {
            err(what + ": struct `" + dst->str() +
                    "` has a destructor and cannot be copied by value (use a pointer or an explicit clone)",
                e);
        }
        TypePtr src = check_expr(e, dst);
        if (assignable(e, src, dst)) return src;
        if (is_enum_t(src) || is_enum_t(dst)) {
            err(what + ": cannot assign " + (src ? src->str() : "void") + " to " +
                    (dst ? dst->str() : "void") + " (no implicit conversion between enums and integers; write `as`)",
                e);
        }
        err(what + ": cannot assign " + (src ? src->str() : "void") + " to " +
                (dst ? dst->str() : "void"),
            e);
    }

    bool Sema::assignable(Expr* e, const TypePtr& src, const TypePtr& dst) {
        // C model: const may only be relaxed, never tightened (same_type ignores const, so this must be caught first)
        if (src && dst && src->kind == dst->kind &&
            (src->kind == TypeKind::Ptr || src->kind == TypeKind::Slice) &&
            src->elem && dst->elem && src->elem->is_const && !dst->elem->is_const) {
            return false;
        }
        if (same_type(src, dst)) return true;
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (promote_int_literal(il->value, dst)) {
                il->ty = dst;
                return true;
            }
            if (il->value == 0 && dst &&
                (dst->kind == TypeKind::Class || dst->kind == TypeKind::Interface)) {
                il->ty = dst;  // `Cat c = 0` is equivalent to null
                return true;
            }
        }
        if (dynamic_cast<CharLit*>(e) && dst && is_prim(dst) && dst->is_integer() &&
            dst->bits() == 8) {
            e->ty = dst;  // character literal adapts to char / byte
            return true;
        }
        if (dynamic_cast<NullLit*>(e) && dst) {
            if (dst->kind == TypeKind::Ptr || dst->kind == TypeKind::Slice ||
                dst->kind == TypeKind::Class || dst->kind == TypeKind::Interface) {
                e->ty = dst;
                return true;
            }
        }
        if (src && dst && src->kind == TypeKind::Array && dst->kind == TypeKind::Slice) {
            // T[N] -> T[]; T[m,n] -> T[,] (match rank by nesting depth, element types must agree)
            TypePtr base = src;
            size_t depth = 0;
            while (base && base->kind == TypeKind::Array) {
                base = base->elem;
                depth++;
            }
            if (static_cast<int>(depth) == dst->rank && same_type(base, dst->elem)) {
                if (type_align(base) > 8) err_slice_elem_align(base, e);
                return true;
            }
            return false;
        }
        if (is_str(src) && dst && dst->kind == TypeKind::Ptr && dst->elem &&
            dst->elem->is_const &&
            dst->elem->kind == TypeKind::Prim && dst->elem->bits() == 8) {
            return true;  // Str -> readonly char*/byte* (requires NUL termination)
        }
        return implicit_ok(src, dst);
    }

    std::string Sema::type_str_no_const(const TypePtr& t) {
        if (!t) return "void";
        if (!t->is_const) return t->str();
        Type copy = *t;
        copy.is_const = false;
        return copy.str();
    }

    long long Sema::normalize_int(long long v, const TypePtr& t) {
        if (!t || t->kind != TypeKind::Prim) return v;
        const int bits = t->bits();
        if (bits <= 0 || bits >= 64) return v;
        const unsigned long long mask = (1ULL << bits) - 1;
        unsigned long long u = static_cast<unsigned long long>(v) & mask;
        if (t->is_signed() && (u >> (bits - 1)) != 0) u |= ~mask;
        return static_cast<long long>(u);
    }

    long long Sema::sext_bits(long long v, int bits) {
        if (bits <= 0 || bits >= 64) return v;
        const unsigned long long mask = (1ULL << bits) - 1;
        unsigned long long u = static_cast<unsigned long long>(v) & mask;
        if ((u >> (bits - 1)) & 1ULL) u |= ~mask;
        return static_cast<long long>(u);
    }

    Sema::ConstVal Sema::cast_const(const Sema::ConstVal& v, const TypePtr& dst) {
        ConstVal r;
        r.ty = dst;
        if (dst->kind == TypeKind::Enum) {
            r.i = v.i;
            return r;
        }
        if (v.ty && v.ty->kind == TypeKind::Enum) {
            ConstVal tmp = v;
            tmp.ty = t_int();
            return cast_const(tmp, dst);
        }
        if (dst->is_float()) {
            double d;
            if (v.is_float) {
                d = v.f;
            } else if (v.ty && v.ty->kind == TypeKind::Prim && !v.ty->is_signed()) {
                // A 64-bit unsigned value sits in v.i as a two's-complement bit pattern; converting to float must interpret it as unsigned
                d = static_cast<double>(static_cast<unsigned long long>(v.i));
            } else {
                d = static_cast<double>(v.i);
            }
            if (dst->name == "float") d = static_cast<double>(static_cast<float>(d));
            r.is_float = true;
            r.f = d;
            return r;
        }
        long long i = v.is_float ? static_cast<long long>(v.f) : v.i;
        r.i = normalize_int(i, dst);
        return r;
    }

    void Sema::convert_const_to(Sema::ConstVal& v, const TypePtr& dst, const Node* at,
                          const std::string& what) {
        if (!dst) return;
        if (same_type(v.ty, dst)) {
            v.ty = dst;
            return;
        }
        // Integer literal adapts to a floating-point target (`const double D = 2;`) and floating-point literal adapts
        // to float / double (`const float F = 1.5;`), consistent with the variable initialization rules
        if ((v.lit || v.flit) && dst->is_float()) {
            v = cast_const(v, dst);
            v.ty = dst;
            v.lit = false;
            v.flit = false;
            return;
        }
        // A character literal adapts bit-pattern-wise to an 8-bit integer target (`const char C = '\u{e9}';`),
        // consistent with run-time check_expr_impl: no signed range check, only bit-pattern truncation
        if (v.clit && is_prim(dst) && dst->is_integer() && dst->bits() == 8) {
            v.i = normalize_int(v.i, dst);
            v.ty = dst;
            v.clit = false;
            return;
        }
        if (v.lit && !v.is_float && promote_int_literal(v.i, dst)) {
            v.i = normalize_int(v.i, dst);
            v.ty = dst;
            v.lit = false;
            return;
        }
        if (v.ty && (v.ty->kind == TypeKind::Enum || dst->kind == TypeKind::Enum)) {
            err(what + ": cannot assign " + type_str_no_const(v.ty) + " to " +
                    type_str_no_const(dst) +
                    " (no implicit conversion between enums and integers; write `as`)",
                at);
        }
        if (implicit_ok(v.ty, dst)) {
            v = cast_const(v, dst);
            v.ty = dst;
            return;
        }
        err(what + ": cannot assign " + type_str_no_const(v.ty) + " to " + type_str_no_const(dst),
            at);
    }

    bool Sema::eval_const_member(ConstMember* cm, Sema::ConstVal& out) {
        if (!cm) return false;
        if (cm->evaluated) {
            out.ty = cm->rty;
            out.is_float = cm->is_float;
            out.i = cm->ival;
            out.f = cm->fval;
            return true;
        }
        if (cm->evaluating) {
            err("circular constant definition involving `" + cm->name + "`", cm);
        }
        if (!cm->init) return false;
        Unit* owner_unit = nullptr;
        if (cm->owner) {
            auto it = infos_.find(cm->owner->unit);
            if (it != infos_.end()) owner_unit = it->second.unit;
        }
        CurUnitGuard guard(*this, owner_unit);
        EvalFlagGuard flag(cm->evaluating);
        ConstVal v;
        const bool ok = eval_const(cm->init.get(), cm->owner, v);
        if (!ok) return false;
        convert_const_to(v, cm->rty, cm, "initializer of const member `" + cm->name + "`");
        cm->evaluated = true;
        cm->is_float = v.is_float;
        cm->ival = v.i;
        cm->fval = v.f;
        out = v;
        return true;
    }

    bool Sema::eval_global_const(GlobalVar* g, const std::string& unit_name, Sema::ConstVal& out) {
        if (!g) return false;
        if (g->const_evaluated) {
            out.ty = g->rty;
            out.is_float = g->const_is_float;
            out.i = g->const_ival;
            out.f = g->const_fval;
            return true;
        }
        if (g->const_evaluating) {
            err("circular constant definition involving `" + g->name + "`", g);
        }
        if (!g->init || !g->rty) return false;
        auto* ie = dynamic_cast<InitExpr*>(g->init.get());
        if (!ie) return false;
        Unit* u = nullptr;
        auto it = infos_.find(unit_name);
        if (it != infos_.end()) u = it->second.unit;
        CurUnitGuard guard(*this, u);
        EvalFlagGuard flag(g->const_evaluating);
        ConstVal v;
        const bool ok = eval_const(ie->expr.get(), nullptr, v);
        if (!ok) return false;
        convert_const_to(v, g->rty, g, "initializer of `" + g->name + "`");
        g->const_evaluated = true;
        g->const_is_float = v.is_float;
        g->const_ival = v.i;
        g->const_fval = v.f;
        out = v;
        return true;
    }

    bool Sema::eval_const_name(Name* e, const TypePtr& self_ty, Sema::ConstVal& out) {
        // 1. Type-level constants of the current type (bare name, including the base class chain)
        if (self_ty) {
            if (ConstMember* cm = find_const_member_in(self_ty, e->ident)) {
                if (!const_member_visible(cm)) {
                    err("const member `" + cm->name + "` is not declared public (type `" +
                            cm->owner->str() + "`)",
                        e);
                }
                return eval_const_member(cm, out);
            }
        }
        // 2. Variables of this unit: only unit-level const can take part in evaluation
        UnitInfo& ui = info();
        if (auto g = ui.globals.find(e->ident); g != ui.globals.end()) {
            if (!g->second->is_const) return false;
            return eval_global_const(g->second, cur_->name, out);
        }
        // 3. Enum members of this unit (bare name)
        if (EnumConst* ec = lookup_enum_const(e->ident)) {
            out.ty = ec->ty;
            out.i = ec->value;
            return true;
        }
        // 4. Directly `use`d units (public only; several same-name const candidates are an ambiguity error, consistent with check_name)
        GlobalVar* imp_g = nullptr;
        std::string imp_g_unit;
        EnumConst* imp_ec = nullptr;
        std::string imp_ec_unit;
        for (auto& use : cur_->uses) {
            if (!unit_visible(use->unit_name)) continue;
            auto it = infos_.find(use->unit_name);
            if (it == infos_.end()) continue;
            UnitInfo& o = it->second;
            if (auto g = o.globals.find(e->ident); g != o.globals.end() && g->second->is_public) {
                // A non-const candidate cannot take part in constant evaluation: keep looking for const candidates in other units
                if (!g->second->is_const) continue;
                if (imp_g) {
                    err("`" + e->ident + "` is ambiguous: `" + imp_g_unit + "." + e->ident + "` and `" +
                            use->unit_name + "." + e->ident + "`; use a qualified name",
                        e);
                }
                imp_g = g->second;
                imp_g_unit = use->unit_name;
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
        if (imp_g) return eval_global_const(imp_g, imp_g_unit, out);
        if (imp_ec) {
            out.ty = imp_ec->ty;
            out.i = imp_ec->value;
            return true;
        }
        return false;
    }

    TypePtr Sema::const_expr_base_type(Expr* base) {
        if (auto nm = dynamic_cast<Name*>(base)) {
            if (lookup_local(nm->ident)) return nullptr;
            if (info().globals.count(nm->ident)) return nullptr;
            return find_type_in_expr(nm->ident, base);
        }
        if (auto inner = dynamic_cast<Member*>(base)) {
            auto un = dynamic_cast<Name*>(inner->obj.get());
            if (!un || lookup_local(un->ident)) return nullptr;
            auto it = infos_.find(un->ident);
            if (it == infos_.end() || un->ident == cur_->name) return nullptr;
            if (!unit_imported(un->ident) || !unit_visible(un->ident)) return nullptr;
            TypePtr t = find_type_named(un->ident, inner->name, false);
            if (t && !type_is_public(t)) return nullptr;
            return t;
        }
        return nullptr;
    }

    bool Sema::eval_const_member_expr(Member* e, Sema::ConstVal& out) {
        TypePtr et = const_expr_base_type(e->obj.get());
        if (!et) return false;  // `obj.Name` is not a compile-time constant (constants do not occupy instance memory)
        if (et->kind == TypeKind::Enum) {
            EnumConst* ec = find_enum_member(et, e->name);
            if (!ec) return false;
            out.ty = ec->ty;
            out.i = ec->value;
            return true;
        }
        if (ConstMember* cm = find_const_member_in(et, e->name)) {
            if (!const_member_visible(cm)) {
                err("const member `" + e->name + "` is not declared public (type `" +
                        cm->owner->str() + "`)",
                    e);
            }
            return eval_const_member(cm, out);
        }
        return false;
    }

    bool Sema::eval_const_binary(Binary* e, const TypePtr& self_ty, Sema::ConstVal& out) {
        ConstVal l, r;
        if (!eval_const(e->left.get(), self_ty, l)) return false;
        if (!eval_const(e->right.get(), self_ty, r)) return false;
        const std::string& op = e->op;

        if (op == "and" || op == "or") {
            if (!(l.ty && l.ty->is_bool() && r.ty && r.ty->is_bool())) return false;
            out.ty = t_bool();
            out.i = (op == "and") ? ((l.i != 0) && (r.i != 0)) : ((l.i != 0) || (r.i != 0));
            return true;
        }

        if (op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=") {
            bool res = false;
            if (l.ty && r.ty && l.ty->kind == TypeKind::Enum &&
                r.ty->kind == TypeKind::Enum) {
                // An enum is backed by a signed int
                res = cmp_const(op, l.i, r.i);
            } else {
                // Consistent with run time: first compute the common type via usual_arith, round the operands to it, then compare
                TypePtr ct = usual_arith(l.ty, r.ty);
                if (!ct) return false;
                const ConstVal a = cast_const(l, ct);
                const ConstVal b = cast_const(r, ct);
                if (ct->is_float()) {
                    res = cmp_const(op, a.f, b.f);
                } else if (ct->is_signed()) {
                    res = cmp_const(op, a.i, b.i);
                } else {
                    res = cmp_const(op, static_cast<unsigned long long>(a.i),
                                    static_cast<unsigned long long>(b.i));
                }
            }
            out.ty = t_bool();
            out.i = res ? 1 : 0;
            return true;
        }

        if (op == "<<" || op == ">>") {
            if (!l.ty || !r.ty || !l.ty->is_integer() || !r.ty->is_integer()) return false;
            if (l.ty->kind == TypeKind::Enum || r.ty->kind == TypeKind::Enum) return false;
            const int bits = l.ty->bits() > 0 ? l.ty->bits() : 64;
            if (r.i < 0 || r.i >= bits) return false;
            long long v = l.i;
            if (op == "<<") {
                v = static_cast<long long>(static_cast<unsigned long long>(v) << r.i);
            } else {
                // Run time unconditionally uses AShr (arithmetic right shift by the declared width): the constant path must agree,
                // so sign-extend the two's-complement bit pattern to 64 bits by that width, shift arithmetically, then normalize.
                v = sext_bits(v, bits) >> r.i;
            }
            out.ty = l.ty;
            out.i = normalize_int(v, l.ty);
            return true;
        }

        if (l.is_float || r.is_float) {
            if (op == "%") return false;  // floating-point modulo has no semantics
            TypePtr rt = usual_arith(l.ty, r.ty);
            if (!rt) return false;
            // Round the operands to the common type first (float uses float precision), then compute
            const ConstVal a = cast_const(l, rt);
            const ConstVal b = cast_const(r, rt);
            const double av = a.f;
            const double bv = b.f;
            if (op == "/" && bv == 0.0) err("division by zero in constant expression", e);
            double v = 0;
            if (op == "+") v = av + bv;
            else if (op == "-") v = av - bv;
            else if (op == "*") v = av * bv;
            else if (op == "/") v = av / bv;
            else return false;
            out.ty = rt;
            out.is_float = true;
            out.f = rt->name == "float" ? static_cast<double>(static_cast<float>(v)) : v;
            return true;
        }

        TypePtr rt = usual_arith(l.ty, r.ty);
        if (!rt) return false;
        // Round the operands to the common type first, consistent with run-time convertTo
        const ConstVal a = cast_const(l, rt);
        const ConstVal b = cast_const(r, rt);
        const long long li = a.i;
        const long long ri = b.i;
        if ((op == "/" || op == "%") && ri == 0) {
            err(op == "/" ? "division by zero in constant expression"
                          : "remainder by zero in constant expression",
                e);
        }
        const bool uns = !rt->is_signed();
        // Signed division/remainder overflow (`MIN / -1`) is undefined behavior (SPEC 8.2), so a constant
        // expression reports it instead of producing a value the runtime cannot reproduce (the hardware traps).
        if (!uns && ri == -1) {
            const int bits = rt->bits() > 0 ? rt->bits() : 64;
            const long long lo = bits >= 64 ? LLONG_MIN : -(1LL << (bits - 1));
            if (li == lo) {
                err(op == "/" ? "signed division overflow in constant expression (the most negative value divided by -1 is undefined behavior)"
                              : "signed remainder overflow in constant expression (the most negative value % -1 is undefined behavior)",
                    e);
            }
        }
        long long v = 0;
        if (op == "+") v = static_cast<long long>(static_cast<unsigned long long>(li) + static_cast<unsigned long long>(ri));
        else if (op == "-") v = static_cast<long long>(static_cast<unsigned long long>(li) - static_cast<unsigned long long>(ri));
        else if (op == "*") v = static_cast<long long>(static_cast<unsigned long long>(li) * static_cast<unsigned long long>(ri));
        else if (op == "/") {
            v = uns ? static_cast<long long>(static_cast<unsigned long long>(li) / static_cast<unsigned long long>(ri))
                    : li / ri;
        } else if (op == "%") {
            v = uns ? static_cast<long long>(static_cast<unsigned long long>(li) % static_cast<unsigned long long>(ri))
                    : li % ri;
        } else if (op == "&") v = li & ri;
        else if (op == "|") v = li | ri;
        else if (op == "^") v = li ^ ri;
        else return false;
        out.ty = rt;
        out.i = normalize_int(v, rt);
        return true;
    }

    bool Sema::eval_const(Expr* e, const TypePtr& self_ty, Sema::ConstVal& out) {
        if (!e) return false;
        if (const_depth_ >= kMaxConstDepth) {
            err("constant expression is too deeply nested", e);
        }
        ConstDepthGuard depth(*this);
        if (auto il = dynamic_cast<IntLit*>(e)) {
            out.ty = (il->value >= -(1LL << 31) && il->value < (1LL << 31)) ? t_int()
                                                                           : Type::make_prim("long");
            out.i = il->value;
            out.lit = true;
            return true;
        }
        if (auto cl = dynamic_cast<CharLit*>(e)) {
            out.ty = Type::make_prim("byte");
            out.i = cl->value;
            out.lit = true;
            out.clit = true;
            return true;
        }
        if (auto bl = dynamic_cast<BoolLit*>(e)) {
            out.ty = t_bool();
            out.i = bl->value ? 1 : 0;
            return true;
        }
        if (auto fl = dynamic_cast<FloatLit*>(e)) {
            out.ty = Type::make_prim(fl->is_float32 ? "float" : "double");
            out.is_float = true;
            out.f = fl->is_float32 ? static_cast<double>(static_cast<float>(fl->value)) : fl->value;
            out.flit = true;
            return true;
        }
        if (auto nm = dynamic_cast<Name*>(e)) return eval_const_name(nm, self_ty, out);
        if (auto m = dynamic_cast<Member*>(e)) return eval_const_member_expr(m, out);
        if (auto sz = dynamic_cast<SizeofExpr*>(e)) {
            TypePtr t = resolve_type(sz->spec.get(), cur_);
            if (!t || is_void(t)) return false;
            out.ty = Type::make_prim("usize");
            out.i = static_cast<long long>(sz->which == "alignof" ? type_align(t) : type_size(t));
            return true;
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            ConstVal v;
            if (!eval_const(u->operand.get(), self_ty, v)) return false;
            if (!v.ty || !v.ty->is_numeric() || v.ty->kind == TypeKind::Enum) return false;
            out = v;
            out.lit = false;
            out.flit = false;
            if (u->op == "+") return true;
            if (u->op == "-") {
                if (v.is_float) out.f = -v.f;
                else out.i = normalize_int(-v.i, v.ty);
                return true;
            }
            if (u->op == "~") {
                if (!v.ty->is_integer()) return false;
                out.i = normalize_int(~v.i, v.ty);
                return true;
            }
            return false;
        }
        if (auto nt = dynamic_cast<NotExpr*>(e)) {
            ConstVal v;
            if (!eval_const(nt->operand.get(), self_ty, v)) return false;
            if (!v.ty || !v.ty->is_bool()) return false;
            out.ty = t_bool();
            out.i = v.i ? 0 : 1;
            return true;
        }
        if (auto c = dynamic_cast<Cast*>(e)) {
            ConstVal v;
            if (!eval_const(c->expr.get(), self_ty, v)) return false;
            TypePtr dt = resolve_type(c->spec.get(), cur_);
            if (!dt || is_void(dt)) return false;
            if (!can_cast(v.ty, dt)) {
                err("cannot use `as` to convert " + (v.ty ? v.ty->str() : "void") + " to " +
                        dt->str(),
                    e);
            }
            out = cast_const(v, dt);
            return true;
        }
        if (auto b = dynamic_cast<Binary*>(e)) return eval_const_binary(b, self_ty, out);
        return false;
    }

    bool Sema::is_const_expr(Init* init) {
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            for (auto& item : ia->items) {
                if (!is_const_expr(item.second.get())) return false;
            }
            return true;
        }
        if (auto ie = dynamic_cast<InitExpr*>(init)) return const_ok(ie->expr.get());
        return false;
    }

    bool Sema::const_ok(Expr* e) {
        if (!e) return false;
        if (dynamic_cast<IntLit*>(e) || dynamic_cast<FloatLit*>(e) ||
            dynamic_cast<CharLit*>(e) || dynamic_cast<StringLit*>(e) ||
            dynamic_cast<BoolLit*>(e) || dynamic_cast<NullLit*>(e)) {
            return true;
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            return (u->op == "-" || u->op == "+") && const_ok(u->operand.get());
        }
        if (auto c = dynamic_cast<Cast*>(e)) return const_ok(c->expr.get());
        if (auto b = dynamic_cast<Binary*>(e)) {
            if (b->op == "and" || b->op == "or") return false;
            return const_ok(b->left.get()) && const_ok(b->right.get());
        }
        return false;
    }
}  // namespace muc_detail
