// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds assignment compatibility and compile-time constant evaluation (definitions of Sema members).
//
// owns:      assignment compatibility and the compile-time constant evaluator (eval_const*), including the conversions used by #if, array sizes and align
// not:       expression typing or call resolution
// pass:      queried from every pass that needs a constant
// concept:   constants, assignability
#include "sema_impl.hpp"

#include <sstream>

#include <cmath>

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

    // Sum types are a language built-in whose layout (tag + union payload) is an implementation detail,
    // not a public interface: a value of such a type never crosses into C (SPEC 10.1.1).
    bool Sema::contains_sum_type(const TypePtr& t) {
        if (!t) return false;
        if (t->kind == TypeKind::Sum) return true;
        if (t->kind == TypeKind::Array || t->kind == TypeKind::Slice) {
            return contains_sum_type(t->elem);
        }
        if (t->kind == TypeKind::Struct) {
            for (const auto& f : t->fields) {
                if (f.weak) continue;
                if (contains_sum_type(f.type)) return true;
            }
        }
        return false;
    }

    // A delegate owns a reference-counted environment: C can neither copy nor destroy it, so a
    // nested delegate has the same problem as a nested class handle (SPEC 7).
    bool Sema::contains_delegate_type(const TypePtr& t) {
        if (!t) return false;
        if (t->kind == TypeKind::Delegate) return true;
        if (t->kind == TypeKind::Array || t->kind == TypeKind::Slice) {
            return contains_delegate_type(t->elem);
        }
        if (t->kind == TypeKind::Struct) {
            for (const auto& f : t->fields) {
                if (f.weak) continue;
                if (contains_delegate_type(f.type)) return true;
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

    // Copyability is recursive: a struct that contains an uncopyable struct is uncopyable itself,
    // so a host type cannot silently regain the ability to copy an owned value
    // (agents/value-semantics.md §2). A fixed-size array owns its elements, so an array of such a
    // struct is not copyable either — a payload of `R[1]` must be rejected like `R` (#92).
    bool Sema::noncopyable_struct(const TypePtr& t) {
        if (!t) return false;
        if (t->kind == TypeKind::Array) return noncopyable_struct(t->elem);
        return t->kind == TypeKind::Struct && !t->copyable();
    }

    std::string Sema::noncopyable_reason(const TypePtr& t) const {
        if (!t) return "it is not copyable";
        if (t->kind == TypeKind::Array) return "its element type " + noncopyable_reason(t->elem);
        if (t->kind == TypeKind::Struct) {
            if (t->nocopy) return "it is declared `nocopy`";
            if (t->has_dtor) return "it declares a destructor, so it owns a resource";
            return "it contains a value that owns a resource";
        }
        return "it is not copyable";
    }

    TypePtr Sema::check_assign(Expr* e, const TypePtr& dst, const std::string& what) {
        // `move x` is the sanctioned way to produce an uncopyable value: `check_move` already verified
        // that the type is movable (and empties the source).
        if (noncopyable_struct(dst) && !dynamic_cast<MoveExpr*>(e)) {
            // A prvalue of the same type is moved, not copied: a construction (`Token(5)`) and a call
            // whose result is the value itself (`a = f();` — the callee's return slot is handed over).
            // A call returning a *borrow* (`ref T f()`) is not a prvalue: it must still be copied.
            bool prvalue = false;
            if (auto call = dynamic_cast<Call*>(e); call && !call->by_ref) prvalue = true;
            if (prvalue) {
                TypePtr built = check_expr(e, dst);
                if (built && same_type(built, dst)) return dst;
            }
            err(what + ": struct `" + dst->str() + "` cannot be copied by value: " +
                    noncopyable_reason(dst) + "; declare `" + dst->str() + "(const ref " + dst->str() +
                    " other)`" + (dst->movable() ? ", or move it with `move`" : "") +
                    (borrowable_type(dst) ? ", or borrow it as `const ref " + dst->str() + "`" : ""),
                e);
        }
        // Contextual alternative (SPEC 10.1.1): when the target is a sum type, a bare alternative name
        // means that alternative — `Option<int> o = Some(3);` / `o = None;`. A user function with the
        // same name wins (the usual call resolution), so this only applies to unknown names.
        if (dst && dst->kind == TypeKind::Sum) {
            auto alt_index = [&](const std::string& nm) -> int {
                for (size_t k = 0; k < dst->sum_names.size(); ++k) {
                    if (dst->sum_names[k] == nm) return static_cast<int>(k);
                }
                return -1;
            };
            auto name_is_free = [&](const std::string& nm) {
                // A local variable still wins (a name in scope is a value, not an alternative). A user
                // function of the same name does not: the target type is this sum, so `Some(3)` has to
                // construct here (`check_call` follows the same rule) while the function keeps serving
                // every non-sum target.
                return lookup_local(nm) == nullptr;
            };
            if (auto call = dynamic_cast<Call*>(e); call && call->callee_kind.empty()) {
                if (auto nm = dynamic_cast<Name*>(call->callee.get());
                    nm && name_is_free(nm->ident)) {
                    const int idx = alt_index(nm->ident);
                    if (idx >= 0) {
                        const auto& fs = dst->sum_fields[static_cast<size_t>(idx)];
                        if (call->args.size() != fs.size()) {
                            err("alternative `" + nm->ident + "` of `" + dst->str() + "` takes " +
                                    std::to_string(fs.size()) + " value(s), but " +
                                    std::to_string(call->args.size()) + " were given",
                                call);
                        }
                        for (size_t k = 0; k < fs.size(); ++k) {
                            check_assign(call->args[k].get(), fs[k], "sum payload");
                        }
                        nm->kind = "type";
                        nm->ty = dst;
                        call->callee_kind = "sum_ctor";
                        call->sum_ty = dst;
                        call->sum_alt = idx;
                        call->ty = dst;
                        return dst;
                    }
                }
            }
            if (auto nm = dynamic_cast<Name*>(e); nm && nm->kind.empty() && name_is_free(nm->ident)) {
                const int idx = alt_index(nm->ident);
                if (idx >= 0 && dst->sum_fields[static_cast<size_t>(idx)].empty()) {
                    nm->kind = "sum_unit";
                    nm->sum_ty = dst;
                    nm->sum_alt = idx;
                    nm->ty = dst;
                    return dst;
                }
            }
        }
        TypePtr src = check_expr(e, dst);
        if (assignable(e, src, dst)) return src;
        if (const ImplicitConv* conv = find_implicit_conv(dst, src)) {
            e->conv = conv;
            return dst;  // the effective type after the conversion
        }
        if (is_enum_t(src) || is_enum_t(dst)) {
            err(what + ": cannot assign " + (src ? src->str() : "void") + " to " +
                    (dst ? dst->str() : "void") + " (no implicit conversion between enums and integers; write `as`)",
                e);
        }
        // A read-only view handed to a mutable one is the one conversion worth naming: the fix is not a
        // different type but an explicit, unsafe `as` (SPEC 3.6/3.7).
        if (src && dst && (src->kind == TypeKind::Class || src->kind == TypeKind::Interface) &&
            (dst->kind == TypeKind::Class || dst->kind == TypeKind::Interface) && read_only_view(src) &&
            !read_only_view(dst)) {
            err(what + ": `" + (src->str()) + "` is a read-only view and does not convert to the mutable `" +
                    dst->str() + "`; write `as " + dst->str() + "` to force it (writing through it is UB)",
                e);
        }
        err(what + ": cannot assign " + (src ? src->str() : "void") + " to " +
                (dst ? dst->str() : "void"),
            e);
    }

    bool Sema::assignable(Expr* e, const TypePtr& src, const TypePtr& dst) {
        // C99 6.3.2.1: an array is not a modifiable lvalue and no expression has array type after
        // decay — `a = b` and `int[N] b = a;` are errors (copy element by element, or use `memcpy`).
        // Two Mu extensions keep copying: an array-typed `return`, and initialization from a prvalue
        // array (a function result; there is no array expression to alias in that case).
        if (dst && dst->kind == TypeKind::Array && src && src->kind == TypeKind::Array &&
            !in_return_ && is_lvalue(e)) {
            err("`" + dst->str() +
                    "` is an array: arrays are not assignable (C99); copy the elements or use `memcpy`",
                e);
        }
        // C model: const may only be relaxed, never tightened (same_type ignores const, so this must be
        // caught first). A read-only view never widens to a mutable one implicitly: `const T*` / `const T[]`,
        // and for handles a `const` view (`const Base base`) — the escape is `as` (SPEC 3.6, 3.7).
        if (src && dst) {
            const bool handle_src = src->kind == TypeKind::Class || src->kind == TypeKind::Interface;
            const bool handle_dst = dst->kind == TypeKind::Class || dst->kind == TypeKind::Interface;
            if (handle_src && handle_dst) {
                const bool src_ro = read_only_view(src);
                if (src_ro && !read_only_view(dst)) return false;
            }
        }
        if (src && dst && src->kind == dst->kind &&
            (src->kind == TypeKind::Ptr || src->kind == TypeKind::Slice) &&
            src->elem && dst->elem && src->elem->is_const && !dst->elem->is_const) {
            return false;
        }
        if (same_type_sig(src, dst)) return true;
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
        // A string literal initializes a fixed-size char/byte array (copy + NUL padding, 3.4).
        if (dst && dst->kind == TypeKind::Array && is_char(dst->elem) && dynamic_cast<StringLit*>(e)) {
            const std::string& bytes = static_cast<StringLit*>(e)->value;
            if (bytes.size() >= dst->array_size) {
                err("string literal (" + std::to_string(bytes.size()) + " bytes) does not fit in `" +
                        dst->str() + "` (it needs room for the terminating NUL)",
                    e);
            }
            return true;
        }
        if (src && dst && src->kind == TypeKind::Array && dst->kind == TypeKind::Slice) {
            // T[N] -> T[]; T[m,n] -> T[,] (match rank by nesting depth, element types must agree)
            TypePtr base = src;
            size_t depth = 0;
            while (base && base->kind == TypeKind::Array) {
                base = base->elem;
                depth++;
            }
            if (static_cast<int>(depth) == dst->rank && same_type_sig(base, dst->elem)) {
                if (type_align(base) > 8) err_slice_elem_align(base, e);
                return true;
            }
            return false;
        }
        return implicit_ok(src, dst);
    }

    const ImplicitConv* Sema::find_implicit_conv(const TypePtr& dst, const TypePtr& src) {
        // Process-level pool: `Expr::conv` must stay valid after Sema is destroyed (codegen runs later).
        static std::deque<ImplicitConv> pool;
        if (!dst || !src) return nullptr;
        // `null` is not a value that a conversion may consume: `s == null` must stay an error rather than
        // building a string from a null C string.
        if (src->kind == TypeKind::Null) return nullptr;
        if (dst->kind != TypeKind::Struct) return nullptr;  // value types only: a class conversion would be a hidden allocation
        if (same_type(dst, src)) return nullptr;
        for (size_t i = 0; i < dst->ctors.size(); ++i) {
            if (i >= dst->ctor_is_implicit.size() || !dst->ctor_is_implicit[i]) continue;
            if (i >= dst->ctor_is_public.size() || !dst->ctor_is_public[i]) continue;
            const std::vector<TypePtr>& p = dst->ctors[i];
            if (p.size() != 1) continue;
            if (!implicitly_convertible(src, p[0])) continue;
            for (auto& c : pool) {
                if (same_type(c.to, dst) && c.pty.size() == 1 && same_type(c.pty[0], p[0])) return &c;
            }
            pool.push_back(ImplicitConv{dst, p, ctor_cname(dst->unit, dst->name, p)});
            return &pool.back();
        }
        return nullptr;
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
        long long i = v.i;
        if (v.is_float) {
            // Saturate before narrowing: `static_cast<long long>` of an out-of-range double is undefined
            // on the host (the checked paths reject such a value first; this keeps the helper safe even
            // if a caller reaches it). `normalize_int` then applies the target width.
            const double f = v.f;
            if (f != f) i = 0;
            else if (f >= 9223372036854775807.0) i = 9223372036854775807LL;
            else if (f <= -9223372036854775808.0) i = (-9223372036854775807LL - 1);
            else i = static_cast<long long>(f);
        }
        r.i = normalize_int(i, dst);
        return r;
    }

    void Sema::convert_const_to(Sema::ConstVal& v, const TypePtr& dst, const Node* at,
                          const std::string& what) {
        if (!dst) return;
        if (same_type_sig(v.ty, dst)) {
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
            // Out-of-range shift counts are UB at run time (8.2): report them here instead of returning
            // "not evaluable", which used to let `check` pass and fail later in the backend.
            if (r.i < 0) err("shift count " + std::to_string(r.i) + " must be non-negative", e);
            if (r.i >= bits) {
                err("shift count " + std::to_string(r.i) + " is out of range for `" + l.ty->str() +
                        "` (0.." + std::to_string(bits - 1) +
                        "); shifting by the bit width or more is undefined behavior",
                    e);
            }
            long long v = l.i;
            if (op == "<<") {
                v = static_cast<long long>(static_cast<unsigned long long>(v) << r.i);
            } else if (l.ty->kind == TypeKind::Prim && !l.ty->is_signed()) {
                // Unsigned: the run-time instruction is LShR, so zero-extend the bit pattern of the
                // declared width to 64 bits and shift that.
                const unsigned long long mask = bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
                v = static_cast<long long>((static_cast<unsigned long long>(v) & mask) >> r.i);
            } else {
                // Signed: AShr sign-extends from the declared width, so do the same here.
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
            // A `2^63` magnitude is only meaningful directly after unary `-`; the parser consumes it
            // there, so one reaching constant evaluation is out of range (otherwise it would silently
            // be the value 0).
            if (il->min_magnitude) {
                err("integer literal is too large (maximum 2^63-1): a 2^63 magnitude is only valid "
                    "directly after unary `-`",
                    e);
            }
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
            if (!sz->spec) return false;  // `sizeof expr`: a value expression is not a compile-time constant
            TypePtr t = resolve_type(sz->spec.get(), cur_);
            if (!t || is_void(t)) return false;
            out.ty = Type::make_prim("isize");
            if (sz->which == "offsetof") {
                int idx = -1;
                if (t->kind != TypeKind::Struct) return false;
                for (size_t i = 0; i < t->fields.size(); ++i) {
                    if (t->fields[i].name == sz->field) idx = static_cast<int>(i);
                }
                if (idx < 0) return false;
                out.i = static_cast<long long>(type_member_offset(t, static_cast<size_t>(idx)));
                return true;
            }
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
                if (v.is_float) {
                    out.f = -v.f;
                } else {
                    out.i = normalize_int(-v.i, v.ty);
                    // A negated literal is still a literal: `const int MIN = -2147483648;` spells its
                    // magnitude through 2147483648, which only the target type can place (SPEC 3.4).
                    out.lit = v.lit;
                }
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
            // Constant evaluation agrees with the checked run time (SPEC 8.2): a float->int conversion
            // whose value is NaN or outside the target's range has no value the program could
            // reproduce, so it is an error here rather than a poisoned constant.
            if (v.is_float && dt->kind == TypeKind::Prim && dt->is_integer()) {
                const int bits = dt->bits();
                const double lo = dt->is_signed() ? -std::ldexp(1.0, bits - 1) : 0.0;
                const double hi = dt->is_signed() ? std::ldexp(1.0, bits - 1)
                                                  : std::ldexp(1.0, bits);
                if (!(v.f >= lo && v.f < hi)) {  // NaN fails both comparisons
                    std::ostringstream os;
                    if (v.f != v.f) os << "nan";
                    else os << v.f;  // default formatting: 1e+30, not a 30-digit fixed-point number
                    err("the value " + os.str() + " does not fit `" + dt->str() +
                            "`; a compile-time `as` conversion must be in range (a run-time one is "
                            "checked by `--bounds`)",
                        e);
                }
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
