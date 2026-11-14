// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds fields/utilities/visibility/collection/signature checks/layout modifiers (definitions of Sema members).
#include "sema_impl.hpp"

namespace muc_detail {

    [[noreturn]] void Sema::err(const std::string& msg, const Node* node) const {
        const int line = node ? node->line : 0;
        const int col = node ? node->col : 0;
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string());
    }

    void Sema::warn(const std::string& msg, const Node* node) const {
        const int line = node ? node->line : 0;
        const int col = node ? node->col : 0;
        std::cerr << (cur_ ? cur_->filename : std::string()) << ":" << line << ":" << col
                  << ": warning: " << msg << "\n";
    }

    [[noreturn]] void Sema::err_ts(const std::string& msg, const TypeSpec* spec) const {
        const int line = spec ? spec->line : 0;
        const int col = spec ? spec->col : 0;
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string());
    }

    UnitInfo& Sema::info() { return infos_.at(cur_->name); }

    UnitInfo* Sema::std_info() {
        auto it = infos_.find("std.string");
        return it == infos_.end() ? nullptr : &it->second;
    }

    UnitInfo* Sema::exception_info() {
        auto it = infos_.find("std.exception");
        return it == infos_.end() ? nullptr : &it->second;
    }

    TypePtr Sema::exception_type() {
        if (UnitInfo* ex = exception_info()) {
            auto it = ex->classes.find("Exception");
            if (it != ex->classes.end()) return it->second->ty;
        }
        return nullptr;
    }

    bool Sema::implicit_ok(const TypePtr& src, const TypePtr& dst) {
        return implicitly_convertible(src, dst);
    }

    bool Sema::uses_unit(const std::string& uname) const {
        if (!cur_) return false;
        for (const auto& use : cur_->uses) {
            if (use->unit_name == uname) return true;
        }
        return false;
    }

    bool Sema::unit_imported(const std::string& uname) const {
        return uname == "std.string" || uname == "std.exception" || uses_unit(uname);
    }

    std::string Sema::lib_of(const std::string& unit_name) {
        const auto dot = unit_name.find('.');
        return dot == std::string::npos ? std::string() : unit_name.substr(0, dot);
    }

    bool Sema::unit_visible(const std::string& target) const {
        auto it = infos_.find(target);
        if (it == infos_.end()) return false;
        if (cur_ && lib_of(target) == lib_of(cur_->name)) return true;
        return it->second.unit && it->second.unit->is_public;
    }

    bool Sema::decl_visible(const std::string& owner_unit, bool is_public) const {
        if (!cur_ || owner_unit == cur_->name) return true;
        if (!extra_visible_unit_.empty() && owner_unit == extra_visible_unit_) return true;
        if (!unit_visible(owner_unit)) return false;
        return is_public;
    }

    bool Sema::type_is_public(const TypePtr& t) const {
        if (!t) return true;
        auto it = infos_.find(t->unit);
        if (it == infos_.end()) return true;  // built-in type
        const UnitInfo& ui = it->second;
        switch (t->kind) {
            case TypeKind::Struct: {
                auto p = ui.structs.find(t->name);
                return p != ui.structs.end() && p->second->is_public;
            }
            case TypeKind::Enum: {
                auto p = ui.enums.find(t->name);
                return p != ui.enums.end() && p->second->is_public;
            }
            case TypeKind::Class: {
                auto p = ui.classes.find(t->name);
                return p != ui.classes.end() && p->second->is_public;
            }
            case TypeKind::Interface: {
                auto p = ui.interfaces.find(t->name);
                return p != ui.interfaces.end() && p->second->is_public;
            }
            default: return true;
        }
    }

    void Sema::require_type_visible(const TypePtr& t, const TypeSpec* spec) const {
        if (!t) return;
        if (cur_ && t->unit == cur_->name) return;
        if (!unit_visible(t->unit)) {
            err_ts("unit `" + t->unit + "` is only visible within its library; cross-library use requires `public unit`", spec);
        }
        if (!type_is_public(t)) {
            err_ts("type `" + t->unit + "." + t->name + "` is not declared public", spec);
        }
    }

    void Sema::collect() {
        // First stage: generic templates are only kept and do not take part in regular checks (moved to Unit::templates).
        // This makes "declarations accepted, use reports not implemented" a stable state and does not affect non-generic code.
        for (auto& u : units_) {
            std::vector<std::unique_ptr<Decl>> keep;
            std::vector<std::unique_ptr<Decl>> tmpl;
            keep.reserve(u->decls.size());
            for (auto& d : u->decls) {
                if (!d->type_params.empty()) {
                    d->is_template = true;
                    tmpl.push_back(std::move(d));
                } else {
                    keep.push_back(std::move(d));
                }
            }
            u->decls = std::move(keep);
            u->templates = std::move(tmpl);
        }
        index_templates();
        std::unordered_set<std::string> seen_units;
        for (auto& u : units_) {
            if (!seen_units.insert(u->name).second) {
                cur_ = u.get();
                err("duplicate unit `" + u->name + "`");
            }
            UnitInfo ui;
            ui.unit = u.get();
            infos_.emplace(u->name, std::move(ui));
        }

        for (auto& u : units_) {
            cur_ = u.get();
            UnitInfo& ui = infos_.at(u->name);
            for (auto& d : u->decls) {
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    ui.funcs[f->name].push_back(f);
                    f->sym = f;
                } else if (auto s = dynamic_cast<StructDecl*>(d.get())) {
                    ui.structs[s->name] = s;
                } else if (auto e = dynamic_cast<EnumDecl*>(d.get())) {
                    ui.enums[e->name] = e;
                } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                    if (ui.classes.count(c->name)) err("duplicate definition of class `" + c->name + "`", c);
                    ui.classes[c->name] = c;
                } else if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) {
                    if (ui.interfaces.count(i->name)) err("duplicate definition of interface `" + i->name + "`", i);
                    ui.interfaces[i->name] = i;
                } else if (auto a = dynamic_cast<TypeAliasDecl*>(d.get())) {
                    if (ui.aliases.count(a->name)) err("duplicate definition of type alias `" + a->name + "`", a);
                    ui.aliases[a->name] = a;
                } else if (auto g = dynamic_cast<GlobalVar*>(d.get())) {
                    if (ui.globals.count(g->name)) err("duplicate definition of variable `" + g->name + "`", g);
                    ui.globals[g->name] = g;
                    g->cname = g->is_extern ? g->name : mangle(u->name, g->name);
                }
            }

            // Names in the same scope must not clash across categories
            std::unordered_map<std::string, Node*> names;
            for (auto& d : u->decls) {
                Node* node = d.get();
                std::string nm;
                if (auto f = dynamic_cast<FuncDecl*>(node)) {
                    nm = f->name;
                } else if (auto s = dynamic_cast<StructDecl*>(node)) {
                    nm = s->name;
                } else if (auto e = dynamic_cast<EnumDecl*>(node)) {
                    nm = e->name;
                } else if (auto c = dynamic_cast<ClassDecl*>(node)) {
                    nm = c->name;
                } else if (auto i = dynamic_cast<InterfaceDecl*>(node)) {
                    nm = i->name;
                } else if (auto a = dynamic_cast<TypeAliasDecl*>(node)) {
                    nm = a->name;
                } else if (auto g = dynamic_cast<GlobalVar*>(node)) {
                    nm = g->name;
                } else {
                    continue;
                }
                if (dynamic_cast<FuncDecl*>(node)) {
                    auto it = names.find(nm);
                    if (it != names.end() && !dynamic_cast<FuncDecl*>(it->second)) {
                        err("`" + nm + "` conflicts with another declaration", node);
                    }
                    if (it == names.end()) names.emplace(nm, node);
                    continue;
                }
                if (names.count(nm)) err("`" + nm + "` is declared more than once in the same unit", node);
                names.emplace(nm, node);
            }
        }

        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& use : u->uses) {
                if (!infos_.count(use->unit_name)) {
                    err("cannot find unit `" + use->unit_name + "`", use.get());
                }
                if (!unit_visible(use->unit_name)) {
                    err("unit `" + use->unit_name +
                            "` is only visible within its library; cross-library use requires `public unit`",
                        use.get());
                }
            }
        }
    }

    void Sema::check_exposed_type(const TypePtr& t, const std::string& what, const Node* at) {
        if (!t) return;
        TypePtr b = t;
        while (b && (b->kind == TypeKind::Ptr || b->kind == TypeKind::Array ||
                     b->kind == TypeKind::Slice)) {
            b = b->elem;
        }
        if (!b) return;
        if (b->kind != TypeKind::Struct && b->kind != TypeKind::Class &&
            b->kind != TypeKind::Interface && b->kind != TypeKind::Enum) {
            return;
        }
        if (cur_ && b->unit == cur_->name) {
            if (!type_is_public(b)) {
                err(what + " cannot use non-public type `" + b->str() + "`", at);
            }
            return;
        }
        if (!unit_visible(b->unit)) {
            err(what + " cannot use type `" + b->str() + "` from non-visible unit `" + b->unit + "`", at);
        }
        if (!type_is_public(b)) {
            err(what + " cannot use non-public type `" + b->str() + "`", at);
        }
    }

    void Sema::check_public_signatures() {
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                if (!d->is_public) continue;
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    if (f->is_extern) continue;
                    const std::string what = "public function `" + f->name + "`";
                    check_exposed_type(f->rty, what, f);
                    for (auto& p : f->pty) check_exposed_type(p, what, f);
                } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                    const std::string what = "public class `" + c->name + "`";
                    if (c->ty->base) check_exposed_type(c->ty->base, "base class of " + what, c);
                    for (auto& i : c->ty->interfaces) {
                        check_exposed_type(i, "interface of " + what, c);
                    }
                    for (auto& fd : c->ty->fields) {
                        if (fd.is_public) {
                            check_exposed_type(fd.type, "public field `" + fd.name + "` of " + what, c);
                        }
                    }
                    for (auto& m : c->ty->methods) {
                        if (!m.is_public) continue;
                        const std::string mw = "public method `" + m.name + "` of " + what;
                        check_exposed_type(m.ret, mw, c);
                        for (auto& p : m.params) check_exposed_type(p, mw, c);
                    }
                } else if (auto sd = dynamic_cast<StructDecl*>(d.get())) {
                    const std::string what = "public struct `" + sd->name + "`";
                    for (auto& fd : sd->ty->fields) {
                        check_exposed_type(fd.type, "field `" + fd.name + "` of " + what, sd);
                    }
                    for (auto& m : sd->ty->methods) {
                        if (!m.is_public) continue;
                        const std::string mw = "public method `" + m.name + "` of " + what;
                        check_exposed_type(m.ret, mw, sd);
                        for (auto& p : m.params) check_exposed_type(p, mw, sd);
                    }
                } else if (auto itf = dynamic_cast<InterfaceDecl*>(d.get())) {
                    const std::string what = "public interface `" + itf->name + "`";
                    for (auto& m : itf->ty->methods) {
                        const std::string mw = "method `" + m.name + "` of " + what;
                        check_exposed_type(m.ret, mw, itf);
                        for (auto& p : m.params) check_exposed_type(p, mw, itf);
                    }
                }
            }
        }
    }

    void Sema::place_structs() {
        for (auto& u : units_) {
            for (auto& d : u->decls) {
                if (auto s = dynamic_cast<StructDecl*>(d.get())) {
                    s->ty = Type::make_struct(u->name, s->name);
                } else if (auto e = dynamic_cast<EnumDecl*>(d.get())) {
                    // Unlike the Python version (see the report): the enum type is created here, so that a cross-unit
                    // reference to "an enum of a unit not yet resolved" never yields a null type.
                    e->ty = Type::make_enum(u->name, e->name);
                } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                    c->ty = Type::make_class(u->name, c->name);
                } else if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) {
                    i->ty = Type::make_interface(u->name, i->name);
                }
            }
        }
    }

    void Sema::resolve_class_fields(ClassDecl* c, Unit* u) {
        std::vector<Field> fields;
        if (c->ty->base) fields = c->ty->base->fields;
        std::unordered_set<std::string> fnames;
        for (auto& f : fields) fnames.insert(f.name);
        for (auto& [fname, spec] : c->fields) {
            if (!fnames.insert(fname).second) {
                err("duplicate field `" + fname + "` in class `" + c->name + "`", c);
            }
            TypePtr ft = resolve_type(spec.get(), u);
            if (is_void(ft)) err("class field cannot be void", c);
            if (spec->weak && !is_class_type(ft)) {
                err("`weak` field must be a class reference (interfaces do not support `weak` yet)", c);
            }
            if (noncopyable_struct(ft)) {
                err("struct `" + ft->str() +
                        "` has a destructor and cannot be used as a field (field destruction is not implemented)",
                    c);
            }
            bool field_pub = false;  // class defaults to private
            if (auto vit = c->field_visibility.find(fname); vit != c->field_visibility.end()) {
                field_pub = vit->second;
            }
            fields.push_back(Field{fname, ft, spec->weak, field_pub});
        }
        c->ty->fields = std::move(fields);
    }

    bool Sema::eval_const_int(Expr* e, long long& out) {
        if (!e) return false;
        if (auto il = dynamic_cast<IntLit*>(e)) {
            out = il->value;
            return true;
        }
        if (auto cl = dynamic_cast<CharLit*>(e)) {
            out = cl->value;
            return true;
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            long long v = 0;
            if (!eval_const_int(u->operand.get(), v)) return false;
            if (u->op == "-") {
                out = -v;
                return true;
            }
            if (u->op == "+") {
                out = v;
                return true;
            }
            return false;
        }
        if (auto c = dynamic_cast<Cast*>(e)) return eval_const_int(c->expr.get(), out);
        if (auto b = dynamic_cast<Binary*>(e)) {
            long long l = 0, r = 0;
            if (!eval_const_int(b->left.get(), l) || !eval_const_int(b->right.get(), r)) {
                return false;
            }
            const std::string& op = b->op;
            if (op == "+") out = l + r;
            else if (op == "-") out = l - r;
            else if (op == "*") out = l * r;
            else if (op == "/") {
                if (r == 0) return false;
                out = l / r;
            } else if (op == "%") {
                if (r == 0) return false;
                out = l % r;
            } else if (op == "<<") {
                if (r < 0 || r > 62) return false;
                out = l << r;
            } else if (op == ">>") {
                if (r < 0 || r > 63) return false;
                out = l >> r;
            } else if (op == "&") out = l & r;
            else if (op == "|") out = l | r;
            else if (op == "^") out = l ^ r;
            else return false;
            return true;
        }
        return false;
    }

    bool Sema::is_pow2(long long n) { return n > 0 && (n & (n - 1)) == 0; }

    void Sema::check_layouts() {
        // First pass: evaluate align(N) and check for a positive power of two
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto sd = dynamic_cast<StructDecl*>(d.get());
                if (!sd || !sd->ty) continue;
                sd->ty->is_packed = sd->is_packed;
                if (!sd->align_expr) continue;
                long long n = 0;
                if (!eval_const_int(sd->align_expr.get(), n)) {
                    err("N in `align(N)` must be a compile-time integer constant", sd);
                }
                if (!is_pow2(n) || n > 65536) {
                    err("N in `align(N)` must be a power of two (got " + std::to_string(n) + ")", sd);
                }
                sd->ty->align_req = static_cast<int>(n);
            }
        }
        // Second pass: compare N with the natural alignment + packed field type restrictions (fields of all units are resolved by now)
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto sd = dynamic_cast<StructDecl*>(d.get());
                if (!sd || !sd->ty) continue;
                const TypePtr& ty = sd->ty;
                const std::string kw = sd->is_union ? "union" : "struct";
                if (ty->align_req > 0 && !ty->is_packed) {
                    size_t natural = 1;
                    for (auto& f : ty->fields) natural = std::max(natural, type_align(f.type));
                    if (static_cast<size_t>(ty->align_req) < natural) {
                        err("align(" + std::to_string(ty->align_req) + ") is less than the natural alignment of " + kw + " `" +
                                sd->name + "` (" + std::to_string(natural) + ")" +
                                "; use `packed` to lower the alignment",
                            sd);
                    }
                }
                if (!ty->is_packed) continue;
                for (auto& f : ty->fields) {
                    // arrays are checked by their element type
                    TypePtr base = f.type;
                    while (base && base->kind == TypeKind::Array) base = base->elem;
                    if (!base) continue;
                    if (base->kind == TypeKind::Slice) {
                        err("field `" + f.name + "` of packed " + kw + " `" + sd->name +
                                "` cannot be a dynamic array `" + base->str() + "` (unaligned pointer access)",
                            sd);
                    }
                    if (is_reference_type(base)) {
                        err("field `" + f.name + "` of packed " + kw + " `" + sd->name +
                                "` cannot be a `class` or `interface` reference (unaligned pointer access)",
                            sd);
                    }
                    if (base->kind == TypeKind::Struct && base->needs_cleanup()) {
                        err("field `" + f.name + "` of packed " + kw + " `" + sd->name + "` cannot be `" +
                                base->str() +
                                "`: it holds reference-counted fields, which need aligned access",
                            sd);
                    }
                }
            }
        }
    }

    void Sema::resolve_const_members(std::vector<std::unique_ptr<ConstMember>>& list,
                               const TypePtr& owner, Unit* u, const std::string& kw,
                               const std::string& type_name) {
        for (auto& cm : list) {
            cm->owner = owner;
            cm->rty = resolve_type(cm->spec.get(), u);
            if (is_void(cm->rty)) {
                err("const member `" + cm->name + "` in " + kw + " `" + type_name +
                        "` cannot be void",
                    cm.get());
            }
            if (!(is_prim(cm->rty) || is_enum_t(cm->rty))) {
                err("const member `" + cm->name +
                        "` must have a scalar type (bool, integer, float, double, or enum), found `" +
                        cm->rty->str() + "`",
                    cm.get());
            }
        }
    }

    void Sema::resolve_unit_types(Unit* u) {
        cur_ = u;

        // Structs: the placeholder type is ready, fields are filled in here (fields may reference other structs/enums)
        for (auto& d : u->decls) {
            auto sd = dynamic_cast<StructDecl*>(d.get());
            if (!sd) continue;
            if (!sd->ty) sd->ty = Type::make_struct(u->name, sd->name);
            sd->ty->is_packed = sd->is_packed;  // align_req is evaluated in check_layouts
            sd->ty->is_union = sd->is_union;
            const std::string kw = sd->is_union ? "union" : "struct";
            std::vector<Field> fields;
            std::unordered_set<std::string> fnames;
            for (auto& [fname, spec] : sd->fields) {
                if (!fnames.insert(fname).second) {
                    err("duplicate field `" + fname + "` in " + kw + " `" + sd->name + "`", sd);
                }
                TypePtr ft = resolve_type(spec.get(), u);
                if (is_void(ft)) err("field of " + kw + " `" + sd->name + "` cannot be void", sd);
                if (same_type(ft, sd->ty)) {
                    err(kw + " `" + sd->name + "` cannot contain itself by value", sd);
                }
                if (spec->weak) err("`weak` can only be used on class fields", sd);
                if (sd->is_union && ft->needs_cleanup()) {
                    err("field `" + fname + "` of union `" + sd->name + "` cannot be `" + ft->str() +
                            "`: union members cannot hold values that need reference counting",
                        sd);
                }
                if (noncopyable_struct(ft)) {
                    err("struct `" + ft->str() +
                            "` has a destructor and cannot be used as a field (field destruction is not implemented)",
                        sd);
                }
                bool field_pub = true;  // struct defaults to public
                if (auto vit = sd->field_visibility.find(fname);
                    vit != sd->field_visibility.end()) {
                    field_pub = vit->second;
                }
                fields.push_back(Field{fname, ft, false, field_pub});
            }
            sd->ty->fields = std::move(fields);
            if (sd->is_union && sd->ty->fields.empty()) {
                err("union `" + sd->name + "` must have at least one member", sd);
            }
            resolve_const_members(sd->const_members, sd->ty, u, kw, sd->name);

            sd->ty->methods.clear();
            for (auto& m : sd->methods) {
                resolve_method_ret(m.get(), u);
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = false;
                sig.is_override = m->is_override;
                sig.is_public = m->is_public;
                sig.owner = sd->ty;
                sig.is_operator = m->is_operator;
                if (m->is_operator) check_member_operator_sig(m.get(), sd->ty);
                sd->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, sd->name, m->name, m->pty);
            }
            sd->ty->ctors.clear();
            for (auto& ctor : sd->ctors) {
                ctor->pty.clear();
                for (auto& p : ctor->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    ctor->pty.push_back(pt);
                }
                if (ctor->is_implicit) {
                    if (ctor->pty.size() != 1) {
                        err("`implicit` constructor of struct `" + sd->name +
                                "` must take exactly one parameter (the source value)",
                            ctor.get());
                    }
                    if (!ctor->is_public) {
                        err("`implicit` constructor of struct `" + sd->name + "` must be public", ctor.get());
                    }
                    if (ctor->pty.size() == 1 && same_type(ctor->pty[0], sd->ty)) {
                        err("an `implicit` constructor cannot take the struct itself (it would be a copy)",
                            ctor.get());
                    }
                }
                ctor->cname = ctor_cname(u->name, sd->name, ctor->pty);
                sd->ty->ctors.push_back(ctor->pty);
                sd->ty->ctor_is_public.push_back(ctor->is_public);
                sd->ty->ctor_is_implicit.push_back(ctor->is_implicit);
            }
            sd->ty->has_dtor = sd->dtor != nullptr;
            if (sd->dtor) sd->dtor->cname = dtor_cname(u->name, sd->name);
        }

        // class: base class, interfaces, flattened fields, method/constructor signatures
        for (auto& d : u->decls) {
            auto c = dynamic_cast<ClassDecl*>(d.get());
            if (!c) continue;
            if (!c->ty) c->ty = Type::make_class(u->name, c->name);

            bool has_base = false;
            TypeSpec where;
            where.line = c->line;
            where.col = c->col;
            for (size_t i = 0; i < c->bases.size(); ++i) {
                // Structured spec first: an instantiated generic interface (`class C : IIterator<int>`)
                // has no name to look up — it is created by the instantiator (U34 M5b).
                TypePtr bt;
                if (i < c->base_specs.size() && c->base_specs[i]) {
                    bt = resolve_type(c->base_specs[i].get(), u);
                } else {
                    bt = resolve_named_type(c->bases[i], u, &where);
                }
                if (bt->kind == TypeKind::Class) {
                    if (i != 0 || has_base) err("a class may have at most one base class, and it must come first", c);
                    has_base = true;
                    c->ty->base = bt;
                } else if (bt->kind == TypeKind::Interface) {
                    for (auto& prev : c->ty->interfaces) {
                        if (same_type(prev, bt)) {
                            err("class `" + c->name + "` implements interface `" + bt->str() + "` more than once", c);
                        }
                    }
                    c->ty->interfaces.push_back(bt);
                } else {
                    err("`" + c->bases[i] + "` is not a class or interface", c);
                }
            }

            // fields are handled by the topological pass below (base class fields must be ready first)

            resolve_const_members(c->const_members, c->ty, u, "class", c->name);

            c->ty->methods.clear();
            c->ty->is_abstract = false;
            for (auto& m : c->methods) {
                resolve_method_ret(m.get(), u);
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = !m->body;
                sig.is_override = m->is_override;
                sig.is_public = m->is_public;
                sig.owner = c->ty;
                c->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, c->name, m->name, m->pty);
                if (!m->body) c->ty->is_abstract = true;
            }
            for (auto& ctor : c->ctors) {
                ctor->pty.clear();
                for (auto& p : ctor->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    ctor->pty.push_back(pt);
                }
                ctor->cname = ctor_cname(u->name, c->name, ctor->pty);
                c->ty->ctors.push_back(ctor->pty);
                c->ty->ctor_is_public.push_back(ctor->is_public);
            }
            if (c->dtor) c->dtor->cname = dtor_cname(u->name, c->name);
        }

        // class fields: resolved in base-class dependency order (a base class may be declared later in the same unit)
        {
            std::unordered_map<std::string, ClassDecl*> by_name;
            for (auto& d : u->decls) {
                if (auto c = dynamic_cast<ClassDecl*>(d.get())) by_name[c->name] = c;
            }
            std::unordered_map<ClassDecl*, int> state;
            std::function<void(ClassDecl*)> resolve_fields = [&](ClassDecl* c) {
                if (!c || state[c] != 0) return;
                state[c] = 1;
                for (auto& bn : c->bases) {
                    if (bn.find('.') != std::string::npos) continue;
                    auto it = by_name.find(bn);
                    if (it != by_name.end() && it->second != c) resolve_fields(it->second);
                }
                resolve_class_fields(c, u);
                state[c] = 2;
            };
            for (auto& d : u->decls) {
                if (auto c = dynamic_cast<ClassDecl*>(d.get())) resolve_fields(c);
            }
        }

        // interface: base interfaces and method signatures
        for (auto& d : u->decls) {
            auto itf = dynamic_cast<InterfaceDecl*>(d.get());
            if (!itf) continue;
            if (!itf->ty) itf->ty = Type::make_interface(u->name, itf->name);
            TypeSpec where;
            where.line = itf->line;
            where.col = itf->col;
            for (size_t i = 0; i < itf->bases.size(); ++i) {
                // Structured spec when present: `interface IInt : Base<int>` names an instantiated
                // generic interface, which has no lookup name.
                TypePtr bt = (i < itf->base_specs.size() && itf->base_specs[i])
                                 ? resolve_type(itf->base_specs[i].get(), u)
                                 : resolve_named_type(itf->bases[i], u, &where);
                if (bt->kind != TypeKind::Interface) {
                    err("an interface can only inherit from an interface", itf);
                }
                for (auto& prev : itf->ty->interfaces) {
                    if (same_type(prev, bt)) {
                        err("interface `" + itf->name + "` inherits `" + bt->str() + "` more than once", itf);
                    }
                }
                itf->ty->interfaces.push_back(bt);
            }
            itf->ty->methods.clear();
            TypePtr saved_class = cur_class_;
            cur_class_ = itf->ty;  // make `Self` in signatures resolve to the interface type
            for (auto& m : itf->methods) {
                resolve_method_ret(m.get(), u);
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = !m->body;
                sig.is_override = false;
                sig.owner = itf->ty;
                sig.is_operator = m->is_operator;
                sig.ret_self = m->ret && m->ret->is_self;
                for (auto& p : m->params) sig.params_self.push_back(p->spec && p->spec->is_self);
                itf->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, itf->name, m->name, m->pty);
            }
            cur_class_ = saved_class;
        }

        // enums: check for duplicate members (the type is already created in place_structs)
        for (auto& d : u->decls) {
            auto ed = dynamic_cast<EnumDecl*>(d.get());
            if (!ed) continue;
            std::unordered_set<std::string> names;
            for (auto& [m, v] : ed->members) {
                if (!names.insert(m).second) {
                    err("duplicate member `" + m + "` in enum `" + ed->name + "`", ed);
                }
            }
        }

        for (auto& d : u->decls) {
            auto g = dynamic_cast<GlobalVar*>(d.get());
            if (!g) continue;
            g->rty = resolve_type(g->spec.get(), u);
            if (is_void(g->rty)) err("variable `" + g->name + "` cannot be void", g);
        }

        for (auto& d : u->decls) {
            auto f = dynamic_cast<FuncDecl*>(d.get());
            if (!f) continue;
            f->rty = make_borrow_ret(f->ret.get(), f->ret_ref, f, u, "function `" + f->name + "`");
            if (noncopyable_struct(referent_of(f->rty))) {
                err("struct `" + referent_of(f->rty)->str() + "` has a destructor and cannot be returned by value (use a pointer)", f);
            }
            f->pty.clear();
            f->pty_ref.clear();
            f->pty_cref.clear();
            for (auto& p : f->params) {
                TypePtr pt = resolve_type(p->spec.get(), u);
                if (!p->by_ref) pt = decay_param_type(pt);
                if (is_void(pt)) err("parameter `" + p->name + "` cannot be void", p.get());
                if (noncopyable_struct(pt)) {
                    err("struct `" + pt->str() +
                            "` has a destructor and can only be passed by pointer (passing by value would copy)",
                        p.get());
                }
                if (p->by_ref) {
                    if (f->is_extern) {
                        err("`extern` functions cannot take `ref` parameters (`&` borrows are a language mechanism)", p.get());
                    }
                    if (!borrowable_type(pt)) {
                        if (pt->kind == TypeKind::Struct && pt->is_union) {
                            err("`ref` parameters cannot borrow a union value", p.get());
                        }
                        if (pt->kind == TypeKind::Struct) {
                            err("struct `" + pt->str() +
                                    "` cannot be borrowed by `ref` (it contains reference-counted fields or a destructor)",
                                p.get());
                        }
                        err("`ref` parameters can only borrow a struct or scalar value (found `" + pt->str() + "`)", p.get());
                    }
                }
                f->pty.push_back(pt);
                f->pty_ref.push_back(p->by_ref);
                f->pty_cref.push_back(p->by_ref && p->is_const);
            }
            if (f->is_extern) {
                // A delegate owns an environment: C has no such ABI (and no way to release it).
                if (f->rty && f->rty->kind == TypeKind::Delegate) {
                    err("`extern` function `" + f->name +
                            "` cannot return a `delegate` (the environment is reference counted; pass "
                            "`.code`/`.env` explicitly as `void*`)",
                        f);
                }
                for (auto& pt : f->pty) {
                    if (pt && pt->kind == TypeKind::Delegate) {
                        err("`extern` function `" + f->name +
                                "` cannot take a `delegate` parameter (pass `.code`/`.env` explicitly as "
                                "`void*`)",
                            f);
                    }
                }
                if (contains_reference_type(f->rty)) {
                    err("`extern` function `" + f->name + "` cannot return a class or interface (C code does not participate in reference counting)",
                        f);
                }
                for (auto& pt : f->pty) {
                    if (contains_reference_type(pt)) {
                        err("parameter of `extern` function `" + f->name + "` cannot be a class or interface", f);
                    }
                }
                if (contains_slice_type(f->rty) || [&] {
                        for (auto& pt : f->pty)
                            if (contains_slice_type(pt)) return true;
                        return false;
                    }()) {
                    err("`extern` function `" + f->name +
                            "` cannot use dynamic arrays `T[]` (they carry an RC header and are not C layout)",
                        f);
                }
                f->cname = f->name;
            } else if (f->name == "main" && u->name == entry_) {
                f->cname = "mu_main";
                f->is_entry = true;
            } else {
                f->cname = func_cname(u->name, f);
            }
        }
    }
Sema::Sema(std::vector<std::unique_ptr<Unit>> units, std::string entry)
    : units_(std::move(units)), entry_(std::move(entry)) {}

Program Sema::run() {
    collect();
    place_structs();
    for (auto& u : units_) resolve_unit_types(u.get());
    check_layouts();
    check_hierarchy_cycles();
    compute_abstract_all();
    check_public_signatures();
    // Before any body is checked: `struct S : I` gains the interface's default implementations as
    // ordinary methods, so a call `s.m()` in another unit's function body already resolves.
    for (auto& u : units_) {
        cur_ = u.get();
        for (auto& d : u->decls) {
            if (auto sd = dynamic_cast<StructDecl*>(d.get())) {
                recover([&] { apply_struct_iface_defaults(sd); });
            }
        }
    }
    for (auto& u : units_) check_unit(u.get());
    if (!errors_.empty()) throw MuErrorList(std::move(errors_));
    check_entry();

    // Ownership of the generic instantiation products goes to the unit (Program only holds units; they must not dangle after Sema is destroyed)
    if (!generic_decls_.empty()) {
        if (!units_.empty()) {
            for (auto& d : generic_decls_) units_.front()->decls.push_back(std::move(d));
        }
        generic_decls_.clear();
    }
    // Synthesized lambda environment classes (U41): codegen emits their descriptors and destructors
    // from the unit's declaration list, like any other class.
    if (!synth_decls_.empty()) {
        if (!units_.empty()) {
            for (auto& d : synth_decls_) units_.front()->decls.push_back(std::move(d));
        }
        synth_decls_.clear();
    }

    Program prog;
    prog.units = std::move(units_);
    prog.infos = std::move(infos_);
    prog.entry = entry_;
    return prog;
}

}  // namespace muc_detail
