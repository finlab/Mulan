// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 泛型单态化实例化（U34，M2）。
//
// 实例化 = 克隆模板 AST + 替换类型参数 + 规范化命名 + 走常规类型解析。
// 产物是普通 struct：`same_type` / `mangle` / 布局 / codegen 均无需感知泛型。
#include <string>
#include <utility>
#include <vector>

#include "ast_clone.hpp"
#include "ast_subst.hpp"
#include "sema_impl.hpp"

namespace muc_detail {

namespace {

// 类型语法 → 规范化显示名（`[const] Box<Vec<int[]>>`），用于 `Type::name` 与实例缓存键。
std::string canon_spec(const TypeSpec& s) {
    std::string prefix = s.is_const ? "const " : "";
    switch (s.kind) {
        case TypeSpec::K::Named: {
            std::string n = prefix + s.name;
            if (!s.args.empty()) {
                n += "<";
                for (size_t i = 0; i < s.args.size(); ++i) {
                    if (i) n += ", ";
                    n += canon_spec(*s.args[i]);
                }
                n += ">";
            }
            return n;
        }
        case TypeSpec::K::Ptr:
            return canon_spec(*s.elem) + "*";
        case TypeSpec::K::Slice:
            return canon_spec(*s.elem) + "[]";
        case TypeSpec::K::Array:
            return canon_spec(*s.elem) + "[" + std::to_string(s.size) + "]";
    }
    return prefix + s.name;
}

// 把 `a.b.C` 拆成 (a.b, C)；无前缀时 unit 为空
void split_qualified(const std::string& full, std::string& unit, std::string& name) {
    const auto dot = full.rfind('.');
    if (dot == std::string::npos) {
        unit.clear();
        name = full;
    } else {
        unit = full.substr(0, dot);
        name = full.substr(dot + 1);
    }
}

}  // namespace

void Sema::index_templates() {
    for (auto& u : units_) {
        for (auto& d : u->templates) {
            std::string name;
            if (auto s = dynamic_cast<StructDecl*>(d.get())) {
                name = s->name;
            } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                name = c->name;
            } else if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) {
                name = i->name;
            } else if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                name = f->name;
            } else {
                continue;
            }
            generic_templates_[u->name + "." + name] = d.get();
        }
    }
}

TypePtr Sema::instantiate_generic_named(const std::string& name,
                                        const std::vector<TypeSpecPtr>& args, Unit* u) {
    TypeSpec tmp;
    tmp.kind = TypeSpec::K::Named;
    tmp.name = name;
    for (const auto& a : args) tmp.args.push_back(a ? clone_type_spec(*a) : nullptr);
    return instantiate_generic(&tmp, u);
}

Decl* Sema::find_generic_template(const std::string& full, Unit* u) {
    std::string want_unit, name;
    split_qualified(full, want_unit, name);
    if (!want_unit.empty()) {
        auto it = generic_templates_.find(want_unit + "." + name);
        return it == generic_templates_.end() ? nullptr : it->second;
    }
    auto own = generic_templates_.find(u->name + "." + name);
    if (own != generic_templates_.end()) return own->second;
    for (const auto& kv : generic_templates_) {
        std::string tu, tn;
        split_qualified(kv.first, tu, tn);
        if (tn == name) return kv.second;  // 同名跨 unit：取第一个（M2d 简化）
    }
    return nullptr;
}

TypePtr Sema::instantiate_generic(const TypeSpec* spec, Unit* u) {
    std::string want_unit;
    std::string base;
    split_qualified(spec->name, want_unit, base);

    // 查找模板：显式 unit 前缀优先；否则本 unit → 唯一同名
    Decl* tmpl_decl = nullptr;
    std::string tmpl_unit;
    if (!want_unit.empty()) {
        auto it = generic_templates_.find(want_unit + "." + base);
        if (it != generic_templates_.end()) {
            tmpl_decl = it->second;
            tmpl_unit = want_unit;
        }
    } else {
        auto own = generic_templates_.find(u->name + "." + base);
        if (own != generic_templates_.end()) {
            tmpl_decl = own->second;
            tmpl_unit = u->name;
        } else {
            for (const auto& kv : generic_templates_) {
                std::string tu, tn;
                split_qualified(kv.first, tu, tn);
                if (tn != base) continue;
                if (tmpl_decl) {
                    err_ts("generic `" + base + "` is ambiguous; qualify it with a unit name", spec);
                }
                tmpl_decl = kv.second;
                tmpl_unit = tu;
            }
        }
    }
    if (!tmpl_decl) {
        err_ts("`" + base + "` is not a generic type or has no type parameters", spec);
    }

    // 规范化实例名与缓存键
    std::string display = canon_spec(*spec);
    std::string key = tmpl_unit + "." + display;
    auto cached = generic_insts_.find(key);
    if (cached != generic_insts_.end()) return cached->second;
    if (generic_busy_.count(key)) {
        err_ts("recursive generic instantiation of `" + display + "`", spec);
    }
    generic_busy_.insert(key);

    auto* sd_tmpl = dynamic_cast<StructDecl*>(tmpl_decl);
    auto* cd_tmpl = dynamic_cast<ClassDecl*>(tmpl_decl);
    auto* id_tmpl = dynamic_cast<InterfaceDecl*>(tmpl_decl);
    if (!sd_tmpl && !cd_tmpl && !id_tmpl) {
        err_ts("`" + base + "` is not a generic type", spec);
    }
    const std::vector<TypeParam>& tps =
        sd_tmpl ? sd_tmpl->type_params : (cd_tmpl ? cd_tmpl->type_params : id_tmpl->type_params);
    if (tps.size() != spec->args.size()) {
        err_ts("generic `" + base + "` expects " + std::to_string(tps.size()) +
                   " type argument(s) but got " + std::to_string(spec->args.size()),
               spec);
    }
    if (id_tmpl) {
        if (!id_tmpl->bases.empty()) {
            err_ts("generic interface inheritance is not supported yet", spec);
        }
    } else if (sd_tmpl) {
        if (!sd_tmpl->const_members.empty() || !sd_tmpl->bases.empty()) {
            err_ts("generic struct `" + base + "` does not support const members or interfaces yet", spec);
        }
    } else {
        if (!cd_tmpl->const_members.empty() || cd_tmpl->bases.size() > 1) {
            err_ts("generic class `" + base + "` does not support interfaces or const members yet", spec);
        }
    }

    Unit* tmpl_unit_ptr = nullptr;
    auto info_it = infos_.find(tmpl_unit);
    if (info_it != infos_.end()) tmpl_unit_ptr = info_it->second.unit;
    if (!tmpl_unit_ptr) tmpl_unit_ptr = u;

    // 约束检查（U34 M3b）
    {
        std::vector<TypePtr> arg_types;
        for (const auto& ta : spec->args) arg_types.push_back(resolve_type(ta.get(), u));
        for (size_t i = 0; i < tps.size(); ++i) {
            check_bound(arg_types[i], tps[i], u, nullptr);
        }
    }

    // 克隆 + 替换
    std::unique_ptr<Decl> clone = clone_decl(*tmpl_decl);
    TypeBindings bindings;
    for (size_t i = 0; i < tps.size(); ++i) {
        bindings[tps[i].name] = spec->args[i].get();
    }
    subst_decl(*clone, bindings);
    clone->type_params.clear();
    clone->is_template = false;

    auto* sd = dynamic_cast<StructDecl*>(clone.get());
    auto* cd = dynamic_cast<ClassDecl*>(clone.get());
    auto* id = dynamic_cast<InterfaceDecl*>(clone.get());
    if (sd) {
        sd->name = display;
    } else if (cd) {
        cd->name = display;
    } else {
        id->name = display;
    }

    // ---------------- 泛型接口：只用于约束（M5a），不产生值类型 ----------------
    if (id) {
        TypePtr ity = Type::make_interface(tmpl_unit, display);
        Unit* saved_cur = cur_;
        TypePtr saved_class = cur_class_;
        cur_ = tmpl_unit_ptr;
        cur_class_ = ity;  // 让 `Self` 解析为接口类型

        ity->methods.clear();
        for (auto& m : id->methods) {
            if (m->ret) m->rty = resolve_type(m->ret.get(), tmpl_unit_ptr);
            m->pty.clear();
            for (auto& p : m->params) {
                m->pty.push_back(resolve_type(p->spec.get(), tmpl_unit_ptr));
            }
            MethodSig sig;
            sig.name = m->name;
            sig.ret = m->rty;
            sig.params = m->pty;
            sig.is_abstract = !m->body;
            sig.is_override = m->is_override;
            sig.is_public = true;
            sig.owner = ity;
            sig.is_operator = m->is_operator;
            sig.ret_self = m->ret && m->ret->is_self;
            for (auto& p : m->params) sig.params_self.push_back(p->spec && p->spec->is_self);
            ity->methods.push_back(std::move(sig));
            m->cname = method_cname(tmpl_unit, display, m->name, m->pty);
        }
        cur_class_ = saved_class;
        cur_ = saved_cur;

        id->ty = ity;
        generic_insts_[key] = ity;
        infos_[tmpl_unit].interfaces[display] = id;
        generic_busy_.erase(key);

        // 默认方法体检查（与其他类型相同的上下文保存/恢复）
        {
            auto saved_scopes = std::move(scopes_);
            auto saved_autos = std::move(auto_groups_);
            auto saved_this = std::move(this_local_);
            FuncDecl* saved_func = cur_func_;
            TypePtr saved_ret = cur_ret_;
            std::string saved_name = cur_name_;
            TypePtr saved_cls = cur_class_;
            int saved_loop = loop_depth_;
            bool saved_in_ctor = in_ctor_;
            bool saved_in_dtor = in_dtor_;
            bool saved_super = super_ok_;
            Expr* saved_direct = direct_init_expr_;
            Unit* saved_unit = cur_;

            cur_ = tmpl_unit_ptr;
            check_interface(id);

            scopes_ = std::move(saved_scopes);
            auto_groups_ = std::move(saved_autos);
            this_local_ = std::move(saved_this);
            cur_func_ = saved_func;
            cur_ret_ = saved_ret;
            cur_name_ = std::move(saved_name);
            cur_class_ = saved_cls;
            loop_depth_ = saved_loop;
            in_ctor_ = saved_in_ctor;
            in_dtor_ = saved_in_dtor;
            super_ok_ = saved_super;
            direct_init_expr_ = saved_direct;
            cur_ = saved_unit;
        }

        generic_decls_.push_back(std::move(clone));
        return ity;
    }

    const bool is_struct = sd != nullptr;
    TypePtr ty = is_struct ? Type::make_struct(tmpl_unit, display)
                           : Type::make_class(tmpl_unit, display);
    if (is_struct) {
        ty->is_packed = sd->is_packed;
        ty->is_union = sd->is_union;
    }

    Unit* saved_cur = cur_;
    cur_ = tmpl_unit_ptr;

    // 基类（单个，泛型或非泛型）：先实例化，再把基类字段扁平化到前面
    if (!is_struct && !cd->base_specs.empty()) {
        TypePtr bt = resolve_type(cd->base_specs[0].get(), tmpl_unit_ptr);
        if (bt->kind != TypeKind::Class) {
            err_ts("base `" + cd->bases[0] + "` of `" + base + "` is not a class", spec);
        }
        ty->base = bt;
        for (const auto& bf : bt->fields) ty->fields.push_back(bf);
    }

    // 字段（struct 默认 public；class 按 field_visibility，默认 private）
    const auto& fields = is_struct ? sd->fields : cd->fields;
    const auto& field_vis = is_struct ? sd->field_visibility : cd->field_visibility;
    for (auto& f : fields) {
        TypePtr ft = resolve_type(f.second.get(), tmpl_unit_ptr);
        bool pub = is_struct;
        auto vit = field_vis.find(f.first);
        if (vit != field_vis.end()) pub = vit->second;
        Field fld;
        fld.name = f.first;
        fld.type = ft;
        fld.weak = false;
        fld.is_public = pub;
        ty->fields.push_back(std::move(fld));
    }

    // 方法 / 构造器 / 析构器签名
    const auto& methods = is_struct ? sd->methods : cd->methods;
    ty->methods.clear();
    for (auto& m : methods) {
        if (m->ret) m->rty = resolve_type(m->ret.get(), tmpl_unit_ptr);
        m->pty.clear();
        for (auto& p : m->params) {
            m->pty.push_back(resolve_type(p->spec.get(), tmpl_unit_ptr));
        }
        MethodSig sig;
        sig.name = m->name;
        sig.ret = m->rty;
        sig.params = m->pty;
        sig.is_abstract = !m->body;
        sig.is_override = m->is_override;
        sig.is_public = m->is_public;
        sig.owner = ty;
        ty->methods.push_back(sig);
        m->cname = method_cname(tmpl_unit, display, m->name, m->pty);
        if (!m->body) ty->is_abstract = true;
    }

    const auto& ctors = is_struct ? sd->ctors : cd->ctors;
    ty->ctors.clear();
    for (auto& ctor : ctors) {
        ctor->pty.clear();
        for (auto& p : ctor->params) {
            ctor->pty.push_back(resolve_type(p->spec.get(), tmpl_unit_ptr));
        }
        ctor->cname = ctor_cname(tmpl_unit, display, ctor->pty);
        ty->ctors.push_back(ctor->pty);
        ty->ctor_is_public.push_back(ctor->is_public);
    }

    DtorDecl* dtor = is_struct ? sd->dtor.get() : cd->dtor.get();
    ty->has_dtor = dtor != nullptr;
    if (dtor) dtor->cname = dtor_cname(tmpl_unit, display);
    cur_ = saved_cur;

    if (is_struct) {
        sd->ty = ty;
    } else {
        cd->ty = ty;
    }
    generic_insts_[key] = ty;
    if (is_struct) {
        infos_[tmpl_unit].structs[display] = sd;
    } else {
        infos_[tmpl_unit].classes[display] = cd;
    }
    generic_busy_.erase(key);  // 方法体里允许再次出现本实例（递归）

    // 检查方法体（与 check_struct / check_class 相同的上下文保存/恢复）
    {
        auto saved_scopes = std::move(scopes_);
        auto saved_autos = std::move(auto_groups_);
        auto saved_this = std::move(this_local_);
        FuncDecl* saved_func = cur_func_;
        TypePtr saved_ret = cur_ret_;
        std::string saved_name = cur_name_;
        TypePtr saved_class = cur_class_;
        int saved_loop = loop_depth_;
        bool saved_in_ctor = in_ctor_;
        bool saved_in_dtor = in_dtor_;
        bool saved_super = super_ok_;
        Expr* saved_direct = direct_init_expr_;
        Unit* saved_unit = cur_;

        cur_ = tmpl_unit_ptr;
        if (is_struct) {
            check_struct(sd);
        } else {
            check_class(cd);
        }

        scopes_ = std::move(saved_scopes);
        auto_groups_ = std::move(saved_autos);
        this_local_ = std::move(saved_this);
        cur_func_ = saved_func;
        cur_ret_ = saved_ret;
        cur_name_ = std::move(saved_name);
        cur_class_ = saved_class;
        loop_depth_ = saved_loop;
        in_ctor_ = saved_in_ctor;
        in_dtor_ = saved_in_dtor;
        super_ok_ = saved_super;
        direct_init_expr_ = saved_direct;
        cur_ = saved_unit;
    }

    // 声明本体在 check_struct 之后才 move，避免检查期间指针失效
    generic_decls_.push_back(std::move(clone));
    return ty;
}

FuncDecl* Sema::instantiate_func(Call* c, const std::string& base, Unit* u) {    std::string want_unit;
    std::string name;
    split_qualified(base, want_unit, name);

    Decl* tmpl_decl = nullptr;
    std::string tmpl_unit;
    if (!want_unit.empty()) {
        auto it = generic_templates_.find(want_unit + "." + name);
        if (it != generic_templates_.end()) {
            tmpl_decl = it->second;
            tmpl_unit = want_unit;
        }
    } else {
        auto own = generic_templates_.find(u->name + "." + name);
        if (own != generic_templates_.end()) {
            tmpl_decl = own->second;
            tmpl_unit = u->name;
        } else {
            for (const auto& kv : generic_templates_) {
                std::string tu, tn;
                split_qualified(kv.first, tu, tn);
                if (tn != name) continue;
                if (tmpl_decl) {
                    err("generic function `" + name + "` is ambiguous; qualify it with a unit name", c);
                }
                tmpl_decl = kv.second;
                tmpl_unit = tu;
            }
        }
    }
    if (!tmpl_decl) err("`" + name + "` is not a generic function", c);
    auto* tmpl = dynamic_cast<FuncDecl*>(tmpl_decl);
    if (!tmpl) err("`" + name + "` is not a generic function", c);
    if (tmpl->is_extern) err("`extern` function `" + name + "` cannot be generic", c);
    if (tmpl->type_params.size() != c->type_args.size()) {
        err("generic function `" + name + "` expects " + std::to_string(tmpl->type_params.size()) +
                " type argument(s) but got " + std::to_string(c->type_args.size()),
            c);
    }

    // 约束检查（U34 M3b）：结构化匹配，实参类型先解析
    {
        std::vector<TypePtr> arg_types;
        for (const auto& ta : c->type_args) arg_types.push_back(resolve_type(ta.get(), u));
        for (size_t i = 0; i < tmpl->type_params.size(); ++i) {
            check_bound(arg_types[i], tmpl->type_params[i], u, c);
        }
    }

    std::string display = name + "<";
    for (size_t i = 0; i < c->type_args.size(); ++i) {
        if (i) display += ", ";
        display += canon_spec(*c->type_args[i]);
    }
    display += ">";

    std::string key = tmpl_unit + "." + display;
    auto cached = generic_func_insts_.find(key);
    if (cached != generic_func_insts_.end()) return cached->second;
    if (generic_busy_.count(key)) {
        err("recursive generic instantiation of `" + display + "`", c);
    }
    generic_busy_.insert(key);

    std::unique_ptr<Decl> clone = clone_decl(*tmpl);
    auto* fn = static_cast<FuncDecl*>(clone.get());
    TypeBindings bindings;
    for (size_t i = 0; i < tmpl->type_params.size(); ++i) {
        bindings[tmpl->type_params[i].name] = c->type_args[i].get();
    }
    subst_decl(*fn, bindings);
    fn->type_params.clear();
    fn->is_template = false;
    fn->name = display;

    // 先登记再检查：函数体里的递归调用（`f<T>`）要命中缓存
    generic_func_insts_[key] = fn;
    generic_decls_.push_back(std::move(clone));

    // 保存当前检查上下文：check_func 会重置 scopes_ / cur_func_ 等，
    // 而实例化可能发生在另一个函数体内部（嵌套调用）。
    auto saved_scopes = std::move(scopes_);
    auto saved_autos = std::move(auto_groups_);
    auto saved_this = std::move(this_local_);
    FuncDecl* saved_func = cur_func_;
    TypePtr saved_ret = cur_ret_;
    std::string saved_name = cur_name_;
    TypePtr saved_class = cur_class_;
    int saved_loop = loop_depth_;
    bool saved_in_ctor = in_ctor_;
    bool saved_in_dtor = in_dtor_;
    bool saved_super = super_ok_;
    Expr* saved_direct = direct_init_expr_;
    Unit* saved_cur = cur_;

    Unit* tu = u;
    auto info_it = infos_.find(tmpl_unit);
    if (info_it != infos_.end()) tu = info_it->second.unit;
    cur_ = tu;

    // 实例化函数绕过 resolve_unit_types，签名要在这里解析
    fn->rty = resolve_type(fn->ret.get(), tu);
    fn->pty.clear();
    fn->pty_ref.clear();
    fn->pty_cref.clear();
    for (auto& p : fn->params) {
        fn->pty.push_back(resolve_type(p->spec.get(), tu));
        fn->pty_ref.push_back(p->by_ref);
        fn->pty_cref.push_back(p->by_ref && p->is_const);
    }

    check_func(fn);
    if (fn->cname.empty()) fn->cname = func_cname(tmpl_unit, fn);

    // 恢复上下文
    scopes_ = std::move(saved_scopes);
    auto_groups_ = std::move(saved_autos);
    this_local_ = std::move(saved_this);
    cur_func_ = saved_func;
    cur_ret_ = saved_ret;
    cur_name_ = std::move(saved_name);
    cur_class_ = saved_class;
    loop_depth_ = saved_loop;
    in_ctor_ = saved_in_ctor;
    in_dtor_ = saved_in_dtor;
    super_ok_ = saved_super;
    direct_init_expr_ = saved_direct;
    cur_ = saved_cur;

    generic_busy_.erase(key);
    return fn;
}

namespace {

bool is_cmp_op(const std::string& n) {
    return n == "operator<" || n == "operator<=" || n == "operator>" ||
           n == "operator>=" || n == "operator==" || n == "operator!=";
}

bool is_arith_op(const std::string& n) {
    return n == "operator+" || n == "operator-" || n == "operator*" ||
           n == "operator/" || n == "operator%";
}

// 在 `t`（含基类链与接口默认方法）上找与 `sig` 同名同签名的方法；
// `sig` 里来自 `Self` 的位置用 `self_ty` 替换。
bool type_has_method(const TypePtr& t, const MethodSig& sig, const TypePtr& self_ty, int depth = 0) {
    if (!t || depth > 64) return false;
    for (const auto& m : t->methods) {
        if (m.name != sig.name) continue;
        if (m.params.size() != sig.params.size()) continue;
        bool ok = true;
        for (size_t i = 0; i < sig.params.size(); ++i) {
            const TypePtr& want = (i < sig.params_self.size() && sig.params_self[i]) ? self_ty : sig.params[i];
            if (!same_type(m.params[i], want)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        const TypePtr& want_ret = sig.ret_self ? self_ty : sig.ret;
        if (same_type(m.ret, want_ret)) return true;
    }
    if (t->kind == TypeKind::Class && t->base && type_has_method(t->base, sig, self_ty, depth + 1)) return true;
    for (const auto& i : t->interfaces) {
        if (type_has_method(i, sig, self_ty, depth + 1)) return true;
    }
    return false;
}

}  // namespace

bool Sema::satisfies_one(const TypePtr& t, const TypeSpec* bound, Unit* u, std::string* why) {
    Unit* saved = cur_;
    cur_ = u;
    TypePtr bt = resolve_type(bound, u);
    cur_ = saved;
    if (!bt) return false;

    // 具体类型：精确相等
    if (bt->kind != TypeKind::Interface) {
        if (same_type(t, bt)) return true;
        if (why) *why = "expected `" + bt->str() + "`";
        return false;
    }

    // 接口：收集成员（含接口继承链）
    std::vector<const MethodSig*> sigs;
    std::function<void(const TypePtr&, int)> collect = [&](const TypePtr& itf, int depth) {
        if (!itf || depth > 64) return;
        for (const auto& m : itf->methods) sigs.push_back(&m);
        for (const auto& p : itf->interfaces) collect(p, depth + 1);
    };
    collect(bt, 0);

    for (const MethodSig* sig : sigs) {
        if (!sig->is_operator) {
            if (!type_has_method(t, *sig, t)) {
                if (why) *why = "missing method `" + sig->name + "`";
                return false;
            }
            continue;
        }
        // 运算符成员：替换 Self 后，操作数必须全是 `t`
        std::vector<TypePtr> want;
        for (size_t i = 0; i < sig->params.size(); ++i) {
            want.push_back((i < sig->params_self.size() && sig->params_self[i]) ? t : sig->params[i]);
        }
        const TypePtr want_ret = sig->ret_self ? t : sig->ret;
        bool all_t = !want.empty();
        for (const auto& p : want) {
            if (!same_type(p, t)) all_t = false;
        }
        if (!all_t) {
            if (why) *why = "operator `" + sig->name + "` must take `Self` operands";
            return false;
        }

        // 内置运算符（注：enum 不参与算术）
        bool builtin = false;
        if (is_arith_op(sig->name) && t->is_numeric() && t->kind != TypeKind::Enum &&
            same_type(want_ret, t)) {
            builtin = true;
        }
        if (is_cmp_op(sig->name) &&
            (t->is_numeric() || t->kind == TypeKind::Ptr || t->kind == TypeKind::String) && want_ret &&
            want_ret->is_bool()) {
            builtin = true;
        }
        if (builtin) continue;

        // 用户定义运算符：在可见 unit 里找同签名自由函数（精确匹配，不做隐式转换）
        bool found = false;
        auto try_unit = [&](UnitInfo& oi, bool need_pub) {
            auto it = oi.funcs.find(sig->name);
            if (it == oi.funcs.end()) return;
            for (FuncDecl* f : it->second) {
                if (need_pub && !f->is_public) continue;
                if (f->pty.size() != want.size()) continue;
                if (!same_type(f->rty, want_ret)) continue;
                bool ok = true;
                for (size_t i = 0; i < want.size(); ++i) {
                    if (!same_type(f->pty[i], want[i])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) found = true;
            }
        };
        auto own = infos_.find(u->name);
        if (own != infos_.end()) try_unit(own->second, false);
        if (!found) {
            for (auto& use : u->uses) {
                if (!unit_visible(use->unit_name)) continue;
                auto it = infos_.find(use->unit_name);
                if (it != infos_.end()) try_unit(it->second, true);
            }
        }
        if (!found && std_info()) try_unit(*std_info(), true);
        if (!found) {
            if (why) {
                *why = "no `" + sig->name + "` accepting `(" + t->str() + ", " + t->str() + ")`";
            }
            return false;
        }
    }
    return true;
}

void Sema::check_bound(const TypePtr& t, const TypeParam& p, Unit* u, const Node* at) {
    if (p.bounds.empty()) return;
    if (p.op != TypeParam::Op::Or) {
        for (const auto& b : p.bounds) {
            std::string why;
            if (!satisfies_one(t, b.get(), u, &why)) {
                const std::string msg = "type `" + t->str() + "` does not satisfy `" + canon_spec(*b) +
                                        "` for type parameter `" + p.name + "`: " + why;
                if (at) {
                    err(msg, at);
                } else {
                    err(msg);
                }
            }
        }
        return;
    }
    bool any = false;
    for (const auto& b : p.bounds) {
        if (satisfies_one(t, b.get(), u, nullptr)) {
            any = true;
            break;
        }
    }
    if (!any) {
        const std::string msg =
            "type `" + t->str() + "` does not satisfy any alternative bound of type parameter `" + p.name + "`";
        if (at) {
            err(msg, at);
        } else {
            err(msg);
        }
    }
}

}  // namespace muc_detail
