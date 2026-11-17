// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file holds fields/utilities/visibility/collection/signature checks/layout modifiers (definitions of Sema members).
#include "sema_impl.hpp"

#include <iterator>

namespace muc_detail {

    [[noreturn]] void Sema::err(const std::string& msg, const Node* node) const {
        const int line = node ? node->line : 0;
        const int col = node ? node->col : 0;
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string(), inst_notes_text());
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
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string(), inst_notes_text());
    }

    std::string Sema::inst_notes_text() const {
        // Innermost instantiation first: the reader asked for the outermost call, but the body that
        // failed is the innermost one, so the chain reads "error → who expanded it → who expanded that".
        std::string notes;
        for (auto it = instantiation_notes_.rbegin(); it != instantiation_notes_.rend(); ++it) {
            notes += "\n" + *it;
        }
        return notes;
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

namespace {
// The parser folds literals and arithmetic in a `const` initializer; a `sizeof`/`alignof`/`offsetof`
// form needs the type, so collect evaluates the constant instead (basic types only until the layout
// pass). Scanning the initializer keeps every other constant on the existing path.
bool has_sizeof_expr(const Expr* e) {
    if (!e) return false;
    if (dynamic_cast<const SizeofExpr*>(e)) return true;
    if (auto b = dynamic_cast<const Binary*>(e)) {
        return has_sizeof_expr(b->left.get()) || has_sizeof_expr(b->right.get());
    }
    if (auto u = dynamic_cast<const Unary*>(e)) return has_sizeof_expr(u->operand.get());
    if (auto n = dynamic_cast<const NotExpr*>(e)) return has_sizeof_expr(n->operand.get());
    if (auto c = dynamic_cast<const Cast*>(e)) return has_sizeof_expr(c->expr.get());
    if (auto c = dynamic_cast<const Call*>(e)) {
        if (has_sizeof_expr(c->callee.get())) return true;
        for (const auto& a : c->args) {
            if (has_sizeof_expr(a.get())) return true;
        }
    }
    if (auto m = dynamic_cast<const Member*>(e)) return has_sizeof_expr(m->obj.get());
    return false;
}
bool has_sizeof_init(const Init* i) {
    if (auto ie = dynamic_cast<const InitExpr*>(i)) return has_sizeof_expr(ie->expr.get());
    return false;
}
}  // namespace

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
            // The declaration list is processed in place: a `DirectiveDecl` (P3.2) is expanded to the
            // declarations of its selected arm before the loop moves on. `current_consts` follows the
            // text order, so a later constant is not visible to an earlier `#if` (SPEC 5.1).
            UserConstTable current_consts;
            for (size_t di = 0; di < u->decls.size();) {
                if (auto* dd = dynamic_cast<DirectiveDecl*>(u->decls[di].get())) {
                    // Evaluate the arms with the constants visible at this point: the public constants
                    // of the `use`d units (and the compiler builtins after `use std.comptime`) plus the
                    // unit's own constants declared before this point (text order, P3.3).
                    UserConstTable table;
                    std::unordered_map<std::string, std::string> const_src;
                    std::unordered_set<std::string> ambiguous;
                    for (auto& use : u->uses) {
                        if (use->unit_name == "std.comptime") {
                            add_builtin_constants(table);
                            for (const auto& [k, v] : table) const_src.emplace(k, "std.comptime");
                            continue;
                        }
                        const UserConstTable* t = unit_public_consts(use->unit_name);
                        if (!t) continue;
                        for (const auto& [k, v] : *t) {
                            auto it = const_src.find(k);
                            if (it == const_src.end()) const_src[k] = use->unit_name;
                            else if (it->second != use->unit_name) ambiguous.insert(k);
                            table.emplace(k, v);
                            table[use->unit_name + "." + k] = v;  // the qualified form
                        }
                    }
                    for (const auto& [k, v] : current_consts) {
                        table[k] = v;  // this unit wins; a same-name constant is not ambiguous
                        ambiguous.erase(k);
                    }
                    DirectiveDecl::Arm* chosen = nullptr;
                    bool taken = false;
                    for (auto& arm : dd->arms) {
                        if (!arm.cond) {
                            if (!taken) chosen = &arm;  // `#else`: only when nothing above matched
                            break;
                        }
                        std::string why;
                        if (eval_directive_cond(*arm.cond, why, &table, &ambiguous)) {
                            chosen = &arm;
                            taken = true;
                            break;
                        }
                        if (!why.empty()) err("`#if` condition: " + why, arm.cond.get());
                    }
                    std::vector<std::unique_ptr<Decl>> picked;
                    if (chosen) picked = std::move(chosen->decls);
                    // The selected arm may carry `use` (added to the unit now) and `#error`.
                    std::vector<std::unique_ptr<Decl>> kept;
                    for (auto& pd : picked) {
                        if (auto* uu = dynamic_cast<UseDecl*>(pd.get())) {
                            if (!infos_.count(uu->unit_name)) {
                                err("cannot find unit `" + uu->unit_name + "`", uu);
                            }
                            u->uses.push_back(std::unique_ptr<UseDecl>(static_cast<UseDecl*>(pd.release())));
                            continue;
                        }
                        if (auto* ee = dynamic_cast<ErrorDecl*>(pd.get())) {
                            err("#error " + ee->message, ee);
                        }
                        kept.push_back(std::move(pd));
                    }
                    u->decls.erase(u->decls.begin() + static_cast<long>(di));
                    u->decls.insert(u->decls.begin() + static_cast<long>(di),
                                    std::make_move_iterator(kept.begin()),
                                    std::make_move_iterator(kept.end()));
                    continue;  // reprocess the same index
                }
                auto& d = u->decls[di];
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
                } else if (auto sm = dynamic_cast<SumDecl*>(d.get())) {
                    if (ui.sums.count(sm->name)) err("duplicate definition of sum type `" + sm->name + "`", sm);
                    ui.sums[sm->name] = sm;
                } else if (auto a = dynamic_cast<TypeAliasDecl*>(d.get())) {
                    if (ui.aliases.count(a->name)) err("duplicate definition of type alias `" + a->name + "`", a);
                    ui.aliases[a->name] = a;
                } else if (auto g = dynamic_cast<GlobalVar*>(d.get())) {
                    if (ui.globals.count(g->name)) err("duplicate definition of variable `" + g->name + "`", g);
                    ui.globals[g->name] = g;
                    g->cname = g->is_extern ? g->name : mangle(u->name, g->name);
                    if (g->is_const) {
                        bool have = false;
                        if (const UserConstTable* t = unit_all_consts(u->name)) {
                            auto it = t->find(g->name);
                            if (it != t->end()) {
                                current_consts[g->name] = it->second;
                                have = true;
                            }
                        }
                        if (!have && g->init && !g->is_builtin && has_sizeof_init(g->init.get())) {
                            // The parser folds literals and arithmetic; a `sizeof`/`alignof` initializer
                            // needs the type, so it is evaluated here (basic types only until the layout
                            // pass; a struct value stays a runtime initializer).
                            g->rty = resolve_type(g->spec.get(), u.get());
                            if (g->rty && g->rty->kind != TypeKind::Struct &&
                                g->rty->kind != TypeKind::Class) {
                                ConstVal v;
                                if (eval_global_const(g, u->name, v)) {
                                    UserConst uc;
                                    uc.value = v.i;
                                    uc.is_bool = v.ty && v.ty->is_bool();
                                    if (v.ty && v.ty->kind == TypeKind::Enum) uc.enum_ns = v.ty->name;
                                    current_consts[g->name] = uc;
                                }
                            }
                        }
                    }
                }
                ++di;
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
                } else if (auto sm = dynamic_cast<SumDecl*>(node)) {
                    nm = sm->name;
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
                // `std.thread` / `std.task` are the threading runtime: with `--single-thread` the RC
                // operations are plain loads/stores, so touching either unit is an error rather than
                // a silent data race. The library's own units (`std.*`) are exempt.
                if (single_thread_ && u->name.compare(0, 4, "std.") != 0 &&
                    (use->unit_name == "std.thread" || use->unit_name.compare(0, 11, "std.thread.") == 0 ||
                     use->unit_name == "std.task" || use->unit_name.compare(0, 9, "std.task.") == 0)) {
                    err("unit `" + use->unit_name + "` needs atomic reference counting; " +
                            "threading cannot be used with `--single-thread`",
                        use.get());
                }
            }
        }
        check_comptime_unit();
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
                } else if (auto sm = dynamic_cast<SumDecl*>(d.get())) {
                    // The payload/layout types are filled in by `resolve_sum` (they need the alternatives'
                    // field types resolved).
                    sm->ty = Type::make_sum(u->name, sm->name, {}, {}, {}, nullptr);
                }
            }
        }
    }

    // Sum type (SPEC 10.1): resolve the alternatives' payload types and synthesize the layout type
    // `struct { i32 tag; union { payload... } }`. `sd->ty` must already exist (created in
    // `place_structs` for a declared sum, or by the instantiator for a generic instance), and it is
    // filled in place so every reference keeps pointing at the same object.
    void Sema::resolve_sum(SumDecl* sd, Unit* u) {
        if (sd->alts.size() < 2) {
            err("sum type `" + sd->name + "` needs at least two alternatives", sd);
        }
        std::vector<std::string> names;
        std::vector<std::vector<TypePtr>> fields;
        std::vector<TypePtr> payload;
        std::unordered_set<std::string> seen;
        for (auto& alt : sd->alts) {
            if (!seen.insert(alt.name).second) {
                err("duplicate alternative `" + alt.name + "` in sum type `" + sd->name + "`", sd);
            }
            std::vector<TypePtr> fts;
            for (auto& fs : alt.fields) {
                TypePtr ft = resolve_type(fs.get(), u);
                if (is_void(ft)) err("payload of alternative `" + alt.name + "` cannot be `void`", sd);
                // A payload stored by value cannot contain the sum itself: `type H = Holds(H) | None`
                // has no finite layout. `resolve_type` returns the same Type object, so identity is
                // enough (a pointer to the sum is fine and is the documented way to recurse).
                if (ft.get() == sd->ty.get()) {
                    err("payload of alternative `" + alt.name + "` would make sum `" + sd->name +
                            "` infinitely large (store a pointer or a class handle instead)",
                        sd);
                }
                if (noncopyable_struct(ft)) {
                    err("payload of alternative `" + alt.name + "` is struct `" + ft->str() +
                            "`, which has a destructor and cannot be stored by value in a sum (use a "
                            "pointer or a class)",
                        sd);
                }
                fts.push_back(ft);
            }
            TypePtr pt;
            if (fts.size() == 1) {
                pt = fts[0];
            } else if (fts.size() > 1) {
                // several payload fields: an anonymous struct laid out like a tuple
                pt = Type::make_struct(u->name, "$alt$" + sd->name + "$" + alt.name);
                for (size_t i = 0; i < fts.size(); ++i) {
                    Field f;
                    f.name = "f" + std::to_string(i);
                    f.type = fts[i];
                    pt->fields.push_back(std::move(f));
                }
            }
            names.push_back(alt.name);
            payload.push_back(pt);
            fields.push_back(std::move(fts));
        }
        // payload area = a union of the alternatives (all at offset 0, like `union`)
        TypePtr uni = Type::make_struct(u->name, "$payload$" + sd->name);
        uni->is_union = true;
        for (size_t i = 0; i < payload.size(); ++i) {
            if (!payload[i]) continue;
            Field f;
            f.name = "a" + std::to_string(i);
            f.type = payload[i];
            uni->fields.push_back(std::move(f));
        }
        if (uni->fields.empty()) {  // all alternatives are unit values: keep the union non-empty
            Field f;
            f.name = "none";
            f.type = Type::make_prim("byte");
            uni->fields.push_back(std::move(f));
        }
        TypePtr layout = Type::make_struct(u->name, "$sum$" + sd->name);
        {
            Field tag;
            tag.name = "tag";
            tag.type = Type::make_prim("int");
            layout->fields.push_back(std::move(tag));
            Field pay;
            pay.name = "payload";
            pay.type = uni;
            layout->fields.push_back(std::move(pay));
        }
        sd->ty->sum_names = std::move(names);
        sd->ty->sum_fields = std::move(fields);
        sd->ty->sum_payload = std::move(payload);
        sd->ty->sum_layout = std::move(layout);
    }

    void Sema::resolve_class_fields(ClassDecl* c, Unit* u) {        std::vector<Field> fields;
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
            // A class field of an uncopyable struct is allowed: a class is a reference type, so its "copy"
            // retains the handle and never copies the field (agents/value-semantics.md §8).
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
                sd->ty->nocopy = sd->nocopy;
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
            sd->ty->nocopy = sd->nocopy;
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
                // A struct with a destructor is a legal member: construction, destruction and the
                // copyability gate are all recursive (agents/value-semantics.md §4).
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
                    if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.is_const = m->is_const;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = false;
                sig.is_override = m->is_override;
                sig.is_public = m->is_public;
                sig.owner = sd->ty;
                sig.is_operator = m->is_operator;
                if (m->is_operator) check_member_operator_sig(m.get(), sd->ty);
                sd->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, sd->name, m->name, m->pty, m->is_const);
            }
            sd->ty->ctors.clear();
            for (auto& ctor : sd->ctors) {
                ctor->pty.clear();
                for (auto& p : ctor->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (p->by_move) {
                        // `T(move T other)`: the move constructor (agents/value-semantics.md §8.1)
                        if (ctor->params.size() != 1 || !same_type(pt, sd->ty)) {
                            err("the move constructor of struct `" + sd->name +
                                    "` must be written `" + sd->name + "(move " + sd->name +
                                    " other)` (one parameter of the struct's own type)",
                                p.get());
                        }
                        if (sd->ty->has_move_ctor) {
                            err("struct `" + sd->name + "` already declares a move constructor", p.get());
                        }
                        ctor->is_move = true;
                        sd->ty->has_move_ctor = true;
                    }
                    if (p->by_ref) {
                        // Only the copy constructor may borrow its argument (agents/value-semantics.md §8.1):
                        // `T(const ref T other)`. Any other `ref` parameter on a constructor would need the
                        // argument-passing machinery that constructors do not have.
                        if (ctor->params.size() != 1 || !same_type(pt, sd->ty)) {
                            err("a constructor may take a `ref` parameter only as the copy constructor "
                                "`" + sd->name + "(const ref " + sd->name + " other)`",
                                p.get());
                        }
                        if (!p->is_const) {
                            err("a copy constructor must not modify its source: write `" + sd->name +
                                    "(const ref " + sd->name + " other)`",
                                p.get());
                        }
                        if (sd->ty->has_copy_ctor) {
                            err("struct `" + sd->name + "` already declares a copy constructor", p.get());
                        }
                        if (sd->ty->nocopy) {
                            err("struct `" + sd->name + "` is `nocopy`: it cannot declare a copy constructor",
                                p.get());
                        }
                        ctor->is_copy = true;
                        sd->ty->has_copy_ctor = true;
                    }
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (!p->by_ref && !ctor->is_move && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
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
                ctor->cname = ctor->is_move ? move_ctor_cname(u->name, sd->name)
                                            : ctor->is_copy ? copy_ctor_cname(u->name, sd->name)
                                                            : ctor_cname(u->name, sd->name, ctor->pty);
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
                    if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.is_const = m->is_const;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = !m->body;
                sig.is_override = m->is_override;
                // An abstract method has no implementation to name, so it is always virtual; an `override`
                // stays virtual (a further subclass may override it too) — `check_class` verifies the base
                // declaration allows that. Everything else needs `virtual` (build 160: static by default).
                sig.is_virtual = m->is_virtual || m->is_override || !m->body;
                sig.is_public = m->is_public;
                sig.owner = c->ty;
                sig.is_operator = m->is_operator;
                if (m->is_operator) check_member_operator_sig(m.get(), c->ty);
                c->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, c->name, m->name, m->pty, m->is_const);
                if (!m->body) c->ty->is_abstract = true;
            }
            for (auto& ctor : c->ctors) {
                ctor->pty.clear();
                for (auto& p : ctor->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (p->by_move) {
                        err("`class` is a reference type: it has no move or copy constructor, so `move` "
                            "parameters are only meaningful on a struct",
                            p.get());
                    }
                    if (!p->by_ref) pt = decay_param_type(pt);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
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

        // sum types: payload + layout (after the struct/class fields they may mention)
        for (auto& d : u->decls) {
            if (auto sm = dynamic_cast<SumDecl*>(d.get())) resolve_sum(sm, u);
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
                    if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                        err("struct `" + pt->str() +
                                "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.is_const = m->is_const;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = !m->body;
                sig.is_override = false;
                sig.owner = itf->ty;
                sig.is_operator = m->is_operator;
                sig.ret_self = m->ret && m->ret->is_self;
                for (auto& p : m->params) sig.params_self.push_back(p->spec && p->spec->is_self);
                if (m->is_operator) init_interface_operator(m.get(), sig, itf->name);
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
            if (noncopyable_struct(referent_of(f->rty)) && !referent_of(f->rty)->movable()) {
                err("struct `" + referent_of(f->rty)->str() +
                        "` cannot be returned by value: " + noncopyable_reason(referent_of(f->rty)) +
                        "; declare `" + referent_of(f->rty)->str() + "(const ref " + referent_of(f->rty)->str() + " other)` or `" + referent_of(f->rty)->str() + "(move " + referent_of(f->rty)->str() + " other)`" + (borrowable_type(referent_of(f->rty)) ? ", or return a `ref " + referent_of(f->rty)->str() + "` borrow" : ""),
                    f);
            }
            // `extern` imports a C symbol and `export` publishes one for DLL consumers, so both sides
            // of the boundary must obey the C ABI; a purely internal function keeps the faster one.
            const bool c_abi = f->is_extern || f->is_export;
            const std::string ckw = f->is_extern ? "`extern`" : "`export`";
            f->c_abi = c_abi;
            f->pty.clear();
            f->pty_ref.clear();
            f->pty_cref.clear();
            for (auto& p : f->params) {
                TypePtr pt = resolve_type(p->spec.get(), u);
                if (!p->by_ref) pt = decay_param_type(pt);
                if (is_void(pt)) err("parameter `" + p->name + "` cannot be void", p.get());
                if (!p->by_ref && noncopyable_struct(pt) && !pt->movable()) {
                    err("struct `" + pt->str() +
                            "` cannot be passed by value: " + noncopyable_reason(pt) +
                                (borrowable_type(pt) ? "; take it as `const ref " + pt->str() + "`" : "; declare `" + pt->str() + "(const ref " + pt->str() + " other)`"),
                        p.get());
                }
                // A C caller never participates in Mulan's ownership protocol: it cannot move a value
                // into the callee or hand over a resource, so a by-value struct that needs cleanup
                // (a destructor, or a field/array that owns something) is rejected at the boundary
                // even when it is movable inside the language (#30).
                if (c_abi && !p->by_ref && pt->kind == TypeKind::Struct && pt->needs_cleanup() &&
                    !contains_reference_type(pt)) {
                    err(ckw + " function `" + f->name + "` cannot take struct `" + pt->str() +
                            "` by value: it needs cleanup (" + noncopyable_reason(pt) +
                            "), and a C caller does not own it the way a Mulan callee expects; pass a "
                            "pointer or use a plain C struct",
                        p.get());
                }
                if (p->by_ref) {
                    if (c_abi) {
                        err(ckw + " functions cannot take `ref` parameters (`&` borrows are a language mechanism; write a pointer)", p.get());
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
            if (c_abi) {
                // C has no array results: a C header returns a pointer (or fills a struct).
                if (f->rty && f->rty->kind == TypeKind::Array) {
                    err(ckw + " function `" + f->name + "` cannot return a fixed-size array `" +
                            f->rty->str() + "`: C has no array result (return a pointer or wrap it in a struct)",
                        f);
                }
                // A C caller never participates in Mulan's ownership protocol: it cannot move a value
                // into the callee or hand over a resource, so a by-value struct that needs cleanup
                // (a destructor, or a field/array that owns something) is rejected at the boundary
                // even when it is movable inside the language (#30).
                if (f->rty && f->rty->kind == TypeKind::Struct && f->rty->needs_cleanup() &&
                    !contains_reference_type(f->rty)) {
                    err(ckw + " function `" + f->name + "` cannot return struct `" + f->rty->str() +
                            "` by value: it needs cleanup (" + noncopyable_reason(f->rty) +
                            "), and a C caller does not participate in Mulan's ownership protocol; "
                            "return a pointer or use a plain C struct",
                        f);
                }
                // A delegate owns an environment: C has no such ABI (and no way to release it).
                if (contains_delegate_type(f->rty)) {
                    err(ckw + " function `" + f->name +
                            "` cannot return a `delegate` (the environment is reference counted; pass "
                            "`.code`/`.env` explicitly as `void*`)",
                        f);
                }
                for (auto& pt : f->pty) {
                    if (contains_delegate_type(pt)) {
                        err(ckw + " function `" + f->name +
                                "` cannot take a `delegate` parameter (pass `.code`/`.env` explicitly as "
                                "`void*`)",
                            f);
                    }
                }
                if (contains_reference_type(f->rty)) {
                    err(ckw + " function `" + f->name + "` cannot return a class or interface (C code does not participate in reference counting)",
                        f);
                }
                for (auto& pt : f->pty) {
                    if (contains_reference_type(pt)) {
                        err("parameter of " + ckw + " function `" + f->name + "` cannot be a class or interface", f);
                    }
                }
                if (contains_slice_type(f->rty) || [&] {
                        for (auto& pt : f->pty)
                            if (contains_slice_type(pt)) return true;
                        return false;
                    }()) {
                    err(ckw + " function `" + f->name +
                            "` cannot use dynamic arrays `T[]` (they carry an RC header and are not C layout)",
                        f);
                }
                // Sum types are a built-in whose layout is an implementation detail: build a plain
                // `struct { tag; union }` on the C side and convert explicitly (SPEC 10.1.1).
                if (contains_sum_type(f->rty) || [&] {
                        for (auto& pt : f->pty)
                            if (contains_sum_type(pt)) return true;
                        return false;
                    }()) {
                    err(ckw + " function `" + f->name +
                            "` cannot use a sum type: its tag+payload layout is an implementation detail, "
                            "not a public interface (pass a plain `struct { tag; union }` and convert)",
                        f);
                }
                if (f->is_extern) f->cname = f->name;  // the C symbol itself
            }
            if (!f->is_extern) {
                if (f->name == "main" && u->name == entry_) {
                    f->cname = "mu_main";
                    f->is_entry = true;
                } else {
                    f->cname = func_cname(u->name, f);
                }
            }
        }
    }
Sema::Sema(std::vector<std::unique_ptr<Unit>> units, std::string entry, bool single_thread,
           bool strict)
    : units_(std::move(units)), entry_(std::move(entry)), single_thread_(single_thread),
      strict_(strict) {}

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
    // A constrained generic body is checked at its definition (build 156), after every unit's own
    // bodies: the constraint types must be fully resolved by then.
    check_generic_contracts();
    if (strict_) warn_uninstantiated_generics();
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

    void Sema::init_interface_operator(MethodDecl* m, MethodSig& sig, const std::string& iface_name) {
        // The receiver is the first operand: an interface member has no `this`, so `Self` is the only
        // way to say the operator applies to the implementor. Only that operand may borrow (the rest
        // are ordinary operands, e.g. the byte of a stream).
        if (m->params.empty() || !m->params[0]->spec || !m->params[0]->spec->is_self) {
            err("interface `" + iface_name +
                    "`: an operator must spell its receiver as the first `Self` operand "
                    "(write `T operator" + m->name.substr(8) + "(Self c, ...)`)",
                m);
        }
        for (size_t i = 1; i < m->params.size(); ++i) {
            if (m->params[i]->by_ref) {
                err("interface `" + iface_name +
                        "`: only the receiver (the first operand) may be a `ref` parameter",
                    m->params[i].get());
            }
        }
        // The witness type has no storage, so a borrow return can only mean "the element the
        // subscription exposes"; every other operator must return a value.
        if (m->ret_ref && m->name != "operator[]") {
            err("interface `" + iface_name +
                    "`: only `operator[]` may return a borrow; another operator's receiver is a value, "
                    "not storage",
                m);
        }
        if (!m->params.empty()) {
            sig.recv_ref = m->params[0]->by_ref;
            sig.recv_cref = sig.recv_ref && m->params[0]->is_const;
        }
    }

}  // namespace muc_detail
