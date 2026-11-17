// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Generic monomorphization instantiation (U34, M2).
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
    InstNoteGuard(std::vector<std::string>& n, std::string text) : notes(n) {
        notes.push_back(std::move(text));
    }
    ~InstNoteGuard() { notes.pop_back(); }
    InstNoteGuard(const InstNoteGuard&) = delete;
    InstNoteGuard& operator=(const InstNoteGuard&) = delete;
};

// A type parameter is checked at the definition only when it has a conjunction of interface bounds;
// a bare `<T>` and an `A | B` union stay instantiation-time (3.15.5).
bool param_checked_at_definition(const TypeParam& tp) {
    return !tp.bounds.empty() && tp.op != TypeParam::Op::Or;
}

// `T`, `U` (only the parameters that are not checked at the definition).
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

// Attach the hint to the outermost unchecked frame only: a nested chain would otherwise repeat the
// same explanation on every line.
std::string unchecked_hint_once(const std::vector<TypeParam>& tps,
                                const std::vector<std::string>& notes) {
    for (const auto& n : notes) {
        if (n.find("unconstrained, so the definition") != std::string::npos) return {};
    }
    return unchecked_hint(tps);
}

// Type syntax → canonical display name (`[const] Box<Vec<int[]>>`), used for `Type::name` and the instance cache key.
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

// Split `a.b.C` into (a.b, C); unit is empty when there is no prefix
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
            } else if (auto sm = dynamic_cast<SumDecl*>(d.get())) {
                name = sm->name;
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
    // Every diagnostic raised below now carries the use site and, for an unchecked parameter, the
    // reason it could not be reported at the definition.
    InstNoteGuard inst_note(
        instantiation_notes_,
        "  while instantiating `" + display + "` (requested at " +
            (u ? u->filename : std::string()) + ":" + std::to_string(spec->line) + ":" +
            std::to_string(spec->col) + ")" + unchecked_hint_once(tps, instantiation_notes_));
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
        if (!sd_tmpl->const_members.empty() || !sd_tmpl->bases.empty()) {
            err_ts("generic struct `" + base + "` does not support const members or interfaces yet", spec);
        }
    } else if (cd_tmpl) {
        if (!cd_tmpl->const_members.empty()) {
            err_ts("generic class `" + base + "` does not support const members yet", spec);
        }
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
            err_ts("sum type `" + display + "` needs at least two alternatives", spec);
        }
        std::vector<std::string> names;
        std::vector<std::vector<TypePtr>> fields;
        std::vector<TypePtr> payload;
        std::unordered_set<std::string> seen;
        for (auto& alt : sm->alts) {
            if (!seen.insert(alt.name).second) {
                err_ts("duplicate alternative `" + alt.name + "` in sum type `" + display + "`", spec);
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
                if (noncopyable_struct(ft)) {
                    err_ts("payload of alternative `" + alt.name + "` is struct `" + ft->str() +
                               "`, which has a destructor and cannot be stored by value in a sum",
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
            m->rty = make_borrow_ret(m->ret.get(), m->ret_ref, m.get(), tmpl_unit_ptr,
                                     "method `" + m->name + "`");
            m->pty.clear();
            for (auto& p : m->params) {
                m->pty.push_back(decay_param_type(resolve_type(p->spec.get(), tmpl_unit_ptr)));
            }
            MethodSig sig;
            sig.name = m->name;
            sig.is_const = m->is_const;
            sig.ret = m->rty;
            sig.params = m->pty;
            sig.is_abstract = !m->body;
            sig.is_override = m->is_override;
            sig.is_public = true;
            sig.owner = ity;
            sig.is_operator = m->is_operator;
            sig.ret_self = m->ret && m->ret->is_self;
            for (auto& p : m->params) sig.params_self.push_back(p->spec && p->spec->is_self);
            if (m->is_operator) init_interface_operator(m.get(), sig, display);
            ity->methods.push_back(std::move(sig));
            m->cname = method_cname(tmpl_unit, display, m->name, m->pty);
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
        Field fld;
        fld.name = f.first;
        fld.type = ft;
        fld.weak = f.second->weak;
        fld.is_public = pub;
        ty->fields.push_back(std::move(fld));
    }

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
        sig.is_abstract = !m->body;
        sig.is_override = m->is_override;
        // The non-generic path sets this in sema_decl.cpp; without it a generic class's `virtual`
        // method became static after instantiation and a subclass's `override` was rejected.
        sig.is_virtual = m->is_virtual || m->is_override || !m->body;
        sig.is_public = m->is_public;
        sig.owner = ty;
        sig.is_operator = m->is_operator;
        ty->methods.push_back(sig);
        m->cname = method_cname(tmpl_unit, display, m->name, m->pty);
        if (!m->body) ty->is_abstract = true;
    }

    const auto& ctors = is_struct ? sd->ctors : cd->ctors;
    ty->ctors.clear();
    ty->has_copy_ctor = false;
    ty->has_move_ctor = false;
    for (auto& ctor : ctors) {
        ctor->pty.clear();
        for (auto& p : ctor->params) {
            TypePtr pt = resolve_type(p->spec.get(), tmpl_unit_ptr);
            // The copy/move constructors are identified by their parameter marker, exactly as the
            // non-generic path does (sema_decl.cpp): the symbol name differs, and the type records
            // that it is copyable/movable. Without this the instantiator emitted a plain constructor
            // that wrote the borrow's pointer into the value slot.
            if (p->by_move) {
                ctor->is_move = true;
                ty->has_move_ctor = true;
            }
            if (p->by_ref) {
                ctor->is_copy = true;
                ty->has_copy_ctor = true;
            } else {
                pt = decay_param_type(pt);
            }
            ctor->pty.push_back(pt);
        }
        ctor->cname = ctor->is_move ? move_ctor_cname(tmpl_unit, display)
                    : ctor->is_copy ? copy_ctor_cname(tmpl_unit, display)
                                    : ctor_cname(tmpl_unit, display, ctor->pty);
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

    // The declaration itself is moved only after check_struct, so pointers stay valid during the check
    generic_decls_.push_back(std::move(clone));
    ty->in_progress = false;
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

    // Constraint check (U34 M3b): structural matching, argument types resolved first
    std::vector<TypePtr> arg_types;
    for (const auto& ta : c->type_args) arg_types.push_back(resolve_type(ta.get(), u));
    for (size_t i = 0; i < tmpl->type_params.size(); ++i) {
        check_bound(arg_types[i], tmpl->type_params[i], u, c);
    }

    std::string display = name + "<";
    for (size_t i = 0; i < c->type_args.size(); ++i) {
        if (i) display += ", ";
        display += instance_label(arg_types[i], c->type_args[i].get(), tmpl_unit);
    }
    display += ">";

    std::string key = tmpl_unit + "." + display;
    auto cached = generic_func_insts_.find(key);
    if (cached != generic_func_insts_.end()) return cached->second;
    if (generic_busy_.count(key)) {
        err("recursive generic instantiation of `" + display + "`", c);
    }
    generic_busy_.insert(key);
    BusyGuard busy_guard{generic_busy_, key};
    // The instantiation chain + "why only now" hint for an unchecked parameter.
    InstNoteGuard inst_note(
        instantiation_notes_,
        "  while instantiating `" + display + "` at " + (u ? u->filename : std::string()) + ":" +
            std::to_string(c->line) + ":" + std::to_string(c->col) +
            unchecked_hint_once(tmpl->type_params, instantiation_notes_));
    instantiated_templates_.insert(tmpl);

    std::unique_ptr<Decl> clone = clone_decl(*tmpl);
    auto* fn = static_cast<FuncDecl*>(clone.get());
    TypeBindings bindings;
    for (size_t i = 0; i < tmpl->type_params.size(); ++i) {
        bindings[tmpl->type_params[i].name] = TypeBinding{c->type_args[i].get(), arg_types[i]};
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
    bool saved_super = super_ok_;
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
        super_ok_ = saved_super;
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

namespace {

bool is_cmp_op(const std::string& n) {
    return n == "operator<" || n == "operator<=" || n == "operator>" ||
           n == "operator>=" || n == "operator==" || n == "operator!=";
}

bool is_arith_op(const std::string& n) {
    return n == "operator+" || n == "operator-" || n == "operator*" ||
           n == "operator/" || n == "operator%";
}

// Find a method on `t` (including the base class chain and interface default methods) with the same name and signature as `sig`;
// positions in `sig` that come from `Self` are replaced with `self_ty`.
bool type_has_method(const TypePtr& t, const MethodSig& sig, const TypePtr& self_ty, int depth = 0) {
    if (!t || depth > 64) return false;
    // A built-in managed sequence has no declared methods: `length()` is a compiler-lowered accessor
    // (SPEC 6.4.3). Without this the most common container of all would not satisfy `IIndexable`.
    if (t->kind == TypeKind::Slice && !sig.is_operator && sig.name == "length" && sig.params.empty() &&
        sig.ret && sig.ret->kind == TypeKind::Prim &&
        (sig.ret->name == "isize" || sig.ret->name == "usize")) {
        return true;
    }
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

    // Concrete type: exact equality
    if (bt->kind != TypeKind::Interface) {
        if (same_type(t, bt)) return true;
        if (why) *why = "expected `" + bt->str() + "`";
        return false;
    }

    // Interface: collect members (including the interface inheritance chain)
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
        // Operator members: the receiver is the first operand and must be `Self` (an interface member
        // has no `this`, so that is the only way to name the implementor). The operands after it are
        // compared exactly as declared: a stream (`Self operator<<(Self b, byte c)`) or a scaled
        // arithmetic (`Self operator*(Self a, int k)`) is as legitimate as `Self op Self`. This is
        // the same asymmetry `operator[]` always had (its index is never `Self`).
        const bool is_subscript = sig->name == "operator[]" || sig->name == "operator[]=";
        if (sig->params_self.empty() || !sig->params_self[0]) {
            if (why) {
                *why = "an operator declared in an interface must spell the receiver as the first `Self` "
                       "operand (write `T " + sig->name + "(Self c, ...)`)";
            }
            return false;
        }
        std::vector<TypePtr> want;
        for (size_t i = 0; i < sig->params.size(); ++i) {
            want.push_back((i < sig->params_self.size() && sig->params_self[i]) ? t : sig->params[i]);
        }
        const TypePtr want_ret = sig->ret_self ? t : sig->ret;
        // Whether every operand is the implementor type; only the built-in arithmetic block requires
        // that (a built-in `+` has no mixed form).
        bool all_t = !want.empty();
        for (size_t i = 0; i < want.size(); ++i) {
            if (!same_type(want[i], t)) all_t = false;
        }

        // Built-in operators (note: enum does not participate in arithmetic). The built-in block is
        // `T op T`, so a contract with a different second operand (`Self * int`) is not satisfied by it.
        bool builtin = false;
        if (is_arith_op(sig->name) && t->is_numeric() && t->kind != TypeKind::Enum &&
            same_type(want_ret, t) && all_t) {
            builtin = true;
        }
        if (is_cmp_op(sig->name) &&
            (t->is_numeric() || t->kind == TypeKind::Ptr) && want_ret &&
            want_ret->is_bool()) {
            builtin = true;
        }
        // A built-in block answers the subscript itself: `T[]` with an `isize` index yields its element.
        if (is_subscript && t->kind == TypeKind::Slice && want.size() == 2 &&
            same_type(want[1], Type::make_prim("isize")) &&
            (same_type(want_ret, t->elem) || same_type(referent_of(want_ret), t->elem))) {
            builtin = true;
        }
        if (builtin) continue;

        // User-defined operators: find a free function with the same signature in a visible unit (exact match, no implicit conversion)
        bool found = false;
        auto try_unit = [&](UnitInfo& oi, bool need_pub) {
            auto it = oi.funcs.find(sig->name);
            if (it == oi.funcs.end()) return;
            for (FuncDecl* f : it->second) {
                if (need_pub && !f->is_public) continue;
                if (f->pty.size() != want.size()) continue;
                if (!same_type(f->rty, want_ret) && !same_type(referent_of(f->rty), want_ret)) continue;
                bool ok = true;
                for (size_t i = 0; i < want.size(); ++i) {
                    if (!same_type(f->pty[i], want[i])) {
                        ok = false;
                        break;
                    }
                    if (i != 0 || !sig->recv_ref) continue;
                    // A receiver declared `ref Self` must be able to mutate the caller's value: a
                    // by-value implementation would only touch a copy. A `ref const Self` receiver
                    // promises read-only use, which a by-value copy also honors.
                    const bool impl_ref = i < f->pty_ref.size() && f->pty_ref[i];
                    const bool impl_cref = i < f->pty_cref.size() && f->pty_cref[i];
                    if (!impl_ref) {
                        if (!sig->recv_cref) { ok = false; break; }
                    } else if (!sig->recv_cref && impl_cref) {
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
        // Member operator on a value type: the receiver is the implicit `this`, so the declared parameter
        // list is the interface signature minus its first operand. `this` is a writable borrow, which
        // satisfies both a by-value receiver and a mutable `ref Self` one; a `ref const Self` contract
        // is not satisfied (there are no `const` methods, so `this` could still write).
        if (!found && !(sig->recv_ref && sig->recv_cref) &&
            (t->kind == TypeKind::Struct || t->kind == TypeKind::Class)) {
            for (const auto& m : t->methods) {
                if (!m.is_operator || m.name != sig->name) continue;
                if (m.params.size() + 1 != want.size()) continue;
                // `ref T` satisfies a declared `T`: a borrow is usable as a value
                if (!same_type(m.ret, want_ret) && !same_type(referent_of(m.ret), want_ret)) continue;
                bool ok = true;
                for (size_t i = 1; i < want.size(); ++i) {
                    if (!same_type(m.params[i - 1], want[i])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) found = true;
            }
        }
        if (!found) {
            if (why) {
                std::string sigdesc;
                for (size_t i = 0; i < want.size(); ++i) {
                    if (i) sigdesc += ", ";
                    sigdesc += want[i] ? want[i]->str() : "void";
                }
                *why = "no `" + sig->name + "` accepting `(" + sigdesc + ")`";
                if (sig->recv_ref) {
                    *why += sig->recv_cref
                                ? "; a `ref const Self` receiver needs an implementation whose first "
                                  "parameter is `ref const` (there are no `const` member operators)"
                                : "; a `ref Self` receiver needs an implementation whose first parameter "
                                  "is `ref` (a member operator qualifies, its `this` is borrowed)";
                }
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

// ---------------------------------------------------------------- contract checking (build 156)
//
// A generic body is only checked when it is instantiated, i.e. against one concrete type argument at
// a time. That let a body use more than its constraint promises: `void w<T: IRead>(T c) { c[0] = 5; }`
// compiled whenever the argument type happened to return a `ref` subscript, and failed for the first
// type that returned a value. The fix is to check the body once at the definition, against a
// *witness* type that carries exactly the members the constraint declares — no more, no less.

void Sema::add_contract_members(const TypePtr& witness, const TypePtr& iface) {
    if (!iface || iface->kind != TypeKind::Interface) return;
    // An interface may list the inherited members itself (`IMutableIndexable` carries both its own
    // `ref T operator[]` and the inherited value one), so the same signature can arrive twice. Keep a
    // single entry, preferring the `ref` return: a borrow satisfies the value form, never the reverse.
    auto same_params = [](const MethodSig& a, const MethodSig& b) {
        if (a.name != b.name || a.params.size() != b.params.size()) return false;
        for (size_t i = 0; i < a.params.size(); ++i) {
            if (!same_type(a.params[i], b.params[i])) return false;
        }
        return true;
    };
    auto returns_ref = [](const MethodSig& s) { return s.ret && s.ret->kind == TypeKind::Ref; };
    for (const auto& m : iface->methods) {
        MethodSig sig = m;
        sig.owner = witness;
        sig.is_public = true;  // an interface member is visible wherever the interface is
        // `Self` positions become the witness. For an operator the first `Self` operand is the
        // receiver, and a member operator takes it as the implicit `this`, so it is dropped.
        std::vector<TypePtr> params;
        for (size_t i = 0; i < sig.params.size(); ++i) {
            const bool is_self = i < sig.params_self.size() && sig.params_self[i];
            // An operator's first `Self` is its receiver and a member operator takes it as `this`. A
            // plain method's `Self` is a parameter type (SPEC 3.15.6), so it stays.
            if (sig.is_operator && i == 0 && is_self) continue;
            params.push_back(is_self ? witness : sig.params[i]);
        }
        sig.params = params;
        sig.params_self.assign(params.size(), false);
        sig.ret = sig.ret_self ? witness : sig.ret;
        sig.ret_self = false;
        // The receiver is gone (it is the witness itself / the future `this`), so its borrow mode
        // has no meaning in the witness's member signature.
        sig.recv_ref = false;
        sig.recv_cref = false;
        bool merged = false;
        for (auto& existing : witness->methods) {
            if (!same_params(existing, sig)) continue;
            if (!returns_ref(existing) && returns_ref(sig)) existing.ret = sig.ret;
            merged = true;
            break;
        }
        if (!merged) witness->methods.push_back(sig);
    }
    for (const auto& base : iface->interfaces) add_contract_members(witness, base);
}

TypePtr Sema::make_contract_witness(const TypeParam& tp, Unit* u, const std::string& owner) {
    // Only a conjunction of interfaces can be witnessed: an unconstrained parameter grants nothing
    // (nothing to check against yet), and `A | B` has no single member set.
    if (tp.bounds.empty() || tp.op == TypeParam::Op::Or) return nullptr;
    TypePtr witness = Type::make_struct(u ? u->name : std::string(), "$witness." + owner + "." + tp.name);
    for (const auto& bound : tp.bounds) {
        TypePtr bt = resolve_type(bound.get(), u);
        if (!bt || bt->kind != TypeKind::Interface) return nullptr;
        add_contract_members(witness, bt);
    }
    return witness;
}

void Sema::warn_uninstantiated_generics() {
    for (auto& u : units_) {
        cur_ = u.get();
        for (auto& d : u->templates) {
            // Sum types have no body, and an interface's default methods are a separate story; the
            // hint is about bodies/members that are only checked when the template is used.
            std::string kind, name, what;
            if (auto* f = dynamic_cast<FuncDecl*>(d.get())) {
                kind = "function";
                name = f->name;
                what = "its body is never checked";
            } else if (auto* s = dynamic_cast<StructDecl*>(d.get())) {
                kind = "struct";
                name = s->name;
                what = "its member bodies are never checked";
            } else if (auto* c = dynamic_cast<ClassDecl*>(d.get())) {
                kind = "class";
                name = c->name;
                what = "its member bodies are never checked";
            } else {
                continue;
            }
            if (d->type_params.empty()) continue;
            const std::string names = definition_unchecked_names(d->type_params);
            if (names.empty()) continue;  // every parameter is checked at the definition
            if (instantiated_templates_.count(d.get())) continue;
            warn("generic " + kind + " `" + name + "` is never instantiated; " + names +
                     (definition_unchecked_count(d->type_params) == 1 ? " is" : " are") +
                     " unconstrained, so " + what +
                     " (add a constraint to check it at the definition)",
                 d.get());
        }
    }
}

void Sema::check_generic_contracts() {
    std::vector<std::pair<std::string, FuncDecl*>> todo;
    for (auto& kv : generic_templates_) {
        if (auto* fn = dynamic_cast<FuncDecl*>(kv.second)) {
            if (fn->type_params.empty() || contract_checked_.count(fn)) continue;
            todo.push_back({kv.first, fn});
        }
    }
    // Stable order: the template map is unordered, and a diagnostic should not depend on hash order.
    std::sort(todo.begin(), todo.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (auto& [key, fn] : todo) {
        std::string unit_name, name;
        split_qualified(key, unit_name, name);
        Unit* tmpl_unit = nullptr;
        auto info = infos_.find(unit_name);
        if (info != infos_.end()) tmpl_unit = info->second.unit;
        if (!tmpl_unit) continue;

        std::vector<TypePtr> witnesses;
        for (const auto& tp : fn->type_params) {
            TypePtr w = make_contract_witness(tp, tmpl_unit, fn->name);
            if (!w) {
                witnesses.clear();
                break;
            }
            witnesses.push_back(w);
        }
        if (witnesses.empty()) continue;  // nothing to check this one against
        contract_checked_.insert(fn);

        auto saved_bind = type_bind_;
        auto saved_cur = cur_;
        for (size_t i = 0; i < fn->type_params.size(); ++i) {
            type_bind_[fn->type_params[i].name] = witnesses[i];
        }
        cur_ = tmpl_unit;
        recover([&] {
            // A template's signature is normally resolved per instantiation; resolve it here against
            // the witnesses. The results are overwritten when a real instantiation clones the
            // template (`instantiate_func` clears and re-resolves `pty`/`rty`).
            fn->rty = resolve_type(fn->ret.get(), tmpl_unit);
            fn->pty.clear();
            fn->pty_ref.clear();
            fn->pty_cref.clear();
            for (auto& p : fn->params) {
                TypePtr pt = resolve_type(p->spec.get(), tmpl_unit);
                if (!p->by_ref) pt = decay_param_type(pt);
                fn->pty.push_back(pt);
                fn->pty_ref.push_back(p->by_ref);
                fn->pty_cref.push_back(p->by_ref && p->is_const);
            }
            check_func(fn);
        });
        cur_ = saved_cur;
        type_bind_ = std::move(saved_bind);
    }
}

}  // namespace muc_detail

