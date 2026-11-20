// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// The Sema entry point: construction and the pass order (definitions of Sema members).
//
// owns:      `Sema::Sema` and `Sema::run` — the only place the passes are sequenced; the order and the
//            reason for it are documented in agents/code-map.md §2
// not:       any checking itself; each step is a function defined in another sema_*.cpp
// pass:      the driver of every pass
// concept:   sema-entry, pass-order

#include "sema_impl.hpp"

#include "directive.hpp"

namespace muc_detail {

Sema::Sema(std::vector<std::unique_ptr<Unit>> units, std::string entry, bool single_thread,
           bool strict)
    : units_(std::move(units)), entry_(std::move(entry)), single_thread_(single_thread),
      strict_(strict) {}

Program Sema::run() {
    collect();
    prune_unreachable_units();
    place_structs();
    for (auto& u : units_) resolve_unit_types(u.get());
    // A `const T` view is a copy of the type node (cv lives on the node), taken while T may still have
    // been a placeholder; now that every shape is final, refresh each view from its type.
    refresh_cv_views();
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
            } else if (auto cl = dynamic_cast<ClassDecl*>(d.get())) {
                recover([&] { apply_class_iface_defaults(cl); });
            }
        }
    }
    // `extend I with J;` (SPEC 3.18): materialize `J`'s defaults into the implementors of `I` before
    // any body is checked, exactly like the interface defaults above (and after them, so a type's own
    // or interface-provided method keeps precedence).
    apply_interface_extensions();
    for (auto& u : units_) check_unit(u.get());
    // A constrained generic body is checked at its definition (build 156), after every unit's own
    // bodies: the constraint types must be fully resolved by then.
    check_generic_contracts();
    check_instance_layouts();
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
    // AST nodes point into the cache and codegen runs after this Sema is gone.
    prog.synthesized_sigs = std::move(op_sig_cache_);
    return prog;
}

}  // namespace muc_detail
