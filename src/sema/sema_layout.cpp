// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Type layout: sizes, alignment and the cv views (definitions of Sema members).
//
// owns:      `check_layouts` (size/align of every placed type, `packed`/`align`, C ABI shape), the
//            instantiated-layout check and the `const` view refresh
// not:       filling in fields or resolving signatures (sema_resolve_types.cpp), codegen layout
// pass:      after every declaration is resolved, before any body
// concept:   layout

#include "sema_impl.hpp"

#include "directive.hpp"

namespace muc_detail {

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
                // A by-value cycle has no finite layout: `type_size` stops at the repeat and reports it
                // through the cycle flag. It used to surface later as "field is not a fixed-size type"
                // in codegen, or as a compiler stack overflow when the cycle ran through a union.
                clear_type_depth_exceeded();
                (void)type_size(ty);
                if (type_cycle_detected()) {
                    err(kw + " `" + sd->name + "` contains itself by value (the cycle is `" +
                            type_cycle_name() +
                            "`); break the cycle up with pointers or a class handle", sd);
                }
                if (type_depth_exceeded()) {
                    err(kw + " `" + sd->name +
                            "` nests more than 512 levels by value; break the chain up with pointers or "
                            "an interface",
                        sd);
                }
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

        // `sizeof` of a sum goes through its synthesized layout (tag + payload union), so a payload at the
        // very edge of the by-value nesting limit silently produced a wrong size; the synthesized layout
        // is validated here. A sum that contains itself by value (directly or through another sum) has no
        // finite layout at all and is rejected as a cycle.
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto sd = dynamic_cast<SumDecl*>(d.get());
                if (!sd || !sd->ty) continue;
                clear_type_depth_exceeded();
                (void)type_size(sd->ty);
                if (type_cycle_detected()) {
                    err("sum `" + sd->name + "` contains itself by value (the cycle is `" +
                            type_cycle_name() +
                            "`); store a pointer or a class handle instead",
                        sd);
                } else if (type_depth_exceeded()) {
                    err("sum `" + sd->name +
                            "` nests more than 512 levels by value; break the chain up with pointers or "
                            "an interface",
                        sd);
                }
            }
        }
    }

    // A generic instance is a synthesized struct that no unit's declaration list contains, and it is
    // created while bodies are checked — i.e. after `check_layouts`. An instance whose by-value nesting is
    // one level past the layout limit used to pass every check and surface as an LLVM layout mismatch, so
    // the instances are validated here, before the errors are reported.
    void Sema::check_instance_layouts() {
        for (auto& d : generic_decls_) {
            auto sd = dynamic_cast<StructDecl*>(d.get());
            if (!sd || !sd->ty) continue;
            clear_type_depth_exceeded();
            (void)type_size(sd->ty);
            const std::string kw = sd->is_union ? "union" : "struct";
            if (type_cycle_detected()) {
                err(kw + " `" + sd->name + "` contains itself by value (the cycle is `" +
                        type_cycle_name() + "`); break the cycle up with pointers or an interface",
                    sd);
            } else if (type_depth_exceeded()) {
                err(kw + " `" + sd->name +
                        "` nests more than 512 levels by value; break the chain up with pointers or an "
                        "interface",
                    sd);
            }
        }
    }


    void Sema::refresh_cv_views() {
        for (auto& [key, entry] : cv_views_) {
            const TypePtr& canon = entry.first;
            if (!canon) continue;
            for (auto& weak : entry.second) {
                TypePtr view = weak.lock();  // a view that was a temporary is already gone
                if (!view) continue;
                const bool keep_const = view->is_const;
                *view = *canon;
                view->is_const = keep_const;
            }
        }
        cv_views_.clear();
    }

}  // namespace muc_detail
