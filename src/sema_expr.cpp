// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 本文件包含 表达式类型检查（Sema 成员函数定义）。
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
            // long long 本身已在 int64 范围内；超出 64 位由 lexer/parser 报错
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
            return Type::make_prim("byte");  // 字符字面量默认是字节
        }
        if (dynamic_cast<StringLit*>(e)) return str_type();
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

        if (auto nm = dynamic_cast<Name*>(e)) return check_name(nm);
        if (auto m = dynamic_cast<Member*>(e)) return check_member(m);

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
            if (is_str(bt)) {
                if (n != 1) err("`string` accepts exactly one subscript", e);
                return Type::make_prim("byte");
            }
            if (is_user_type(bt)) {
                if (n != 1) err("user-defined `operator[]` takes exactly one subscript", e);
                auto cands = collect_operators("[]", {bt, its[0]});
                if (cands.empty()) {
                    err("type `" + bt->str() + "` does not support subscripting (no `operator[]`)", e);
                }
                auto f = choose_overload(cands, {ix->base.get(), ix->indices[0].get()},
                                         "operator[]", e);
                ix->get_func = f;
                return f->rty;
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
            TypePtr t = resolve_type(sz->spec.get(), cur_);
            if (is_void(t)) err("`" + sz->which + "(void)` is invalid", e);
            e->ty = Type::make_prim("usize");
            return e->ty;
        }

        if (auto ln = dynamic_cast<Len*>(e)) {
            TypePtr t = check_expr(ln->seq.get());
            if (is_str(t)) {
                if (ln->dim) err("`string` length has no dimension argument", ln->dim.get());
                return Type::make_prim("usize");
            }
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

    TypePtr Sema::check_name(Name* e) {
        if (e->ident == "super") {
            err("`super` can only be used as `super(...)` or `super.method(...)`", e);
        }
        if (e->ident == "this" && lookup_local("this") == nullptr) {
            err("`this` can only appear inside a class or struct method, constructor, or destructor", e);
        }
        if (LocalVar* lv = lookup_local(e->ident)) {
            e->kind = "local";
            e->sym = lv;
            return lv->rty;
        }
        if (cur_class_) {
            if (TypePtr ft = cur_class_->field_type(e->ident)) {
                e->kind = "field";
                e->implicit_this = true;
                e->field_index = cur_class_->field_index(e->ident);
                return ft;
            }
            // 类型级常量：在声明它的类型（及其派生类）的方法体内可裸写
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
            e->sym = f->second.front();
            err("function `" + e->ident + "` cannot be used as a value", e);
        }
        if (EnumConst* ec = lookup_enum_const(e->ident)) {
            e->kind = "enum_const";
            e->sym = ec;
            return ec->ty;
        }

        // 从直接 use 的 unit 导入（符号直接可见；多个来源同名时报歧义要求限定名）
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
            e->sym = imp_f;
            err("function `" + e->ident + "` cannot be used as a value", e);
        }
        if (imp_ec) {
            e->kind = "enum_const";
            e->sym = imp_ec;
            return imp_ec->ty;
        }
        err("undefined identifier `" + e->ident + "`", e);
    }

    TypePtr Sema::check_member(Member* e) {
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
                    e->sym = f->second.front();
                    err("function `" + uname + "." + e->name + "` cannot be used as a value", e);
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

        // 枚举限定引用：`Color.Red` / `unit.Color.Red`
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
                        // 类型完全没有类型级常量时保持旧的「不是枚举」诊断
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
                // 类型级常量不占实例内存：obj.Name / this.Name 一律拒绝，提示改用 Type.Name
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
            if (auto m = dynamic_cast<Member*>(e->operand.get()); m && m->kind == "enum_const") {
                err("enum members are not addressable", e);
            }
            if (auto m = dynamic_cast<Member*>(e->operand.get()); m && m->kind == "const_member") {
                err("const members are not addressable", e);
            }
            if (auto n = dynamic_cast<Name*>(e->operand.get()); n && n->kind == "const_member") {
                err("const members are not addressable", e);
            }
            // U21：RC 生命周期无法用裸指针表达——禁止对 class/interface 句柄、
            // 以及经引用访问的成员/数组元素取地址（`&c`、`&c.x`、`&h.items[0]`、`&catArray[i]`）。
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
            // U24：含引用字段的 struct 由编译器逐字段 RC；裸指针可绕过 retain/release
            // （也能把弱引用/字段地址带到生命周期之外），第一阶段禁止取址。
            if (t && t->kind == TypeKind::Struct && has_ref_fields(t) && !t->has_dtor) {
                err("cannot take the address of struct `" + t->str() +
                        "` which has reference fields: a raw pointer could bypass RC (backlog U24); pass or return it by value, or use a class"
                        " (structs with a user-defined destructor are still used through pointers and are not affected)",
                    e);
            }
            return Type::make_ptr(t);
        }

        TypePtr t = check_expr(e->operand.get());

        // 枚举不是算术类型：一元运算（`~`/`-`/`+`）需要显式 `as int`
        if (is_enum_t(t) && (e->op == "~" || e->op == "-" || e->op == "+")) {
            err("enum `" + t->str() + "` does not support unary `" + e->op + "` (write `x as int`)", e);
        }

        if (e->op == "*") {
            if (t && t->kind == TypeKind::Ptr) return t->elem;
            err("`*` requires a pointer, found " + (t ? t->str() : "void"), e);
        }
        if (e->op == "~") {
            if (is_integer(t)) return t;
            if (is_user_type(t)) {
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
        // 期望类型透传给两个分支：字面量按上下文适配（`char c = pick ? 'a' : 'b';`）
        TypePtr a = check_expr(e->then_e.get(), expected);
        TypePtr b = check_expr(e->else_e.get(), expected);
        if (!a || !b) err("`?:` is missing a branch expression", e);
        adapt_lit(e->then_e.get(), a, b);
        adapt_lit(e->else_e.get(), b, a);
        a = e->then_e->ty;
        b = e->else_e->ty;

        // null 字面量适配到另一侧（指针 / class / interface / 动态数组）
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

        // 枚举是名义类型：只与同一枚举类型比较；与整数之间没有隐式转换（双向都要 `as`）
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
                if (is_str(other)) {
                    err("`string` cannot be compared with `null` (use `len(s) == 0` or `s == \"\"`)", e);
                }
                if (other->kind == TypeKind::Slice) {
                    err("slice cannot be compared with `null` (use `len(s) == 0`)", e);
                }
                if (other->kind == TypeKind::Ptr || other->kind == TypeKind::Slice ||
                    other->kind == TypeKind::Null || other->kind == TypeKind::Class ||
                    other->kind == TypeKind::Interface) {
                    return t_bool();
                }
            }
            if (is_str(lt) && is_str(rt)) return t_bool();
            if (is_numeric(lt) && is_numeric(rt)) return t_bool();
            const bool eq = same_type(lt, rt);
            if ((eq && (lt->kind == TypeKind::Ptr || lt->kind == TypeKind::Enum ||
                        lt->kind == TypeKind::Class || lt->kind == TypeKind::Interface)) ||
                (is_prim(lt) && lt->name == "bool" && same_type(rt, lt))) {
                return t_bool();
            }
            // 引用类型之间：可相互转换（基类/接口）即可做身份比较
            if (is_reference_type(lt) && is_reference_type(rt) &&
                (implicitly_convertible(lt, rt) || implicitly_convertible(rt, lt))) {
                return t_bool();
            }
            if (is_user_type(lt) || is_user_type(rt)) {
                auto cands = collect_operators(op, {lt, rt});
                if (cands.empty() && op == "!=") {
                    auto eqs = collect_operators("==", {lt, rt});
                    if (!eqs.empty()) {
                        choose_overload(eqs, {e->left.get(), e->right.get()}, "operator==", e);
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
            // 字符字面量是字节：按对端的 8 位整数类型适配（char/byte）
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
        // 枚举是名义类型：只与整数（非枚举）显式互转，且不与浮点互换
        if (src->kind == TypeKind::Enum || dst->kind == TypeKind::Enum) {
            if (src->kind == TypeKind::Enum && dst->kind == TypeKind::Enum) return false;
            return src->kind == TypeKind::Enum ? is_plain_int(dst) : is_plain_int(src);
        }
        if (is_numeric(src) && is_numeric(dst)) return true;
        // 引用类型：向上/向下转换（向下在运行期检查，失败得 null）
        if (is_class_type(src) && is_class_type(dst)) {
            return is_subclass_of(src, dst) || is_subclass_of(dst, src);
        }
        if (is_interface_type(src) && is_interface_type(dst)) {
            return implements_interface(src, dst);  // 仅向上；向下暂缓
        }
        if (is_interface_type(src) && is_class_type(dst)) return true;
        if (src->kind == TypeKind::Ptr && dst->kind == TypeKind::Ptr) return true;
        if (src->kind == TypeKind::Slice && dst->kind == TypeKind::Slice) {
            return src->rank == dst->rank && same_type(src->elem, dst->elem);
        }
        if (src->kind == TypeKind::Ptr && is_integer(dst) && dst->bits() >= 64) return true;
        if (dst->kind == TypeKind::Ptr && is_integer(src) && src->bits() >= 64) return true;
        if (is_str(src) && dst->kind == TypeKind::Ptr && dst->elem &&
            dst->elem->is_const &&
            dst->elem->kind == TypeKind::Prim && dst->elem->bits() == 8) {
            return true;  // Str -> readonly char*/byte*（要求 NUL 结尾）
        }
        return false;
    }
}  // namespace muc_detail
