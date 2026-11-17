// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// `std.comptime` validation: the compiler-provided compile-time constants. Kept in its own TU
// because the declaration list of sema_decl.cpp is already long (AGENTS.md: one TU per functional
// boundary).
//
// The unit declares the names and types; the compiler binds the values (see `check_global`, which
// resolves `builtin` constants through `builtin_const`). This TU cross-checks the target enum
// declarations against the compiler's table so the two cannot drift.
#include "sema_impl.hpp"

#include "directive.hpp"

namespace muc_detail {
    void Sema::check_comptime_unit() {
        auto it = infos_.find("std.comptime");
        if (it == infos_.end()) return;
        UnitInfo& ui = it->second;
        cur_ = ui.unit;

        for (auto& e : ui.enums) {
            std::vector<std::string> want;
            if (!builtin_enum_members(e.first, want)) continue;
            EnumDecl* ed = e.second;
            if (ed->members.size() != want.size()) {
                err("`std.comptime` enum `" + e.first + "` must declare exactly " +
                        std::to_string(want.size()) +
                        " members, in the order the compiler's build-constant table fixes (SPEC 9.3)",
                    ed);
            }
            for (size_t i = 0; i < want.size(); ++i) {
                if (ed->members[i].first != want[i]) {
                    err("`std.comptime` enum `" + e.first + "` member " + std::to_string(i) +
                            " must be `" + want[i] + "` (the compiler's table)",
                        ed);
                }
                if (ed->members[i].second && *ed->members[i].second != static_cast<long long>(i)) {
                    err("`std.comptime` enum `" + e.first + "` member `" + want[i] +
                            "` must have value " + std::to_string(i) + " (the compiler's table)",
                        ed);
                }
            }
        }
    }
}  // namespace muc_detail
