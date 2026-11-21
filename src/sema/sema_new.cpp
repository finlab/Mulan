// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// `new` and constructor arguments (definitions of Sema members).
//
// owns:      `check_new` (allocation, ctor dispatch, dynamic arrays) and `check_ctor_args`
//            (overload choice and named arguments), plus the named-argument helpers
// not:       ordinary call resolution (sema_call.cpp), expression typing, codegen
// pass:      body checking
// concept:   new, constructors, named-arguments

#include "sema_impl.hpp"

#include <unordered_map>
#include <unordered_set>

namespace muc_detail {

    // Named arguments for a constructor call: the names must be exactly the parameter names of the
    // chosen overload (each once). The arguments are moved into positional order before the usual
    // type checks, so codegen only ever sees positional arguments.
    bool named_args_match(const std::vector<std::string>& params,
                          const std::vector<std::pair<std::string, ExprPtr>>& named) {
        if (params.size() != named.size()) return false;
        std::unordered_set<std::string> want(params.begin(), params.end());
        if (want.size() != params.size()) return false;
        std::unordered_set<std::string> seen;
        for (auto& [n, e] : named) {
            if (!want.count(n)) return false;
            if (!seen.insert(n).second) return false;
        }
        return true;
    }

    void reorder_named_args(std::vector<ExprPtr>& args,
                            std::vector<std::pair<std::string, ExprPtr>>& named,
                            const std::vector<std::string>& params) {
        std::unordered_map<std::string, size_t> idx;
        for (size_t i = 0; i < params.size(); ++i) idx[params[i]] = i;
        std::vector<ExprPtr> ordered(params.size());
        for (auto& [n, e] : named) ordered[idx[n]] = std::move(e);
        named.clear();
        args = std::move(ordered);
    }

    // `new T[n]` default-constructs every element (agents/value-semantics.md §7). A struct that declares
    // constructors but no parameterless one has nothing to call, so the allocation is rejected — C++
    // refuses the same way. A struct with a parameterless constructor owns its own member step, so its
    // fields are not inspected; anything without user constructors recurses through fields and elements.
    static TypePtr missing_default_ctor(const TypePtr& t) {
        if (!t) return nullptr;
        if (t->kind == TypeKind::Struct) {
            if (t->has_default_ctor()) return nullptr;
            if (!t->ctors.empty()) return t;
            for (const auto& f : t->fields) {
                if (f.weak || !f.type) continue;
                if (TypePtr bad = missing_default_ctor(f.type)) return bad;
            }
            return nullptr;
        }
        if (t->kind == TypeKind::Array) return missing_default_ctor(t->elem);
        return nullptr;
    }


    TypePtr Sema::check_new(NewExpr* ne) {        TypePtr t = resolve_type(ne->type.get(), cur_);
        if (ne->is_array) {
            if (is_void(t)) err("dynamic array element cannot be void", ne);
            if (ne->dims.empty()) err("dynamic array is missing its length", ne);
            for (auto& d : ne->dims) {
                TypePtr ct = check_expr(d.get());
                if (!(ct && ct->is_integer())) {
                    err("dynamic array length must be an integer", d.get());
                }
                // A constant negative length is caught here rather than at run time (SPEC 4);
                // the runtime guard in emitSliceAlloc stays as the backstop for computed lengths.
                long long n = 0;
                if (eval_const_int(d.get(), n) && n < 0) {
                    err("dynamic array length must not be negative", d.get());
                }
            }
            // A struct with a destructor is a legal element: destruction is implemented (`dtor_glue`
            // runs per element) and the allocation zeroes the block, which the language guarantees is a
            // valid destructible state (SPEC 3.13 zero-value safety). Element construction runs the
            // default constructor of every element (ctor_glue, agents/value-semantics.md §7).
            if (TypePtr bad = missing_default_ctor(t)) {
                err("`new T[n]` default-constructs every element, but struct `" + bad->str() +
                        "` has no parameterless constructor",
                    ne);
            }
            // `uninit` leaves the element block undefined, so the elements must not need the zero
            // value to be valid: a reference-holding or destructible element would corrupt RC.
            if (ne->uninit &&
                (has_ref_fields(t) ||
                    ((t->kind == TypeKind::Struct || t->kind == TypeKind::Array) && t->needs_dtor()))) {
                err("`uninit` requires a trivial element type: `" + t->str() +
                        "` holds a reference or has a destructor",
                    ne);
            }
            if (!ne->args.empty() || !ne->named.empty()) {
                err("dynamic arrays cannot take constructor arguments", ne);
            }
            ne->class_ty = t;
            if (ne->jagged_rank > 0) {
                if (ne->dims.size() != 1) {
                    err("a jagged array `new T[m][]` creates one dimension at a time; "
                        "fill rows in a loop for two dimensions, or use `new T[m,n]` for a rectangular array",
                        ne);
                }
                TypePtr row = t;
                for (int i = 0; i < ne->jagged_rank; ++i) row = Type::make_slice(row);
                ne->elem_ty = row;  // element of the outer array: T[]/T[][]/...
                return Type::make_slice(row);
            }
            if (type_align(t) > 8) err_slice_elem_align(t, ne);
            ne->elem_ty = t;
            return Type::make_slice(t, false, static_cast<int>(ne->dims.size()));
        }
        if (t->kind != TypeKind::Class) err("`new` can only be used with a class or a dynamic array", ne);
        if (t->is_abstract) {
            std::string missing;
            for (auto& mm : t->missing_members) {
                if (!missing.empty()) missing += ", ";
                missing += "`" + mm + "`";
            }
            err("abstract class `" + t->str() + "` cannot be instantiated with `new`" +
                    (missing.empty() ? "" : ": it does not implement " + missing),
                ne);
        }
        const auto& ctors = t->ctors;
        int named_hit = -1;
        if (!ne->named.empty()) {
            // Same rule as `C(...)`: named arguments select the overload whose parameter names they
            // spell out, then become positional arguments.
            if (ctors.empty()) {
                err("class `" + t->str() + "` declares no constructor, so `new` takes no arguments", ne);
            }
            int hit = -1;
            int hits = 0;
            for (size_t i = 0; i < ctors.size(); ++i) {
                const auto& pn = t->ctor_param_names.size() > i ? t->ctor_param_names[i]
                                                                : std::vector<std::string>{};
                if (named_args_match(pn, ne->named)) {
                    if (hit < 0) hit = static_cast<int>(i);
                    hits += 1;
                }
            }
            if (hits == 0) {
                err("no constructor of `" + t->str() +
                        "` has parameters matching the named arguments (each parameter name must be given "
                        "exactly once)",
                    ne);
            }
            if (hits > 1) {
                err("named arguments match more than one constructor of `" + t->str() +
                        "`; use positional arguments",
                    ne);
            }
            reorder_named_args(ne->args, ne->named, t->ctor_param_names[hit]);
            named_hit = hit;
        }
        ne->has_ctor = !ctors.empty();
        if (ctors.empty()) {
            if (!ne->args.empty()) err("class `" + t->str() + "` has no matching constructor", ne);
        } else {
            const auto* chosen = named_hit >= 0
                                     ? &ctors[static_cast<size_t>(named_hit)]
                                     : choose_ctor(ctors, raw_args(ne->args), "class `" + t->str() + "`", ne);
            const size_t idx = static_cast<size_t>(chosen - ctors.data());
            if (idx < t->ctor_is_public.size() && !t->ctor_is_public[idx] &&
                (!cur_ || cur_->name != t->unit)) {
                err("constructor is not declared public and cannot be invoked with `new` from another unit", ne);
            }
            for (size_t i = 0; i < ne->args.size(); ++i) {
                ArgGuard ag(call_arg_depth_);
                check_assign(ne->args[i].get(), (*chosen)[i], "constructor argument");
            }
            ne->ctor_pty = *chosen;
        }
        ne->class_ty = t;
        return t;
    }

    void Sema::check_ctor_args(Call* c, const TypePtr& ty) {
        const auto& fields = ty->fields;

        // A cross-unit `S(...)` construction requires all fields to be public
        if (cur_ && cur_->name != ty->unit) {
            for (auto& f : fields) {
                if (!f.is_public) {
                    err("field `" + f.name + "` of struct `" + ty->str() +
                            "` is not declared public and cannot be constructed from another unit",
                        c);
                }
            }
        }

        if (c->args.empty() && c->named.empty()) {
            c->field_order.clear();
            return;
        }

        if (!c->named.empty()) {
            std::unordered_map<std::string, size_t> fmap;
            for (size_t i = 0; i < fields.size(); ++i) fmap.emplace(fields[i].name, i);

            std::vector<int> order;
            std::unordered_set<std::string> seen;
            for (auto& [n, e] : c->named) {
                auto it = fmap.find(n);
                if (it == fmap.end()) {
                    err("struct `" + ty->str() + "` has no field `" + n + "`", c);
                }
                if (!seen.insert(n).second) err("field `" + n + "` is initialized more than once", c);
                const size_t idx = it->second;
                order.push_back(static_cast<int>(idx));
                check_assign(e.get(), fields[idx].type, "construction of `" + ty->str() + "`");
            }
            if (order.size() != fields.size()) {
                err("construction of `" + ty->str() + "` must initialize every field", c);
            }
            c->field_order = std::move(order);
            return;
        }

        if (c->args.size() != fields.size()) {
            err("construction of `" + ty->str() + "` expects " + std::to_string(fields.size()) +
                    " arguments, but " + std::to_string(c->args.size()) + " were given",
                c);
        }
        c->field_order.clear();
        for (size_t i = 0; i < fields.size(); ++i) {
            check_assign(c->args[i].get(), fields[i].type, "construction of `" + ty->str() + "`");
            c->field_order.push_back(static_cast<int>(i));
        }
    }

}  // namespace muc_detail
