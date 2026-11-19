// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Constraint satisfaction for generic type parameters (U34, M2).
//
// owns:      satisfies_one, the method lookup behind interface bounds, the contract members a witness
//            type receives, bound checking and the never-instantiated warning
// not:       instantiation itself (sema_generic.cpp), parsing, layout
// pass:      from resolve_type / check_bound while instantiated code is checked
// concept:   generics, constraints, witnesses

#include <string>
#include <unordered_set>
#include <vector>

#include "sema_impl.hpp"

namespace muc_detail {
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
bool type_has_method(const TypePtr& t, const MethodSig& sig, const TypePtr& self_ty, int depth = 0,
                     std::unordered_set<const Type*>* seen = nullptr) {
    // The bound is the enforced hierarchy limit, not a separate 64: a valid 65-level base chain used to
    // be truncated here (a required method was reported missing) and a 65-level interface chain lost its
    // inherited requirements (#103).
    if (!t || depth > kMaxInheritanceDepth + kMaxInterfaceDepth) return false;
    std::unordered_set<const Type*> local;
    std::unordered_set<const Type*>& visited = seen ? *seen : local;
    if (!visited.insert(t.get()).second) return false;  // a diamond: each type once
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
    if (t->kind == TypeKind::Class && t->base &&
        type_has_method(t->base, sig, self_ty, depth + 1, &visited)) {
        return true;
    }
    for (const auto& i : t->interfaces) {
        if (type_has_method(i, sig, self_ty, depth + 1, &visited)) return true;
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
    std::unordered_set<const Type*> visited;
    std::function<void(const TypePtr&, int)> collect = [&](const TypePtr& itf, int depth) {
        if (!itf || depth > kMaxInterfaceDepth) return;
        if (!visited.insert(itf.get()).second) return;  // a diamond: each interface once
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
                    // by-value implementation would only touch a copy. A `const ref Self` receiver
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
        // satisfies both a by-value receiver and a mutable `ref Self` one; a `const ref Self` contract
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
                                ? "; a `const ref Self` receiver needs an implementation whose first "
                                  "parameter is `const ref` (there are no `const` member operators)"
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