// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 本文件包含 class / interface 检查（下）（Sema 成员函数定义）。
#include "sema_impl.hpp"

namespace muc_detail {

    void Sema::check_aggregate(InitAggregate* init, const TypePtr& ty, const std::string& what) {
        auto& items = init->items;

        if (ty->kind == TypeKind::Struct && ty->is_union) {
            // union：只允许「恰好一个具名成员」；不提供位置初始化（显式优于隐式）
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
            // 跨 unit 的聚合初始化要求字段全部 public
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
            if (items.size() > ty->array_size) {
                err("array of length " + std::to_string(ty->array_size) + " was given " +
                        std::to_string(items.size()) + " initializers",
                    init);
            }
            for (auto& item : items) check_init_into(ty->elem, item.second.get(), what, init);
            return;
        }

        if (ty->kind == TypeKind::Slice) {
            if (type_align(ty->elem) > 8) err_slice_elem_align(ty->elem, init);
            if (ty->rank != 1) {
                err("dynamic rectangular array `" + ty->str() + "` must be created with `new T[m,n]`", init);
            }
            for (auto& item : items) {
                if (!item.first.empty()) err("slice initializers cannot use named fields", init);
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
            if (root_is_const(t)) err("`" + n->ident + "` is a `const` binding and cannot be assigned", t);
            return;
        }
        if (dynamic_cast<Member*>(t)) {
            if (root_is_const(t)) err("cannot modify a member of a `const` binding", t);
            return;
        }
        if (auto ix = dynamic_cast<Index*>(t)) {
            TypePtr bt = ix->base->ty;
            if (is_str(bt)) err("`string` is immutable; cannot write to elements (use `byte[]` or `str_concat` to build one)", t);
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
        if (!is_lvalue(s->target.get())) err("left side of an assignment must be assignable", s);

        if (auto ix = dynamic_cast<Index*>(s->target.get())) {
            TypePtr bt = check_expr(ix->base.get());
            if (is_user_type(bt)) {
                if (is_str(bt))
                    err("`string` is immutable; cannot write to elements (use `byte[]` or `str_concat` to build one)",
                        s->target.get());
                if (ix->indices.size() != 1)
                    err("user-defined `operator[]=` takes exactly one index", s->target.get());
                TypePtr it = check_expr(ix->indices[0].get());
                if (!is_integer(it)) err("index must be an integer", s->target.get());
                if (s->op != "=") err("user-defined subscript assignment only supports `=`", s);
                auto cands = collect_operators("[]=", {bt, it});
                if (cands.empty()) {
                    err("type `" + bt->str() + "` does not support subscript assignment (no `operator[]=`)", s);
                }
                check_writable(s->target.get());
                s->set_func = choose_overload(cands,
                                              {ix->base.get(), ix->indices[0].get(),
                                               s->value.get()},
                                              "operator[]=", s);
                return;
            }
        }

        TypePtr tt = check_expr(s->target.get());
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
        if (in_dtor_) err("`return` is not allowed in a destructor (fields and the base class are destroyed at the end of the function)", s);
        if (!cur_ret_) err("`return` can only appear in a function or method", s);
        TypePtr rt = cur_ret_;
        if (!s->value) {
            if (!is_void(rt)) {
                err("`" + cur_name_ + "` must return a value of type " + (rt ? rt->str() : "void"), s);
            }
            return;
        }
        if (is_void(rt)) err("`void` function or method `" + cur_name_ + "` cannot return a value", s);
        check_assign(s->value.get(), rt, "return value");
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
            // 成员名直接按被匹配的枚举解析（`Red` 不必写前缀）
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
            check_expr(e);  // 解析成变量 / 枚举成员，好给出更准的错误
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

        // 重叠检测：按 [lo, hi) 区间合并，top = 覆盖到类型的最大值
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
                    if (p.vlo >= p.vhi) {
                        err("switch range `" + std::to_string(p.vlo) + ".." +
                                std::to_string(p.vhi) +
                                "` is empty: the upper bound of a half-open range must be greater than the lower bound",
                            p.lo.get());
                    }
                    iv.hi = p.vhi;
                } else {
                    if (p.vlo == LLONG_MAX) {
                        iv.hi = p.vlo;
                        iv.top = true;
                        p.vhi = p.vlo;  // 仅内部使用；+1 会溢出
                    } else {
                        iv.hi = p.vlo + 1;
                        p.vhi = p.vlo + 1;  // 仅内部使用（p.vlo 是唯一命中值）
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
        s->elem_ty = picked->rty;
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
        } else {
            TypePtr st = check_expr(s->seq.get());
            if (!st) err("`for-in` is missing a sequence", s);
            if (st->kind == TypeKind::Array) {
                if (st->elem && st->elem->kind == TypeKind::Array) {
                    err("`for-in` does not support multidimensional arrays `" + st->str() +
                            "`: use an index loop, or take one row first and iterate over it",
                        s->seq.get());
                }
                s->loop_kind = "array";
                s->elem_ty = st->elem;
            } else if (st->kind == TypeKind::Slice) {
                if (st->rank > 1) {
                    err("`for-in` does not support multidimensional arrays `" + st->str() +
                            "`: use an index loop (get each dimension's length with `len(a, k)`)",
                        s->seq.get());
                }
                s->loop_kind = "slice";
                s->elem_ty = st->elem;
            } else if (is_str(st)) {
                s->loop_kind = "string";
                s->elem_ty = Type::make_prim("byte");
            } else if (st->kind == TypeKind::Struct || st->kind == TypeKind::Class) {
                resolve_index_shape(s, st);
            } else {
                err("`for-in` cannot iterate " + st->str() +
                        " (supported: range, array, slice, string; user types must provide `length() -> usize` and "
                        "`operator[](C, usize)`）",
                    s->seq.get());
            }
            s->seq_ty = st;
        }

        if (s->by_ref && s->loop_kind == "string") {
            err("`string` is immutable and cannot be iterated with `for ref`", s);
        }
        if (s->by_ref && s->loop_kind == "index") {
            err("user types do not support `for ref` (`operator[]` returns a value); use `for i, x` with `c[i] = ...` instead",
                s->seq.get());
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
        check_block(s->body.get());
        --loop_depth_;
    }
}  // namespace muc_detail
