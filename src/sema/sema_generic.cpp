// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Generic monomorphization instantiation (U34, M2).
//
// owns:      monomorphization — instantiate_generic, template cloning/substitution, the instance cache
// not:       constraint satisfaction (sema_constraint.cpp), parsing, layout, declaration checks that do not involve a type parameter
// pass:      lazily, from resolve_type the first time an instantiated type is used
// concept:   generics, constraints, witnesses
//
// Instantiation = clone the template AST + substitute type parameters + canonicalize the name + run the regular type resolution.
// The product is an ordinary struct: `same_type` / `mangle` / layout / codegen all need no knowledge of generics.
#include <string>
#include <utility>
#include <vector>

#include "ast_clone.hpp"
#include "ast_subst.hpp"
#include "sema_impl.hpp"

namespace muc_detail {

namespace {

// Keeps the "being instantiated" set exception-safe: a diagnostic thrown from inside the expansion
// must not leave the key behind, or a later use reports a bogus recursive instantiation.
struct BusyGuard {
    std::unordered_set<std::string>& busy;
    std::string key;
    BusyGuard(std::unordered_set<std::string>& b, std::string k) : busy(b), key(std::move(k)) {
        busy.insert(key);
    }
    ~BusyGuard() { busy.erase(key); }
    BusyGuard(const BusyGuard&) = delete;
    BusyGuard& operator=(const BusyGuard&) = delete;
};

// RAII frame for the instantiation chain. The stack is only rendered when a diagnostic fires, so a
// successful instantiation pays nothing beyond a small string.
struct InstNoteGuard {
    std::vector<std::string>& notes;
    std::vector<std::string>& hints;
    InstNoteGuard(std::vector<std::string>& n, std::vector<std::string>& h, std::string text,
                  std::string hint)
        : notes(n), hints(h) {
        notes.push_back(std::move(text));
        hints.push_back(std::move(hint));
    }
    ~InstNoteGuard() {
        notes.pop_back();
        hints.pop_back();
    }
    InstNoteGuard(const InstNoteGuard&) = delete;
    InstNoteGuard& operator=(const InstNoteGuard&) = delete;
};

// A type parameter is checked at the definition only when it has a conjunction of interface bounds;
// a bare `<T>` and an `A | B` union stay instantiation-time (3.15.5).
bool param_checked_at_definition(const TypeParam& tp) {
    return !tp.bounds.empty() && tp.op != TypeParam::Op::Or;
}

// The "why did this fail only now?" explanation for an unchecked parameter.
std::string unchecked_hint(const std::vector<TypeParam>& tps) {
    const std::string names = definition_unchecked_names(tps);
    if (names.empty()) return {};
    std::string first;
    for (const auto& tp : tps) {
        if (!param_checked_at_definition(tp)) {
            first = tp.name;
            break;
        }
    }
    return " — " + names + (definition_unchecked_count(tps) == 1 ? " is" : " are") +
           " unconstrained, so the definition is checked only when it is instantiated; add a "
           "constraint (for example `" + first + ": IAdd` for `+`, `" + first +
           ": IOrder` for comparisons) to check it at the definition";
}

// A structural key for an array-length expression (`SIZE`, `SIZE * 2`, `std.comptime.ptr_bits`): two
// different lengths must never share a generic-instantiation key (SPEC 3.3.2).
static std::string expr_key(const Expr& e) {
    if (auto* p = dynamic_cast<const IntLit*>(&e)) return std::to_string(p->value);
    if (auto* p = dynamic_cast<const CharLit*>(&e)) return "c" + std::to_string(p->value);
    if (auto* p = dynamic_cast<const Name*>(&e)) return p->ident;
    if (auto* p = dynamic_cast<const Member*>(&e)) return expr_key(*p->obj) + "." + p->name;
    if (auto* p = dynamic_cast<const Unary*>(&e)) return p->op + expr_key(*p->operand);
    if (auto* p = dynamic_cast<const Binary*>(&e)) {
        return "(" + expr_key(*p->left) + p->op + expr_key(*p->right) + ")";
    }
    if (auto* p = dynamic_cast<const Cast*>(&e)) return expr_key(*p->expr) + " as " + p->spec->name;
    return "?";
}

}  // namespace

// `T`, `U` (only the parameters that are not checked at the definition). Shared with sema_constraint.cpp.
std::string definition_unchecked_names(const std::vector<TypeParam>& tps) {
    std::string out;
    for (const auto& tp : tps) {
        if (param_checked_at_definition(tp)) continue;
        if (!out.empty()) out += ", ";
        out += "`" + tp.name + "`";
    }
    return out;
}

size_t definition_unchecked_count(const std::vector<TypeParam>& tps) {
    size_t n = 0;
    for (const auto& tp : tps) {
        if (!param_checked_at_definition(tp)) ++n;
    }
    return n;
}

// Type syntax → canonical display name (`[const] Box<Vec<int[]>>`), used for `Type::name` and the
// instance cache key. Shared with sema_constraint.cpp.
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
            return canon_spec(*s.elem) + "[" + (s.len ? expr_key(*s.len) : std::to_string(s.size)) + "]";
        case TypeSpec::K::Func:
        case TypeSpec::K::Delegate: {
            std::string n = s.kind == TypeSpec::K::Delegate ? "delegate (" : "(";
            for (size_t i = 0; i < s.params.size(); ++i) {
                if (i) n += ", ";
                n += canon_spec(*s.params[i]);
            }
            n += ") => " + canon_spec(*s.ret);
            return n;
        }
    }
    return prefix + s.name;
}

// Split `a.b.C` into (a.b, C); unit is empty when there is no prefix. Shared with sema_constraint.cpp.
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

// Stable spelling of one type argument inside an instance name: arguments from the template's own
// unit (or built-ins) keep their bare name, everything else is qualified so two units that both
// declare `Item` get distinct instances of `Box<Item>`.
static std::string instance_label(const TypePtr& t, const TypeSpec* fallback, const std::string& tmpl_unit) {
    if (!t) return fallback ? canon_spec(*fallback) : std::string("?");
    // Pointer/slice/array/function types have no `name`; their textual form already carries any
    // nested unit qualification (`Result<int, const char*>`).
    if (t->unit.empty() || t->unit == tmpl_unit) return t->name.empty() ? t->str() : t->name;
    return t->unit + "." + t->name;
}

// Registers one template under `<unit>.<name>`. Called as each template is moved out of the unit's
// declaration list, so an instantiation later in the same unit (`sizeof(Box<int>)` in a constant) can
// already find it.
void Sema::index_template(Decl* d, const std::string& unit_name) {
    std::string name;
    if (auto s = dynamic_cast<StructDecl*>(d)) {
        name = s->name;
    } else if (auto c = dynamic_cast<ClassDecl*>(d)) {
        name = c->name;
    } else if (auto i = dynamic_cast<InterfaceDecl*>(d)) {
        name = i->name;
    } else if (auto sm = dynamic_cast<SumDecl*>(d)) {
        name = sm->name;
    } else if (auto f = dynamic_cast<FuncDecl*>(d)) {
        name = f->name;
    } else {
        return;
    }
    generic_templates_[unit_name + "." + name] = d;
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
        if (tn == name) return kv.second;  // same name across units: take the first one (M2d simplification)
    }
    return nullptr;
}

TypePtr Sema::instantiate_generic(const TypeSpec* spec, Unit* u) {
    std::string want_unit;
    std::string base;
    split_qualified(spec->name, want_unit, base);

    // Template lookup: explicit unit prefix first; otherwise this unit → the unique same-name template
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

    // Canonical instance name and cache key. A type argument that comes from another unit is
    // qualified: `Box<Item>` written in two units must not collapse into one instance.
    std::vector<TypePtr> arg_types;
    for (const auto& ta : spec->args) arg_types.push_back(resolve_type(ta.get(), u));
    std::string display = base + "<";
    for (size_t i = 0; i < arg_types.size(); ++i) {
        if (i) display += ", ";
        display += instance_label(arg_types[i], spec->args[i].get(), tmpl_unit);
    }
    display += ">";
    std::string key = tmpl_unit + "." + display;
    auto cached = generic_insts_.find(key);
    if (cached != generic_insts_.end()) return cached->second;
    if (generic_busy_.count(key)) {
        err_ts("recursive generic instantiation of `" + display + "`", spec);
    }
    if (generic_depth_ >= kGenericDepthLimit) {
        err_ts("recursive generic instantiation of `" + base + "` does not terminate (more than " +
                   std::to_string(kGenericDepthLimit) +
                   " nested instances; SPEC 3.15.4 requires a finite expansion)",
               spec);
    }
    // Depth alone does not bound the work: `Pair<T, T>` doubles the instance count per level, so a
    // 24-level program asks for 16M instances. The total is capped instead of letting the compile run.
    if (generic_instance_count_ >= kGenericInstanceLimit) {
        err_ts("this program needs more than " + std::to_string(kGenericInstanceLimit) +
                   " generic instantiations (the last is `" + display +
                   "`); SPEC 3.15.4 requires a finite expansion, so simplify the type arguments",
               spec);
    }
    ++generic_instance_count_;
    ++generic_depth_;
    struct DepthGuard {
        int& d;
        ~DepthGuard() { --d; }
    } depth_guard{generic_depth_};
    generic_busy_.insert(key);
    BusyGuard busy_guard{generic_busy_, key};

    auto* sd_tmpl = dynamic_cast<StructDecl*>(tmpl_decl);
    auto* cd_tmpl = dynamic_cast<ClassDecl*>(tmpl_decl);
    auto* id_tmpl = dynamic_cast<InterfaceDecl*>(tmpl_decl);
    auto* sm_tmpl = dynamic_cast<SumDecl*>(tmpl_decl);
    if (!sd_tmpl && !cd_tmpl && !id_tmpl && !sm_tmpl) {
        err_ts("`" + base + "` is not a generic type", spec);
    }

    // Register an in-progress handle before the body is resolved. A generic that mentions itself
    // (`Node<T>* next`, `Box(const ref Box<T>)`) then resolves to this handle instead of hitting the
    // recursion guard; only a *different* instance of the same generic is a real recursive expansion
    // (the depth limit below still catches it). The handle is filled in by the branch that builds the
    // type, so every pointer to it stays valid.
    TypePtr in_progress = std::make_shared<Type>();
    in_progress->kind = sm_tmpl ? TypeKind::Sum
                        : (sd_tmpl ? TypeKind::Struct
                                   : (cd_tmpl ? TypeKind::Class : TypeKind::Interface));
    in_progress->unit = tmpl_unit;
    in_progress->name = display;
    in_progress->targs = arg_types;  // structural matching for inference (SPEC 3.15.3)
    in_progress->in_progress = true;
    generic_insts_[key] = in_progress;
    struct PendingGuard {
        std::unordered_map<std::string, TypePtr>& map;
        const std::string& key;
        ~PendingGuard() {
            auto it = map.find(key);
            if (it != map.end() && it->second && it->second->in_progress) map.erase(it);
        }
    } pending_guard{generic_insts_, key};
    const std::vector<TypeParam>& tps =
        sm_tmpl ? sm_tmpl->type_params
                : (sd_tmpl ? sd_tmpl->type_params
                           : (cd_tmpl ? cd_tmpl->type_params : id_tmpl->type_params));
    // Every diagnostic raised below now carries the use site; the "why only now" explanation rides
    // along separately and is attached only when the error is about a missing capability.
    InstNoteGuard inst_note(
        instantiation_notes_, instantiation_hints_,
        "  while instantiating `" + display + "` (requested at " +
            (u ? u->filename : std::string()) + ":" + std::to_string(spec->line) + ":" +
            std::to_string(spec->col) + ")",
        unchecked_hint(tps));
    instantiated_templates_.insert(tmpl_decl);
    if (tps.size() != spec->args.size()) {
        err_ts("generic `" + base + "` expects " + std::to_string(tps.size()) +
                   " type argument(s) but got " + std::to_string(spec->args.size()),
               spec);
    }
    if (id_tmpl) {
        // Generic interface inheritance (`interface IList<T> : ISeq<T>`) is resolved from the
        // substituted base specs below, like any other instantiated interface.
    } else if (sd_tmpl) {
        // Generic structs support const members and interfaces like any other struct; the clone is
        // substituted and resolved below.
    } else if (cd_tmpl) {
        // Generic classes support const members; see below.
    }
    // generic sum types have no extra restrictions: the alternatives are plain payload types

    Unit* tmpl_unit_ptr = nullptr;
    auto info_it = infos_.find(tmpl_unit);
    if (info_it != infos_.end()) tmpl_unit_ptr = info_it->second.unit;
    if (!tmpl_unit_ptr) tmpl_unit_ptr = u;

    // Constraint check (U34 M3b); the argument types were resolved above (for the instance name).
    for (size_t i = 0; i < tps.size(); ++i) {
        check_bound(arg_types[i], tps[i], u, nullptr);
    }

    // Clone + substitute
    std::unique_ptr<Decl> clone = clone_decl(*tmpl_decl);
    TypeBindings bindings;
    for (size_t i = 0; i < tps.size(); ++i) {
        bindings[tps[i].name] = TypeBinding{spec->args[i].get(), arg_types[i]};
    }
    subst_decl(*clone, bindings);
    clone->type_params.clear();
    clone->is_template = false;

    auto* sd = dynamic_cast<StructDecl*>(clone.get());
    auto* cd = dynamic_cast<ClassDecl*>(clone.get());
    auto* id = dynamic_cast<InterfaceDecl*>(clone.get());
    auto* sm = dynamic_cast<SumDecl*>(clone.get());
    if (sd) {
        sd->name = display;
    } else if (cd) {
        cd->name = display;
    } else if (sm) {
        sm->name = display;
    } else {
        id->name = display;
    }

    // ---------------- generic sums: `type Option<T> = Some(T) | None;` instantiates like a struct ----------------
    if (sm) {
        TypePtr ty = in_progress;  // filled in below; the handle was registered for self-reference
        sm->ty = ty;
        Unit* saved_cur = cur_;
        cur_ = tmpl_unit_ptr;
        if (sm->alts.size() < 2) {
            err_ts("variant `" + display + "` needs at least two alternatives", spec);
        }
        std::vector<std::string> names;
        std::vector<std::vector<TypePtr>> fields;
        std::vector<TypePtr> payload;
        std::unordered_set<std::string> seen;
        for (auto& alt : sm->alts) {
            if (!seen.insert(alt.name).second) {
                err_ts("duplicate alternative `" + alt.name + "` in variant `" + display + "`", spec);
            }
            std::vector<TypePtr> fts;
            for (auto& fs : alt.fields) {
                TypePtr ft = resolve_type(fs.get(), tmpl_unit_ptr);
                if (is_void(ft)) err_ts("payload of alternative `" + alt.name + "` cannot be `void`", spec);
                // The in-progress handle is the same object a self-reference resolves to, so identity
                // catches the infinitely large payload; a pointer to the sum stays legal.
                if (ft.get() == ty.get()) {
                    err_ts("payload of alternative `" + alt.name + "` would make `" + display +
                               "` infinitely large (store a pointer or a class handle instead)",
                           spec);
                }
                // A payload may own a resource as long as it can be copied or moved: a copyable value
                // is copied into the variant (and the variant is then copyable), a move-only value is
                // transferred with `Some(move v)` and moved out by `match move`. Only a value that is
                // neither (both `nocopy` and `nomove`) cannot be stored by value.
                if (!ft->copyable() && !ft->movable()) {
                    // This is a property of the instantiation (the type argument), not of the
                    // template declaration: report it at the use site, not in the library file
                    // (`cur_` is the template unit here, for resolving the payload types).
                    cur_ = u;
                    err_ts("payload of alternative `" + alt.name + "` is `" + ft->str() +
                               "`, which is neither copyable nor movable and cannot be stored by value "
                               "in a variant (store a pointer or a class handle instead)",
                           spec);
                }
                fts.push_back(ft);
            }
            TypePtr pt;
            if (fts.size() == 1) {
                pt = fts[0];
            } else if (fts.size() > 1) {
                pt = Type::make_struct(tmpl_unit, "$alt$" + display + "$" + alt.name);
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
        TypePtr uni = Type::make_struct(tmpl_unit, "$payload$" + display);
        uni->is_union = true;
        for (size_t i = 0; i < payload.size(); ++i) {
            if (!payload[i]) continue;
            Field f;
            f.name = "a" + std::to_string(i);
            f.type = payload[i];
            uni->fields.push_back(std::move(f));
        }
        if (uni->fields.empty()) {
            Field f;
            f.name = "none";
            f.type = Type::make_prim("byte");
            uni->fields.push_back(std::move(f));
        }
        TypePtr layout = Type::make_struct(tmpl_unit, "$sum$" + display);
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
        cur_ = saved_cur;
        ty->sum_names = std::move(names);
        ty->sum_fields = std::move(fields);
        ty->sum_payload = std::move(payload);
        ty->sum_layout = std::move(layout);
        generic_insts_[key] = ty;
        ty->in_progress = false;
        infos_[tmpl_unit].sums[display] = sm;
        generic_decls_.push_back(std::move(clone));
        return ty;
    }

    // ---------------- generic interfaces: used only for constraints (M5a), no value type is produced ----------------
    if (id) {
        TypePtr ity = in_progress;  // the handle registered for self-reference
        Unit* saved_cur = cur_;
        TypePtr saved_class = cur_class_;
        cur_ = tmpl_unit_ptr;
        cur_class_ = ity;  // make `Self` resolve to the interface type

        ity->methods.clear();
        // Base interfaces: `id` is the substituted clone, so its base specs already carry the
        // concrete arguments (`Base<T>` became `Base<int>`).
        ity->interfaces.clear();
        for (size_t i = 0; i < id->bases.size(); ++i) {
            TypePtr bt = (i < id->base_specs.size() && id->base_specs[i])
                             ? resolve_type(id->base_specs[i].get(), tmpl_unit_ptr)
                             : nullptr;
            if (!bt) {
                err_ts("base `" + id->bases[i] + "` of `" + display + "` cannot be resolved", spec);
            }
            if (bt->kind != TypeKind::Interface) {
                err_ts("an interface can only inherit from an interface (`" + display + "`)", spec);
            }
            for (auto& prev : ity->interfaces) {
                if (same_type(prev, bt)) {
                    err_ts("interface `" + display + "` inherits `" + bt->str() + "` more than once", spec);
                }
            }
            ity->interfaces.push_back(bt);
        }
        for (auto& m : id->methods) {
            // `ref T` returns matter here: a generic interface that declares a (ref) subscript must
            // keep it, otherwise the contract promises a value and the borrow is silently dropped.
            // The shared resolver also applies the `const`-method rule (#66).
            resolve_method_ret(m.get(), tmpl_unit_ptr);
            m->pty.clear();
            for (auto& p : m->params) {
                m->pty.push_back(decay_param_type(resolve_type(p->spec.get(), tmpl_unit_ptr)));
            }
            MethodSig sig;
            sig.name = m->name;
            sig.is_const = m->is_const;
            sig.ret = m->rty;
            sig.params = m->pty;
            for (auto& prm : m->params) {
                sig.params_ref.push_back(prm->by_ref);
                sig.params_cref.push_back(prm->by_ref && prm->is_const);
            }
            sig.is_abstract = !m->body;
            sig.is_override = m->is_override;
            sig.is_public = true;
            sig.owner = ity;
            sig.is_operator = m->is_operator;
            sig.ret_self = m->ret && m->ret->is_self;
            for (auto& p : m->params) sig.params_self.push_back(p->spec && p->spec->is_self);
            if (m->is_operator) init_interface_operator(m.get(), sig, display);
            ity->methods.push_back(std::move(sig));
            m->cname = method_cname(tmpl_unit, display, m->name, method_decl_symbol_params(m.get()),
                                m->is_const);
        }
        cur_class_ = saved_class;
        cur_ = saved_cur;

        id->ty = ity;
        generic_insts_[key] = ity;
        infos_[tmpl_unit].interfaces[display] = id;

        // Default method body check (same context save/restore as other types)
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
                direct_init_expr_ = saved_direct;
            cur_ = saved_unit;
        }

        generic_decls_.push_back(std::move(clone));
        ity->in_progress = false;
        return ity;
    }

    const bool is_struct = sd != nullptr;
    TypePtr ty = in_progress;  // the handle registered for self-reference
    if (is_struct) {
        ty->is_packed = sd->is_packed;
        ty->is_union = sd->is_union;
        ty->nocopy = sd->nocopy;
    }

    Unit* saved_cur = cur_;
    cur_ = tmpl_unit_ptr;

    // Bases: at most one class (which must come first) plus any number of interfaces. A base may be an
    // instantiated generic (`class Src<T> : IEnumerable<T>`), so every entry goes through
    // `resolve_type` — the display-name string cannot produce `IEnumerable<int>`.
    if (is_struct && !sd->base_specs.empty()) {
        // A struct lists interfaces only (static satisfaction, SPEC 3.15.7). The substituted specs
        // (`IIterator<T>` became `IIterator<int>`) resolve like any type; the required members are
        // checked and defaults materialized after the method table is built.
        for (size_t i = 0; i < sd->base_specs.size(); ++i) {
            if (!sd->base_specs[i]) continue;
            TypePtr bt = resolve_type(sd->base_specs[i].get(), tmpl_unit_ptr);
            const std::string spelled = i < sd->bases.size() ? sd->bases[i] : (bt ? bt->str() : "?");
            if (!bt || bt->kind != TypeKind::Interface) {
                err_ts("`" + spelled + "`: only interfaces are allowed after `:` on a struct", spec);
            }
            for (auto& prev : ty->interfaces) {
                if (same_type(prev, bt)) {
                    err_ts("struct `" + display + "` implements interface `" + bt->str() +
                               "` more than once",
                           spec);
                }
            }
            ty->interfaces.push_back(bt);
        }
    }
    if (!is_struct && !cd->base_specs.empty()) {
        for (size_t i = 0; i < cd->base_specs.size(); ++i) {
            if (!cd->base_specs[i]) continue;
            TypePtr bt = resolve_type(cd->base_specs[i].get(), tmpl_unit_ptr);
            const std::string spelled = i < cd->bases.size() ? cd->bases[i] : bt->str();
            if (bt->kind == TypeKind::Class) {
                if (i != 0) err_ts("a class may have at most one base class, and it must come first", spec);
                ty->base = bt;
                for (const auto& bf : bt->fields) ty->fields.push_back(bf);
            } else if (bt->kind == TypeKind::Interface) {
                for (auto& prev : ty->interfaces) {
                    if (same_type(prev, bt)) {
                        err_ts("class `" + display + "` implements interface `" + bt->str() +
                                   "` more than once",
                               spec);
                    }
                }
                ty->interfaces.push_back(bt);
            } else {
                err_ts("`" + spelled + "` is not a class or interface", spec);
            }
        }
    }

    // Fields (struct defaults to public; class follows field_visibility, private by default)
    const auto& fields = is_struct ? sd->fields : cd->fields;
    const auto& field_vis = is_struct ? sd->field_visibility : cd->field_visibility;
    for (auto& f : fields) {
        TypePtr ft = resolve_type(f.second.get(), tmpl_unit_ptr);
        bool pub = is_struct;
        auto vit = field_vis.find(f.first);
        if (vit != field_vis.end()) pub = vit->second;
        // `weak` is a property of the field spec, not of the type: the instantiation must carry it over,
        // otherwise a `weak` field silently becomes an owning reference (the target is kept alive and
        // never nulled). Same rules as the non-generic path in `resolve_class_fields`.
        if (f.second->weak && is_struct) {
            err_ts("`weak` can only be used on class fields", f.second.get());
        }
        if (f.second->weak && !is_class_type(ft)) {
            err_ts("`weak` field must be a class reference (interfaces do not support `weak` yet)",
                   f.second.get());
        }
        // A union has no tag: releasing it cannot know which member is active, so no member may own
        // a resource (SPEC 3.3.2). The non-generic path checks this too; the instantiation resolves a
        // concrete field type, so the check belongs here.
        if (sd && sd->is_union && ft->needs_cleanup()) {
            err_ts("field `" + f.first + "` of union `" + display + "` cannot be `" + ft->str() +
                       "`: union members cannot hold values that need reference counting",
                   spec);
        }
        Field fld;
        fld.name = f.first;
        fld.type = ft;
        fld.weak = f.second->weak;
        fld.is_public = pub;
        ty->fields.push_back(std::move(fld));
    }
    if (sd && sd->is_union && ty->fields.empty()) {
        err_ts("union `" + display + "` must have at least one member", spec);
    }

    // `promote` is resolved against the instantiated field table (the field index is the same as in
    // the template, but the field type is the concrete argument).
    if (is_struct) {
        if (!sd->promotes.empty()) resolve_promotes(sd->promotes, ty, sd, &sd->fields);
    } else {
        if (!cd->promotes.empty()) resolve_promotes(cd->promotes, ty, cd, &cd->fields);
    }

    // Type-level constants (`const int K = 3;` inside the body): the substituted specs resolve against
    // the instance's concrete type arguments, exactly like the non-generic path.
    resolve_const_members(is_struct ? sd->const_members : cd->const_members, ty, tmpl_unit_ptr,
                          is_struct ? (sd->is_union ? "union" : "struct") : "class", display);

    // Method / constructor / destructor signatures
    const auto& methods = is_struct ? sd->methods : cd->methods;
    ty->methods.clear();
    for (auto& m : methods) {
        // `ref T` returns must survive instantiation: resolving only the spec turns the borrow back
        // into a plain value, so every call copies and writes to that copy are lost.
        if (m->ret) resolve_method_ret(m.get(), tmpl_unit_ptr);
        m->pty.clear();
        for (auto& p : m->params) {
            m->pty.push_back(decay_param_type(resolve_type(p->spec.get(), tmpl_unit_ptr)));
        }
        MethodSig sig;
        sig.name = m->name;
        sig.ret = m->rty;
        sig.params = m->pty;
        for (auto& prm : m->params) {
            sig.params_ref.push_back(prm->by_ref);
            sig.params_cref.push_back(prm->by_ref && prm->is_const);
        }
        sig.is_abstract = !m->body;
        sig.is_override = m->is_override;
        // The non-generic path sets this in sema_decl.cpp; without it a generic class's `virtual`
        // method became static after instantiation and a subclass's `override` was rejected.
        sig.is_virtual = m->is_virtual || m->is_override || !m->body;
        sig.is_public = m->is_public;
        // const is part of the signature: without it the const overload of an instantiated class is
        // dropped as a duplicate and a const receiver resolves to the non-const one.
        sig.is_const = m->is_const;
        sig.owner = ty;
        sig.is_operator = m->is_operator;
        ty->methods.push_back(sig);
        m->cname = method_cname(tmpl_unit, display, m->name, method_decl_symbol_params(m.get()),
                                m->is_const);
        if (!m->body) ty->is_abstract = true;
    }

    const auto& ctors = is_struct ? sd->ctors : cd->ctors;
    ty->ctors.clear();
    ty->has_copy_ctor = false;
    ty->has_move_ctor = false;
    // The same synthesized zero-argument constructor the non-generic path adds (SPEC 3.7): a template with
    // default member initializers and no constructor of its own must apply them per instance.
    if (ctors.empty() && !(is_struct ? sd->field_default : cd->field_default).empty()) {
        auto synthesized = std::make_unique<CtorDecl>();
        synthesized->line = is_struct ? sd->line : cd->line;
        synthesized->col = is_struct ? sd->col : cd->col;
        synthesized->is_public = true;
        synthesized->is_synthesized = true;
        ty->implicit_ctor = true;
        if (is_struct) {
            sd->ctors.push_back(std::move(synthesized));
        } else {
            cd->ctors.push_back(std::move(synthesized));
        }
    }
    for (auto& ctor : ctors) {
        ctor->pty.clear();
        for (auto& p : ctor->params) {
            TypePtr pt = resolve_type(p->spec.get(), tmpl_unit_ptr);
            // The copy/move constructors are identified by their parameter marker, with the same rules
            // as the non-generic path (sema_resolve_types.cpp): `move` on a constructor parameter is
            // the move constructor `T(move T other)` (struct only), and `ref` is allowed only as the
            // copy constructor `T(const ref T other)`. A `move` parameter of any other shape (the
            // common "take ownership of the argument" constructor) is an error, exactly as in a
            // non-generic type.
            if (p->by_move) {
                if (!is_struct) {
                    err("`class` is a reference type: it has no move or copy constructor, so `move` "
                               "parameters are only meaningful on a struct",
                           p.get());
                }
                if (ctor->params.size() != 1 || !same_type(pt, ty)) {
                    err("the move constructor of struct `" + display + "` must be written `" +
                               display + "(move " + display + " other)` (one parameter of the struct's "
                               "own type)",
                           p.get());
                }
                if (ty->has_move_ctor) {
                    err("struct `" + display + "` already declares a move constructor", p.get());
                }
                ctor->is_move = true;
                ty->has_move_ctor = true;
            }
            if (p->by_ref) {
                if (is_struct) {
                    if (ctor->params.size() != 1 || !same_type(pt, ty)) {
                        err("a constructor may take a `ref` parameter only as the copy constructor `" +
                                   display + "(const ref " + display + " other)`",
                               p.get());
                    }
                    if (ty->has_copy_ctor) {
                        err("struct `" + display + "` already declares a copy constructor", p.get());
                    }
                    ctor->is_copy = true;
                    ty->has_copy_ctor = true;
                }
            } else {
                pt = decay_param_type(pt);
            }
            ctor->pty.push_back(pt);
        }
        ctor->cname = ctor->is_move ? move_ctor_cname(tmpl_unit, display)
                    : ctor->is_copy ? copy_ctor_cname(tmpl_unit, display)
                                    : ctor_cname(tmpl_unit, display, ctor->pty);
        ty->ctors.push_back(ctor->pty);
        ty->ctor_param_names.push_back(ctor_param_names(ctor.get()));
        ty->ctor_is_public.push_back(ctor->is_public);
    }

    DtorDecl* dtor = is_struct ? sd->dtor.get() : cd->dtor.get();
    ty->has_dtor = dtor != nullptr;
    if (dtor) dtor->cname = dtor_cname(tmpl_unit, display);
    if (is_struct) {
        sd->ty = ty;
        // Interface satisfaction and default materialization, in the template unit (the substituted
        // base specs may name types only that unit imports). A missing member is reported here, at
        // the instantiation, like the non-generic path.
        apply_struct_iface_defaults(sd);
    } else {
        cd->ty = ty;
    }
    cur_ = saved_cur;
    generic_insts_[key] = ty;
    if (is_struct) {
        infos_[tmpl_unit].structs[display] = sd;
    } else {
        infos_[tmpl_unit].classes[display] = cd;
    }

    // Check method bodies (same context save/restore as check_struct / check_class)
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
        direct_init_expr_ = saved_direct;
        cur_ = saved_unit;
    }

    // The declaration itself is moved only after check_struct, so pointers stay valid during the check
    generic_decls_.push_back(std::move(clone));
    ty->in_progress = false;
    return ty;
}

FuncDecl* Sema::instantiate_func(Call* c, const std::string& base, Unit* u,
                                 const std::vector<TypeSpecPtr>& targs) {
    std::string want_unit;
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
    if (tmpl->type_params.size() != targs.size()) {
        err("generic function `" + name + "` expects " + std::to_string(tmpl->type_params.size()) +
                " type argument(s) but got " + std::to_string(targs.size()),
            c);
    }

    // Constraint check (U34 M3b): structural matching, argument types resolved first.
    // The argument types come from the call site, so they are resolved there; the **bounds** are
    // declared in the template's unit and may name types only that unit imports (`T: IOrder` from
    // `std.interfaces`), so they are resolved there instead of blaming the caller.
    Unit* tmpl_unit_ptr = u;
    if (auto it = infos_.find(tmpl_unit); it != infos_.end() && it->second.unit) {
        tmpl_unit_ptr = it->second.unit;
    }
    std::vector<TypePtr> arg_types;
    for (const auto& ta : targs) arg_types.push_back(resolve_type(ta.get(), u));
    for (size_t i = 0; i < tmpl->type_params.size(); ++i) {
        check_bound(arg_types[i], tmpl->type_params[i], tmpl_unit_ptr, c);
    }

    std::string display = name + "<";
    for (size_t i = 0; i < targs.size(); ++i) {
        if (i) display += ", ";
        display += instance_label(arg_types[i], targs[i].get(), tmpl_unit);
    }
    display += ">";

    std::string key = tmpl_unit + "." + display;
    auto cached = generic_func_insts_.find(key);
    if (cached != generic_func_insts_.end()) return cached->second;
    if (generic_busy_.count(key)) {
        err("recursive generic instantiation of `" + display + "`", c);
    }
    if (generic_instance_count_ >= kGenericInstanceLimit) {
        err("this program needs more than " + std::to_string(kGenericInstanceLimit) +
                " generic instantiations (the last is `" + display +
                "`); SPEC 3.15.4 requires a finite expansion, so simplify the type arguments",
            c);
    }
    // The chain of instantiations is bounded as well: `f0<T>` calling `f1<Box<T>>` … recurses through
    // `instantiate_generic_func` once per link, so an unbounded chain overflows the compiler's stack
    // (#107, #91). The type path counts the same budget.
    if (generic_depth_ >= kGenericDepthLimit) {
        err("recursive generic instantiation of `" + base + "` does not terminate (more than " +
                std::to_string(kGenericDepthLimit) +
                " nested instances; SPEC 3.15.4 requires a finite expansion)",
            c);
    }
    ++generic_instance_count_;
    struct FnDepthGuard {
        int& d;
        explicit FnDepthGuard(int& x) : d(x) { ++d; }
        ~FnDepthGuard() { --d; }
    } fn_depth(generic_depth_);
    generic_busy_.insert(key);
    BusyGuard busy_guard{generic_busy_, key};
    // The instantiation chain + "why only now" hint for an unchecked parameter.
    InstNoteGuard inst_note(
        instantiation_notes_, instantiation_hints_,
        "  while instantiating `" + display + "` at " + (u ? u->filename : std::string()) + ":" +
            std::to_string(c->line) + ":" + std::to_string(c->col),
        unchecked_hint(tmpl->type_params));
    instantiated_templates_.insert(tmpl);

    std::unique_ptr<Decl> clone = clone_decl(*tmpl);
    auto* fn = static_cast<FuncDecl*>(clone.get());
    TypeBindings bindings;
    for (size_t i = 0; i < tmpl->type_params.size(); ++i) {
        bindings[tmpl->type_params[i].name] = TypeBinding{targs[i].get(), arg_types[i]};
    }
    subst_decl(*fn, bindings);
    fn->type_params.clear();
    fn->is_template = false;
    fn->name = display;

    // Register before checking: recursive calls inside the body (`f<T>`) must hit the cache
    generic_func_insts_[key] = fn;
    generic_decls_.push_back(std::move(clone));

    // Save the current check context: check_func resets scopes_ / cur_func_ and friends,
    // and instantiation may happen inside another function body (nested call).
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
    Expr* saved_direct = direct_init_expr_;
    Unit* saved_cur = cur_;

    Unit* tu = u;
    auto info_it = infos_.find(tmpl_unit);
    if (info_it != infos_.end()) tu = info_it->second.unit;
    cur_ = tu;

    auto restore_context = [&] {
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
        direct_init_expr_ = saved_direct;
        cur_ = saved_cur;
    };

    try {
        // Instantiated functions bypass resolve_unit_types, so their signatures are resolved here
        fn->rty = resolve_type(fn->ret.get(), tu);
        fn->pty.clear();
        fn->pty_ref.clear();
        fn->pty_cref.clear();
        for (auto& p : fn->params) {
            TypePtr pt = resolve_type(p->spec.get(), tu);
            if (!p->by_ref) pt = decay_param_type(pt);
            // The `ref`-legality rule (a borrow may not alias a value that owns something) has to be
            // re-applied here: a type parameter is resolved only at instantiation, and without this check
            // the call compiled and then corrupted the heap (the value at the borrow was reference-counted
            // twice, or a slice handle was atomically bumped in place).
            if (p->by_ref && !borrowable_type(pt)) {
                std::string why = "it holds a reference-counted field or has a destructor";
                if (pt && pt->kind == TypeKind::Slice) why = "a managed sequence is a reference-counted handle";
                else if (pt && pt->kind == TypeKind::Struct && pt->is_union) why = "a union cannot be borrowed";
                err("instantiating `" + fn->name + "` with `" + (pt ? pt->str() : "void") +
                        "` makes parameter `" + p->name + "` a `ref` borrow, and that type cannot be borrowed: " +
                        why,
                    p.get());
            }
            fn->pty.push_back(pt);
            fn->pty_ref.push_back(p->by_ref);
            fn->pty_cref.push_back(p->by_ref && p->is_const);
        }

        check_func(fn);
        if (fn->cname.empty()) fn->cname = func_cname(tmpl_unit, fn);
    } catch (...) {
        // A diagnostic must not leave a half-checked instance cached or the caller's check context
        // displaced: the compiler may keep going (up to the error cap) and check more declarations.
        restore_context();
        generic_func_insts_.erase(key);
        throw;
    }

    restore_context();
    return fn;
}


}  // namespace muc_detail