// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Overload ranking and the method/operator lookup walks (definitions of Sema members).
//
// owns:      `rank_arg` / `dominates` / `choose_overload` / `choose_method` / `choose_ctor`, and the
//            walks that produce candidates: `collect_methods`, `collect_class_methods`,
//            `filter_methods_for_receiver`, `collect_operators`, `find_member_operator`
// not:       the call forms themselves (sema_call.cpp), constraint checking (sema_generic.cpp)
// pass:      body checking, called from the call checker
// concept:   overload-resolution, method-lookup, operator-lookup

#include "sema_impl.hpp"

#include "ast_clone.hpp"

namespace muc_detail {

    int Sema::rank_arg(Expr* e, const TypePtr& aty, const TypePtr& p) {
        if (!p || !aty) return -1;
        // Implicit struct construction is the weakest match: exact > widening > implicit conversion.
        auto implicit_rank = [&]() { return find_implicit_conv(p, aty) ? 2 : -1; };
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (same_type_sig(aty, p)) return 0;
            if (is_numeric(p) && promote_int_literal(il->value, p)) return 1;
            return implicit_rank();
        }
        if (auto fl = dynamic_cast<FloatLit*>(e)) {
            (void)fl;
            if (same_type_sig(aty, p)) return 0;
            if (p->kind == TypeKind::Prim && (p->name == "float" || p->name == "double")) return 1;
            return implicit_rank();
        }
        if (dynamic_cast<NullLit*>(e)) {
            if (p->kind == TypeKind::Ptr || p->kind == TypeKind::Slice ||
                p->kind == TypeKind::Class || p->kind == TypeKind::Interface) {
                return 0;
            }
            return implicit_rank();
        }
        if (same_type_sig(aty, p)) return 0;
        if (implicit_ok(aty, p)) return 1;
        return implicit_rank();
    }

    bool Sema::dominates(const std::vector<int>& a, const std::vector<int>& b) {
        bool strict = false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] > b[i]) return false;
            if (a[i] < b[i]) strict = true;
        }
        return strict;
    }


    std::vector<FuncDecl*> Sema::collect_operators(const std::string& sym,
                                             const std::vector<TypePtr>& operands) {
        std::unordered_set<std::string> scope;
        if (cur_) {
            scope.insert(cur_->name);
            for (auto& u : cur_->uses) scope.insert(u->unit_name);
        }
        for (auto& t : operands) {
            if (t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Class ||
                      t->kind == TypeKind::Enum || t->kind == TypeKind::Interface)) {
                if (!t->unit.empty()) scope.insert(t->unit);
            }
        }

        std::vector<FuncDecl*> out;
        const std::string name = "operator" + sym;
        for (auto& entry : infos_) {
            if (!scope.empty() && !scope.count(entry.first)) continue;
            auto it = entry.second.funcs.find(name);
            if (it != entry.second.funcs.end()) {
                out.insert(out.end(), it->second.begin(), it->second.end());
            }
        }
        return out;
    }

    // Every function named `name` that is visible from the current unit: the current unit's own
    // functions, then the public ones of `use`d units (a local function fully shadows imports), plus
    // `std.string`'s public functions as the implicit import.
    std::vector<FuncDecl*> Sema::collect_funcs_named(const std::string& name) {
        std::vector<FuncDecl*> cands;
        auto add_funcs = [&](UnitInfo& o, bool require_pub) {
            auto f = o.funcs.find(name);
            if (f == o.funcs.end()) return;
            for (auto* fd : f->second) {
                if (!require_pub || fd->is_public) cands.push_back(fd);
            }
        };
        add_funcs(info(), false);
        if (cands.empty()) {
            for (auto& use : cur_->uses) {
                if (use->unit_name == "std.string") continue;  // added below uniformly as an implicit import
                if (!unit_visible(use->unit_name)) continue;
                auto it = infos_.find(use->unit_name);
                if (it != infos_.end()) add_funcs(it->second, true);
            }
            if (UnitInfo* stdn = std_info(); stdn && cur_->name != "std.string") {
                add_funcs(*stdn, true);
            }
        }
        return cands;
    }

    std::vector<Expr*> Sema::raw_args(const std::vector<ExprPtr>& v) {
        std::vector<Expr*> out;
        out.reserve(v.size());
        for (auto& e : v) out.push_back(e.get());
        return out;
    }

    FuncDecl* Sema::choose_overload(const std::vector<FuncDecl*>& cands,
                              const std::vector<Expr*>& args, const std::string& what,
                              Node* at, bool check_ref) {
        std::vector<TypePtr> atys;
        // One candidate: each parameter type is the target of its argument, so an argument that needs
        // its target to have a type of its own (a bare sum alternative, `Some(3)`) is checked there
        // instead of on its own (SPEC 10.1.1 `target type is already known`).
        const FuncDecl* only = cands.size() == 1 ? cands[0] : nullptr;
        for (size_t i = 0; i < args.size(); ++i) {
            atys.push_back(check_expr(args[i], only && i < only->pty.size() ? only->pty[i] : TypePtr()));
        }

        std::vector<FuncDecl*> ok;
        std::vector<std::vector<int>> ranks;
        size_t ref_reject = static_cast<size_t>(-1);
        for (auto* f : cands) {
            if (f->is_varargs ? f->pty.size() > args.size() : f->pty.size() != args.size()) continue;
            std::vector<int> r;
            bool good = true;
            for (size_t i = 0; i < f->pty.size(); ++i) {
                // A `ref` parameter is feasible only for an addressable argument of exactly the
                // declared type: no conversion, no temporaries, and a mutable borrow never binds a
                // readonly one. This mirrors `check_call`; without it an operator call would accept
                // a temporary and the callee would hold a dangling borrow.
                if (check_ref && i < f->pty_ref.size() && f->pty_ref[i]) {
                    Expr* a = args[i];
                    if (!is_lvalue(a) && !is_borrow_expr(a)) {
                        good = false;
                        ref_reject = i;
                        break;
                    }
                    if (!same_type_sig(atys[i], f->pty[i])) { good = false; break; }
                    const bool is_cref = i < f->pty_cref.size() && f->pty_cref[i];
                    if (!is_cref) {
                        if (root_is_const(a)) { good = false; break; }
                        if (auto* bc = dynamic_cast<Call*>(a); bc && bc->by_ref && bc->ref_const) {
                            good = false;
                            break;
                        }
                        if (auto* bi = dynamic_cast<Index*>(a); bi && bi->by_ref && bi->ref_const) {
                            good = false;
                            break;
                        }
                        if (auto* bb = dynamic_cast<Binary*>(a); bb && bb->by_ref && bb->ref_const) {
                            good = false;
                            break;
                        }
                    }
                    r.push_back(0);
                    continue;
                }
                int k = rank_arg(args[i], atys[i], f->pty[i]);
                if (k < 0) { good = false; break; }
                r.push_back(k);
            }
            if (good) { ok.push_back(f); ranks.push_back(std::move(r)); }
        }
        if (ok.empty()) {
            if (ref_reject != static_cast<size_t>(-1)) {
                err("argument " + std::to_string(ref_reject + 1) + " of `" + what +
                        "` must be an addressable variable: a `ref` parameter borrows the caller's storage",
                    at);
            }
            if (cands.size() == 1 && cands[0]->pty.size() == args.size()) {
                for (size_t i = 0; i < args.size(); ++i) {
                    check_assign(args[i], cands[0]->pty[i], what + " argument");
                }
            }
            err("no matching overload of " + what, at);
        }

        int best = -1;
        for (size_t i = 0; i < ok.size(); ++i) {
            bool unique = true;
            for (size_t j = 0; j < ok.size(); ++j) {
                if (i == j) continue;
                if (dominates(ranks[j], ranks[i]) || ranks[j] == ranks[i]) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                if (best >= 0) { best = -1; break; }
                best = static_cast<int>(i);
            }
        }
        if (best < 0) err("ambiguous overload call of `" + what + "`", at);
        return ok[static_cast<size_t>(best)];
    }

    void Sema::collect_class_methods(const TypePtr& t, const std::string& name,
                               std::vector<const MethodSig*>& out, int depth) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (m.name != name) continue;
            bool dup = false;
            for (auto* e : out) {
                if (same_sig(*e, m)) { dup = true; break; }
            }
            if (!dup) out.push_back(&m);
        }
        if (t->kind == TypeKind::Class && t->base) {
            collect_class_methods(t->base, name, out, depth + 1);
        }
    }

    void Sema::filter_methods_for_receiver(std::vector<const MethodSig*>& ms, bool recv_const) {
        if (ms.empty()) return;
        if (recv_const) {
            std::vector<const MethodSig*> kept;
            for (auto* s : ms) {
                if (s->is_const) kept.push_back(s);
            }
            ms = std::move(kept);
            return;
        }
        // A mutable receiver prefers the non-const overload (C++ rules); the const overload stays
        // callable only when there is no non-const candidate at all.
        bool has_nonconst = false;
        for (auto* s : ms) {
            if (!s->is_const) {
                has_nonconst = true;
                break;
            }
        }
        if (!has_nonconst) return;
        std::vector<const MethodSig*> kept;
        for (auto* s : ms) {
            if (!s->is_const) kept.push_back(s);
        }
        ms = std::move(kept);
    }

    void Sema::collect_methods(const TypePtr& t, const std::string& name,
                         std::vector<const MethodSig*>& out, int depth) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (m.name != name) continue;
            bool dup = false;
            for (auto* e : out) {
                if (same_sig(*e, m)) { dup = true; break; }
            }
            if (!dup) out.push_back(&m);  // derived class / first visitor wins, overriding same-signature entries from the base class
        }
        if (t->kind == TypeKind::Class && t->base) {
            collect_methods(t->base, name, out, depth + 1);
        }
        for (auto& i : t->interfaces) {
            collect_methods(i, name, out, depth + 1);
        }
    }

    const MethodSig* Sema::find_member_operator(const TypePtr& recv, const std::string& name,
                                                const std::vector<Expr*>& args, Node* at) {
        // Inside a default body of interface I, `this` has type I, but a contract operator declared as
        // `T operator[](Self c, isize i)` still applies to it: the receiver is `Self`, and the body is
        // materialized per implementor, where the concrete operator is resolved. Without this branch
        // `this[0]` / `this + x` would be unusable in a default that is exactly where they belong.
        if (recv && recv->kind == TypeKind::Interface && cur_class_ && same_type(cur_class_, recv)) {
            // The member form drops the receiver, exactly as for a class/struct member operator, so the
            // overload choice runs on a copy without the first parameter. The copies are owned by the
            // Sema so the selected signature outlives this call (the expression keeps the pointer).
            std::vector<const MethodSig*> ok;
            for (const auto& m : recv->methods) {
                if (!m.is_operator || m.name != name) continue;
                if (m.params_self.empty() || !m.params_self[0]) continue;  // the receiver must be `Self`
                auto s = std::make_unique<MethodSig>(m);
                s->params.erase(s->params.begin());
                if (!s->params_self.empty()) s->params_self.erase(s->params_self.begin());
                if (s->params.size() != args.size()) continue;
                bool good = true;
                for (size_t i = 0; i < args.size(); ++i) {
                    TypePtr at2 = args[i]->ty ? args[i]->ty : check_expr(args[i]);
                    if (rank_arg(args[i], at2, s->params[i]) < 0) {
                        good = false;
                        break;
                    }
                }
                if (!good) continue;
                ok.push_back(s.get());
                op_sig_cache_.push_back(std::move(s));
                // A contract operator applied to an interface-typed receiver needs the implementor type:
                // the default that contains it can only be materialized, never dispatched through the
                // itable (see MethodDecl::body_self).
                member_used_this_ = true;
            }
            if (ok.empty()) return nullptr;
            return choose_method(ok, args, "`" + name + "` of `" + recv->str() + "`", at);
        }
        // Member operators live on structs (borrowed `this`, static dispatch) and on classes (the
        // receiver is the handle, also static unless the operator is declared `virtual`). `==`/`!=` on a
        // class stay identity comparison and cannot be declared as members — the parser rejects them.
        if (!recv || (recv->kind != TypeKind::Struct && recv->kind != TypeKind::Class)) return nullptr;
        std::vector<const MethodSig*> cands;
        for (const auto& m : recv->methods) {
            if (!m.is_operator || m.name != name) continue;
            cands.push_back(&m);
        }
        if (cands.empty()) return nullptr;
        for (const MethodSig* c : cands) {
            if (!decl_visible(c->owner ? c->owner->unit : std::string(), c->is_public)) {
                err("operator `" + name + "` of `" + recv->str() + "` is not declared public", at);
            }
        }
        // Only take over when an overload actually accepts the arguments: `-s` and `s - t` share one symbol,
        // so a non-matching member must fall back to the free-function lookup instead of erroring here.
        std::vector<TypePtr> atys;
        for (auto* a : args) atys.push_back(a->ty ? a->ty : check_expr(a));
        std::vector<const MethodSig*> ok;
        for (const MethodSig* c : cands) {
            if (c->params.size() != args.size()) continue;
            bool good = true;
            for (size_t i = 0; i < args.size(); ++i) {
                if (rank_arg(args[i], atys[i], c->params[i]) < 0) {
                    good = false;
                    break;
                }
            }
            if (good) ok.push_back(c);
        }
        if (ok.empty()) return nullptr;
        return choose_method(ok, args, "`" + name + "` of `" + recv->str() + "`", at);
    }

    const MethodSig* Sema::choose_method(const std::vector<const MethodSig*>& cands,
                                   const std::vector<Expr*>& args,
                                   const std::string& what, Node* at) {
        std::vector<TypePtr> atys;
        // Same target-typing rule as `choose_overload`: a lone candidate's parameter types type their
        // arguments.
        const MethodSig* only = cands.size() == 1 ? cands[0] : nullptr;
        for (size_t i = 0; i < args.size(); ++i) {
            atys.push_back(
                check_expr(args[i], only && i < only->params.size() ? only->params[i] : TypePtr()));
        }

        std::vector<const MethodSig*> ok;
        std::vector<std::vector<int>> ranks;
        for (auto* sig : cands) {
            if (sig->params.size() != args.size()) continue;
            std::vector<int> r;
            bool good = true;
            for (size_t i = 0; i < args.size(); ++i) {
                int k = rank_arg(args[i], atys[i], sig->params[i]);
                if (k < 0) { good = false; break; }
                r.push_back(k);
            }
            if (good) { ok.push_back(sig); ranks.push_back(std::move(r)); }
        }
        if (ok.empty()) err("no matching overload of " + what, at);

        int best = -1;
        for (size_t i = 0; i < ok.size(); ++i) {
            bool unique = true;
            for (size_t j = 0; j < ok.size(); ++j) {
                if (i == j) continue;
                if (dominates(ranks[j], ranks[i]) || ranks[j] == ranks[i]) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                if (best >= 0) { best = -1; break; }
                best = static_cast<int>(i);
            }
        }
        if (best < 0) err("ambiguous overload call of `" + what + "`", at);
        return ok[static_cast<size_t>(best)];
    }

    const std::vector<TypePtr>* Sema::choose_ctor(const std::vector<std::vector<TypePtr>>& ctors,
                                            const std::vector<Expr*>& args,
                                            const std::string& what, Node* at) {
        std::vector<TypePtr> atys;
        // Same target-typing rule as `choose_overload`: a lone candidate's parameter types type their
        // arguments.
        const std::vector<TypePtr>* only = ctors.size() == 1 ? &ctors[0] : nullptr;
        for (size_t i = 0; i < args.size(); ++i) {
            atys.push_back(check_expr(args[i], only && i < only->size() ? (*only)[i] : TypePtr()));
        }

        std::vector<const std::vector<TypePtr>*> ok;
        std::vector<std::vector<int>> ranks;
        for (auto& cs : ctors) {
            if (cs.size() != args.size()) continue;
            std::vector<int> r;
            bool good = true;
            for (size_t i = 0; i < args.size(); ++i) {
                int k = rank_arg(args[i], atys[i], cs[i]);
                if (k < 0) { good = false; break; }
                r.push_back(k);
            }
            if (good) { ok.push_back(&cs); ranks.push_back(std::move(r)); }
        }
        if (ok.empty()) err("no matching constructor for " + what, at);

        int best = -1;
        for (size_t i = 0; i < ok.size(); ++i) {
            bool unique = true;
            for (size_t j = 0; j < ok.size(); ++j) {
                if (i == j) continue;
                if (dominates(ranks[j], ranks[i]) || ranks[j] == ranks[i]) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                if (best >= 0) { best = -1; break; }
                best = static_cast<int>(i);
            }
        }
        if (best < 0) err("ambiguous constructor call of `" + what + "`", at);
        return ok[static_cast<size_t>(best)];
    }


}  // namespace muc_detail
