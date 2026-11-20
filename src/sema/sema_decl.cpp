// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Units, visibility, diagnostics and the pass entry checks (definitions of Sema members).
//
// owns:      the unit table and its visibility rules (`use`, libraries, `public/private`, `export`),
//            unit collection and pruning, the public-signature check, the diagnostics helpers
//            (err / warn / err_ts / inst_notes_text) and `implicit_ok`
// not:       declaration/field/type resolution (sema_resolve_types.cpp), layout (sema_layout.cpp),
//            pass orchestration (sema_run.cpp), expression or statement checks
// pass:      collect / prune and the public-signature check; the helpers are used by every pass
// concept:   units, visibility, diagnostics, public-signatures
#include "sema_impl.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace muc_detail {

    [[noreturn]] void Sema::err(const std::string& msg, const Node* node) const {
        const int line = node ? node->line : 0;
        const int col = node ? node->col : 0;
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string(), inst_notes_text(msg));
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
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string(), inst_notes_text(msg));
    }

    std::string Sema::inst_notes_text(const std::string& msg) const {
        // Innermost instantiation first: the reader asked for the outermost call, but the body that
        // failed is the innermost one, so the chain reads "error → who expanded it → who expanded that".
        // The "add a constraint" explanation is attached to the innermost frame only, and only when
        // the error is a missing *capability* a bound would supply (an operator, a method). On a shape
        // error — a misspelled field, a missing member, an undefined name, a wrong argument count — it
        // sends the reader hunting for a constraint that would not have prevented the mistake (#134).
        const bool shape_error = msg.find("has no field") != std::string::npos ||
                                 msg.find("has no member") != std::string::npos ||
                                 msg.find("unknown identifier") != std::string::npos ||
                                 msg.find("undefined identifier") != std::string::npos ||
                                 (msg.find("expects ") != std::string::npos &&
                                  msg.find(" argument") != std::string::npos);
        const bool explain = !shape_error;
        std::string notes;
        for (size_t i = instantiation_notes_.size(); i > 0; --i) {
            notes += "\n" + instantiation_notes_[i - 1];
            if (i == instantiation_notes_.size() && explain && i <= instantiation_hints_.size()) {
                notes += instantiation_hints_[i - 1];
            }
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
// The const types whose initializer the `#if` evaluator can fold (the same set the parser used).
bool is_scalar_const_type(const TypeSpec* spec) {
    if (!spec || spec->kind != TypeSpec::K::Named) return false;
    // Floating-point `const`s are compile-time constants as well (SPEC 3.7.2 lists floating-point
    // literals and `as` conversions), so a `#if VALUE > 1.0` can read one (#74).
    static const std::unordered_set<std::string> scalars = {
        "bool",   "byte", "char",   "short", "ushort", "int",
        "uint",   "long", "ulong",  "usize", "isize",  "float",
        "double"};
    return scalars.count(spec->name) != 0;
}
}  // namespace

    void Sema::collect() {
        // Generic templates are moved out of the declaration list as the per-unit pass reaches them
        // (see below): they take no part in the regular checks, and their conditional member groups are
        // expanded at their own source position, like any other type.
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
            // Visible to a `sizeof` pre-resolution that happens while this walk runs (#82).
            struct CollectConstsGuard {
                const UserConstTable*& slot;
                const UserConstTable* saved;
                explicit CollectConstsGuard(const UserConstTable*& s, const UserConstTable* v)
                    : slot(s), saved(s) {
                    slot = v;
                }
                ~CollectConstsGuard() { slot = saved; }
            } collect_guard(collect_consts_, &current_consts);
            for (size_t di = 0; di < u->decls.size();) {
                if (auto* dd = dynamic_cast<DirectiveDecl*>(u->decls[di].get())) {
                    // Evaluate the arms with the constants visible at this point: the public constants
                    // of the `use`d units (and the compiler builtins after `use std.comptime`) plus the
                    // unit's own constants declared before this point (text order, P3.3).
                    const DirectiveScope scope = directive_scope(u.get(), &current_consts, dd->line);
                    DirectiveDecl::Arm* chosen = choose_decl_arm(*dd, scope);
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
                if (!d->type_params.empty()) {
                    // `export` names a concrete C ABI symbol; a template has none until it is
                    // instantiated and its mangled name is not a C name. Reject it instead of silently
                    // ignoring the modifier.
                    if (d->is_export) {
                        err("a generic declaration cannot be `export`ed: `export` needs a concrete C ABI "
                            "symbol; write a non-generic wrapper",
                            d.get());
                    }
                    // A generic template: expanded at its own source position (the same constants an
                    // ordinary type sees), then set aside for instantiation. It is indexed right away so
                    // a later declaration in this unit may already instantiate it (`sizeof(Box<int>)`).
                    expand_type_decl(d.get(), u.get(), &current_consts);
                    d->is_template = true;
                    index_template(d.get(), u->name);
                    u->templates.push_back(std::move(d));
                    u->decls.erase(u->decls.begin() + static_cast<long>(di));
                    continue;
                }
                // A conditional member group is evaluated where the type is declared: the constants
                // visible at this point follow the text order (SPEC 5.1).
                expand_type_decl(d.get(), u.get(), &current_consts);
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    ui.funcs[f->name].push_back(f);
                    f->sym = f;
                } else if (auto s = dynamic_cast<StructDecl*>(d.get())) {
                    ui.structs[s->name] = s;
                } else if (auto e = dynamic_cast<EnumDecl*>(d.get())) {
                    ui.enums[e->name] = e;
                    // Enum members are compile-time constants (SPEC 3.7.2), and `#if` uses the same name
                    // resolution (SPEC 9.2). Each member is visible under both of its spellings: the
                    // unqualified `Red` (members share the unit's unqualified namespace) and the
                    // qualified `Color.Red`.
                    {
                        long long v = 0;
                        bool has = false;
                        for (auto& [m, val] : e->members) {
                            if (val) {
                                v = *val;
                                has = true;
                            } else if (has) {
                                v += 1;
                            } else {
                                v = 0;
                                has = true;
                            }
                            UserConst uc;
                            uc.value = v;
                            uc.enum_ns = e->name;  // keeps the enum identity for `#if` comparisons
                            current_consts.emplace(m, uc);
                            current_consts[e->name + "." + m] = uc;
                        }
                    }
                } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                    if (ui.classes.count(c->name)) err("duplicate definition of class `" + c->name + "`", c);
                    ui.classes[c->name] = c;
                } else if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) {
                    if (ui.interfaces.count(i->name)) err("duplicate definition of interface `" + i->name + "`", i);
                    ui.interfaces[i->name] = i;
                } else if (auto sm = dynamic_cast<SumDecl*>(d.get())) {
                    if (ui.sums.count(sm->name)) err("duplicate definition of variant `" + sm->name + "`", sm);
                    ui.sums[sm->name] = sm;
                } else if (auto a = dynamic_cast<TypeAliasDecl*>(d.get())) {
                    if (ui.aliases.count(a->name)) err("duplicate definition of type alias `" + a->name + "`", a);
                    ui.aliases[a->name] = a;
                } else if (auto ed = dynamic_cast<ExtendDecl*>(d.get())) {
                    // `extend I with J;` — recorded here; validation and application are a later pass.
                    ui.extensions.push_back(ed);
                } else if (auto g = dynamic_cast<GlobalVar*>(d.get())) {
                    if (ui.globals.count(g->name)) err("duplicate definition of variable `" + g->name + "`", g);
                    ui.globals[g->name] = g;
                    g->cname = g->is_extern ? g->name : mangle(u->name, g->name);
                    if (g->is_const) {
                        // Evaluate the initializer of *this* declaration: the `#if` selection above may
                        // have picked an arm that declares a different value under the same name than a
                        // dead arm did, so a name-keyed table cannot answer for it.
                        bool have = false;
                        if (g->init && !g->is_builtin && is_scalar_const_type(g->spec.get())) {
                            if (auto* ie = dynamic_cast<InitExpr*>(g->init.get()); ie && ie->expr) {
                                UserConst uc;
                                std::string why;
                                // The initializer sees exactly what a `#if` at this position sees: this
                                // unit's constants declared so far plus the `public const`s (and public
                                // enum members) of the `use`d units, so `use dep; const int ALIAS = K + 1;`
                                // folds (#81).
                                const DirectiveScope init_scope = directive_scope(u.get(), &current_consts, g->line);
                                if (eval_user_const(*ie->expr, init_scope.table, uc, why)) {
                                    // The declared type fixes the width every later use folds at.
                                    const std::string& tn = g->spec->name;
                                    uc.is_bool = tn == "bool";
                                    uc.bits = tn == "byte" || tn == "char" || tn == "bool"    ? 8
                                              : tn == "short" || tn == "ushort"              ? 16
                                              : tn == "long" || tn == "ulong" || tn == "usize" ||
                                                        tn == "isize"                        ? 64
                                                                                            : 32;
                                    uc.is_signed = tn != "uint" && tn != "ulong" && tn != "usize" &&
                                                   tn != "byte";
                                    current_consts[g->name] = uc;
                                    have = true;
                                }
                            }
                        }
                        if (!have && g->init && !g->is_builtin && has_sizeof_init(g->init.get())) {
                            // The parser folds literals and arithmetic; a `sizeof`/`alignof` initializer
                            // needs the type, so it is evaluated here. The types named by `sizeof` are
                            // resolved first so a struct layout is available.
                            if (auto* ie = dynamic_cast<InitExpr*>(g->init.get())) {
                                resolve_sizeof_types(ie->expr.get(), u.get());
                            }
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

            // Re-register the unit's constants with the values of the declarations that survived the
            // `#if` selection: a later unit (or a function body checked after collect) must not see a
            // value that only a dead arm declared.
            register_unit_all_consts(u->name, current_consts);
            {
                UserConstTable pub;
                for (auto& d : u->decls) {
                    if (auto* g = dynamic_cast<GlobalVar*>(d.get()); g && g->is_const && g->is_public) {
                        auto it = current_consts.find(g->name);
                        if (it != current_consts.end()) pub[g->name] = it->second;
                    } else if (auto* e = dynamic_cast<EnumDecl*>(d.get()); e && e->is_public) {
                        // A public enum exports its members (`dep.Red`, `dep.Color.Red`), like a public
                        // `const` exports its value.
                        for (auto& [m, val] : e->members) {
                            auto it = current_consts.find(e->name + "." + m);
                            if (it == current_consts.end()) continue;
                            pub[m] = it->second;
                            pub[e->name + "." + m] = it->second;
                        }
                    }
                }
                register_unit_public_consts(u->name, pub);
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

    void Sema::prune_unreachable_units() {
        std::unordered_map<std::string, const Unit*> by_name;
        for (auto& u : units_) by_name[u->name] = u.get();

        std::unordered_set<std::string> reachable;
        std::vector<const Unit*> work;
        for (auto& u : units_) {
            // The entry unit, and the standard library units that are implicit dependencies of every
            // program (string literals, exceptions).
            if (u->name == entry_ || u->name.compare(0, 4, "std.") == 0) work.push_back(u.get());
        }
        while (!work.empty()) {
            const Unit* u = work.back();
            work.pop_back();
            if (!reachable.insert(u->name).second) continue;
            for (auto& use : u->uses) {
                auto it = by_name.find(use->unit_name);
                if (it != by_name.end()) work.push_back(it->second);
            }
        }

        // The loader preloads the units named by *every* arm of a `#if` (their constants may be needed by
        // a later condition), but SPEC 9.2 says an unselected arm does not join the dependency graph. The
        // arms are selected now, so the leftovers are dropped instead of being semantically checked (#80).
        units_.erase(std::remove_if(units_.begin(), units_.end(),
                                    [&](const std::unique_ptr<Unit>& u) {
                                        return !reachable.count(u->name);
                                    }),
                     units_.end());
        for (auto it = infos_.begin(); it != infos_.end();) {
            if (!reachable.count(it->first)) it = infos_.erase(it);
            else ++it;
        }
    }

}  // namespace muc_detail

