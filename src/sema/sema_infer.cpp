// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Type-argument inference for a generic free function call (`max2(1, 2)`, SPEC 3.15.3).
//
// owns:      infer_func_type_args — binding the template's type parameters by structural matching of
//            the argument types against the parameter patterns, and the three "cannot infer" diagnostics
// not:       instantiation (sema_generic.cpp), constraint satisfaction (sema_constraint.cpp), parsing,
//            method/constructor/type-context inference (not supported)
// pass:      from check_call, when a bare call names a generic function and no type argument was written
// concept:   generics, inference, calls
//
// The declared parameter is a *pattern* (sema: `T`, `T*`, `T[]`, `Vec<T>`, `(T) => T`), the argument has
// one concrete type. Matching the pattern against it top-down binds each type parameter on its first
// occurrence and reports a conflict when a later one disagrees. Nothing here is speculative: every
// failure has a diagnostic of its own, and the resulting argument list is handed to the ordinary
// instantiation path, so there is exactly one way an instance is built.
#include <functional>
#include <string>
#include <vector>

#include "sema_impl.hpp"

namespace muc_detail {

namespace {

enum class Match {
    Ok,
    Shape,     // the argument does not have the structure the pattern describes
    Conflict,  // the same type parameter is bound to two different types
};

// Index of `name` among the template's type parameters, or -1.
int param_index(const std::vector<TypeParam>& tps, const std::string& name) {
    for (size_t i = 0; i < tps.size(); ++i) {
        if (tps[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

// Indices of the type parameters the pattern mentions, in order of first appearance.
void collect_params(const TypeSpec* s, const std::vector<TypeParam>& tps, std::vector<int>& out) {
    if (!s) return;
    if (s->kind == TypeSpec::K::Named && s->args.empty() && !s->is_self) {
        const int k = param_index(tps, s->name);
        if (k >= 0 && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
        return;
    }
    for (const auto& a : s->args) collect_params(a.get(), tps, out);
    collect_params(s->elem.get(), tps, out);
    for (const auto& p : s->params) collect_params(p.get(), tps, out);
    collect_params(s->ret.get(), tps, out);
}

// "`T`" / "`T` and `U`" / "`T`, `U` and `V`" — the message of a multi-parameter pattern.
std::string param_list_text(const std::vector<TypeParam>& tps, const std::vector<int>& idx) {
    std::string out;
    for (size_t i = 0; i < idx.size(); ++i) {
        if (i) out += (i + 1 == idx.size()) ? " and " : ", ";
        out += "`" + tps[static_cast<size_t>(idx[i])].name + "`";
    }
    return out;
}

// Does `at` come from the generic whose instances are named `base<...>`? An unqualified pattern is
// accepted only when the instance's own unit declares a template of that name, so `Box<T>` written in
// one unit never binds against another unit's `Box<int>`.
bool instance_matches(const std::string& spec_name, const TypePtr& at,
                      const std::unordered_map<std::string, Decl*>& templates) {
    std::string want_unit, base;
    split_qualified(spec_name, want_unit, base);
    if (at->name.size() <= base.size() || at->name.compare(0, base.size(), base) != 0) return false;
    if (at->name[base.size()] != '<') return false;
    if (!want_unit.empty()) return at->unit == want_unit;
    return templates.count(at->unit + "." + base) != 0;
}

}  // namespace

std::vector<TypeSpecPtr> Sema::infer_func_type_args(Call* c, FuncDecl* tmpl, const std::string& name,
                                                    const std::string& note) {
    const std::vector<TypeParam>& tps = tmpl->type_params;
    if (!c->named.empty()) {
        err("function calls do not support named arguments (only struct construction does)", c);
    }
    // An arity mismatch is reported as such: a parameter with no argument would otherwise show up as a
    // type parameter that "cannot be inferred", which names the wrong problem.
    if (tmpl->is_varargs ? c->args.size() < tmpl->params.size()
                         : c->args.size() != tmpl->params.size()) {
        err(std::string(tmpl->is_varargs ? "variadic function `" : "function `") + name + "` expects " +
                (tmpl->is_varargs ? "at least " : "") + std::to_string(tmpl->params.size()) +
                " arguments, but " + std::to_string(c->args.size()) + " were given",
            c);
    }

    std::vector<TypePtr> bound(tps.size());
    std::vector<size_t> bound_at(tps.size(), 0);  // which argument bound it (for the conflict message)
    const TypeSpec* bad_pat = nullptr;
    int conflict_param = -1;
    TypePtr conflict_ty;  // the type that disagreed (the nested part, not the whole argument)
    size_t current_arg = 0;

    std::function<Match(const TypeSpec&, const TypePtr&)> match = [&](const TypeSpec& pat,
                                                                     const TypePtr& at) -> Match {
        if (!at) {
            bad_pat = &pat;
            return Match::Shape;
        }
        switch (pat.kind) {
            case TypeSpec::K::Named: {
                if (pat.is_self) {
                    bad_pat = &pat;
                    return Match::Shape;
                }
                if (pat.args.empty()) {
                    const int k = param_index(tps, pat.name);
                    if (k < 0) return Match::Ok;  // a concrete type: its mismatch belongs to the argument check
                    // A value that has no type of its own to instantiate with: `null` is convertible to
                    // many types, and a range is not a storable value, so neither says what `T` is.
                    if (at->kind == TypeKind::Null || at->kind == TypeKind::Range ||
                        at->kind == TypeKind::Module) {
                        bad_pat = &pat;
                        return Match::Shape;
                    }
                    const size_t ki = static_cast<size_t>(k);
                    if (!bound[ki]) {
                        bound[ki] = at;
                        bound_at[ki] = current_arg;
                        return Match::Ok;
                    }
                    // `const` is a view of the same type here: `T` bound to `int` also takes a `const int` argument.
                    if (same_type_sig(bound[ki], at)) return Match::Ok;
                    conflict_param = k;
                    conflict_ty = at;
                    return Match::Conflict;
                }
                // A generic instantiation used as a pattern: `Vec<T>` against `Vec<int>`.
                if (at->kind != TypeKind::Struct && at->kind != TypeKind::Class &&
                    at->kind != TypeKind::Sum && at->kind != TypeKind::Interface) {
                    bad_pat = &pat;
                    return Match::Shape;
                }
                if (at->targs.size() != pat.args.size() || !instance_matches(pat.name, at, generic_templates_)) {
                    bad_pat = &pat;
                    return Match::Shape;
                }
                for (size_t i = 0; i < pat.args.size(); ++i) {
                    const Match m = match(*pat.args[i], at->targs[i]);
                    if (m != Match::Ok) return m;
                }
                return Match::Ok;
            }
            case TypeSpec::K::Ptr:
                // A one-dimensional static array decays to a pointer to its first element (C99 6.3.2.1),
                // so `T* p` accepts `int[4]` with `T = int`; the argument check performs the decay.
                if (at->kind == TypeKind::Array && pat.elem && at->elem &&
                    at->elem->kind != TypeKind::Array) {
                    return match(*pat.elem, at->elem);
                }
                if (at->kind != TypeKind::Ptr || !pat.elem) {
                    bad_pat = &pat;
                    return Match::Shape;
                }
                return match(*pat.elem, at->elem);
            case TypeSpec::K::Slice:
                // The rank (`T[]` / `T[,]`) is part of the sequence type, so it has to agree. A static
                // array of the same rank converts to the slice (`T[N] -> T[]`, SPEC 3.6), so it binds the
                // element type here; the argument check performs the conversion.
                if (at->kind == TypeKind::Array && pat.elem) {
                    const TypePtr* cur = &at;
                    int depth = 0;
                    while ((*cur)->kind == TypeKind::Array && depth < pat.rank && (*cur)->elem) {
                        cur = &(*cur)->elem;
                        ++depth;
                    }
                    if (depth != pat.rank || (*cur)->kind == TypeKind::Array) {
                        bad_pat = &pat;
                        return Match::Shape;
                    }
                    return match(*pat.elem, *cur);
                }
                if (at->kind != TypeKind::Slice || at->rank != pat.rank || !pat.elem) {
                    bad_pat = &pat;
                    return Match::Shape;
                }
                return match(*pat.elem, at->elem);
            case TypeSpec::K::Array:
                // The length is not inferred (it is a constant expression, not a type), only the element.
                if (at->kind != TypeKind::Array || !pat.elem) {
                    bad_pat = &pat;
                    return Match::Shape;
                }
                return match(*pat.elem, at->elem);
            case TypeSpec::K::Func:
            case TypeSpec::K::Delegate: {
                // A `delegate` parameter accepts a plain function value too: a non-capturing function
                // converts to a delegate, and the argument was typed before its target was known.
                const bool ok = pat.kind == TypeSpec::K::Func ? at->kind == TypeKind::Func
                                                              : is_callable_value(at);
                if (!ok || pat.params.size() != at->fparams.size()) {
                    bad_pat = &pat;
                    return Match::Shape;
                }
                for (size_t i = 0; i < pat.params.size(); ++i) {
                    const Match m = match(*pat.params[i], at->fparams[i]);
                    if (m != Match::Ok) return m;
                }
                return pat.ret ? match(*pat.ret, at->fret) : Match::Ok;
            }
        }
        bad_pat = &pat;
        return Match::Shape;
    };

    for (size_t i = 0; i < c->args.size(); ++i) {
        Expr* a = c->args[i].get();
        current_arg = i;
        // The argument is typed on its own: no parameter type is known before the parameters are bound,
        // and the ordinary argument check runs again once the instance exists.
        TypePtr at = check_expr(a);
        const Param* p = i < tmpl->params.size() ? tmpl->params[i].get() : nullptr;
        if (!p) continue;  // `...`: extra arguments carry no parameter type
        if (!at || is_void(at)) {
            std::string what = at ? std::string("`void`") : std::string("no type");
            err("`" + name + "`: cannot infer the type arguments from argument " +
                    std::to_string(i + 1) + ": it has " + what +
                    ", which matches no parameter type" + note,
                a);
        }
        const Match m = match(*p->spec, at);
        if (m == Match::Conflict) {
            const size_t k = static_cast<size_t>(conflict_param);
            const std::string first = bound[k]->str();
            const std::string second = conflict_ty ? conflict_ty->str() : at->str();
            if (bound_at[k] == i) {
                // Both positions are inside this one argument: `Pair<T, T>` against `Pair<int, long>`.
                err("`" + name + "`: the type argument `" + tps[k].name + "` is inferred as `" + first +
                        "` from one part of argument " + std::to_string(i + 1) + " and as `" + second +
                        "` from another; write the type argument explicitly (`" + name + "<...>(...)`)" + note,
                    a);
            }
            err("`" + name + "`: the type argument `" + tps[k].name + "` inferred from argument " +
                    std::to_string(bound_at[k] + 1) + " is `" + first + "`, but argument " +
                    std::to_string(i + 1) + " gives `" + second +
                    "`; write the type argument explicitly (`" + name + "<...>(...)`) to choose one" + note,
                a);
        }
        if (m == Match::Shape) {
            std::vector<int> mentioned;
            collect_params(p->spec.get(), tps, mentioned);
            // A pattern with no type parameter is a plain type mismatch: the ordinary argument check
            // reports it, with the parameter name and the declared type.
            if (mentioned.empty()) continue;
            err("`" + name + "`: cannot infer the type argument" + (mentioned.size() == 1 ? "" : "s") +
                    " " + param_list_text(tps, mentioned) + " from argument " + std::to_string(i + 1) +
                    ": `" + at->str() + "` does not match the parameter type `" +
                    spec_text(bad_pat ? bad_pat : p->spec.get()) + "`" +
                    (p->name.empty() ? std::string() : " (`" + p->name + "`)") + note,
                a);
        }
    }

    // A parameter no argument could determine: only an explicit type argument can supply it.
    for (size_t k = 0; k < tps.size(); ++k) {
        if (bound[k]) continue;
        bool in_params = false;
        for (const auto& p : tmpl->params) {
            std::vector<int> mentioned;
            collect_params(p->spec.get(), tps, mentioned);
            if (std::find(mentioned.begin(), mentioned.end(), static_cast<int>(k)) != mentioned.end()) {
                in_params = true;
                break;
            }
        }
        err("`" + name + "`: cannot infer the type argument `" + tps[k].name + "` from the arguments: " +
                (in_params ? "no argument determines it"
                           : "no parameter mentions it, so only the return type could, and the return "
                             "type is not used for inference") +
                "; write `" + name + "<...>(...)`" + note,
            c->callee.get());
    }

    // The inferred type must satisfy the template's constraints. This runs before the instantiation so
    // the diagnostic can say the type was *inferred* — the reader did not choose it and must not go
    // looking for a mistake of their own.
    for (size_t k = 0; k < tps.size(); ++k) {
        const TypeParam& tp = tps[k];
        if (tp.bounds.empty()) continue;
        std::string why;
        if (tp.op == TypeParam::Op::Or) {
            bool any = false;
            for (const auto& b : tp.bounds) {
                if (satisfies_one(bound[k], b.get(), cur_, nullptr)) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                err("`" + name + "`: the type argument `" + tp.name + "` inferred from argument " +
                        std::to_string(bound_at[k] + 1) + " is `" + bound[k]->str() +
                        "`, which does not satisfy any alternative bound of the constraint; write `" +
                        name + "<...>(...)` to choose the type explicitly" + note,
                    c->args[bound_at[k]].get());
            }
            continue;
        }
        for (const auto& b : tp.bounds) {
            if (satisfies_one(bound[k], b.get(), cur_, &why)) continue;
            err("`" + name + "`: the type argument `" + tp.name + "` inferred from argument " +
                    std::to_string(bound_at[k] + 1) + " is `" + bound[k]->str() +
                    "`, which does not satisfy the constraint `" + canon_spec(*b) + "`: " + why +
                    "; write `" + name + "<...>(...)` to choose the type explicitly" + note,
                c->args[bound_at[k]].get());
        }
    }

    // Hand the bindings to the ordinary instantiation path: the instance is built from this list
    // exactly as if the arguments had been written at the call site. They are not stored on the call
    // node — a node inside a template body is checked once per instance, and a concrete list written
    // into it would be copied into every later clone.
    std::vector<TypeSpecPtr> inferred;
    for (size_t k = 0; k < tps.size(); ++k) {
        auto spec = std::make_unique<TypeSpec>();
        spec->kind = TypeSpec::K::Named;
        spec->name = bound[k]->str();
        spec->line = c->line;
        spec->col = c->col;
        spec->resolved = bound[k];
        inferred.push_back(std::move(spec));
    }
    return inferred;
}

}  // namespace muc_detail
