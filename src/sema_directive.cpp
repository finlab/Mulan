// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// std.target seeding: the build constants a #if condition reads. Kept in its own TU because the
// declaration list of sema_decl.cpp is already long (AGENTS.md: one TU per functional boundary).
#include "sema_impl.hpp"

#include "directive.hpp"

namespace muc_detail {
    // `std.target` declares the build constants (`os`, `ptr_bits`, ...) but the compiler owns their
    // values: the initializers in the source are placeholders. They are rewritten here into ordinary
    // enum members / literals so the one constant evaluator handles them like any other constant,
    // and the enum declaration is cross-checked against the compiler's table so the two cannot drift.
    void Sema::seed_target_unit() {
        auto it = infos_.find("std.target");
        if (it == infos_.end()) return;
        UnitInfo& ui = it->second;
        cur_ = ui.unit;

        for (auto& e : ui.enums) {
            std::vector<std::string> want;
            if (!builtin_enum_members(e.first, want)) continue;
            EnumDecl* ed = e.second;
            if (ed->members.size() != want.size()) {
                err("`std.target` enum `" + e.first + "` must declare exactly " + std::to_string(want.size()) +
                        " members, in the order the compiler's build-constant table fixes (SPEC 9.2)",
                    ed);
            }
            for (size_t i = 0; i < want.size(); ++i) {
                if (ed->members[i].first != want[i]) {
                    err("`std.target` enum `" + e.first + "` member " + std::to_string(i) + " must be `" +
                            want[i] + "` (the compiler's table)",
                        ed);
                }
                if (ed->members[i].second && *ed->members[i].second != static_cast<long long>(i)) {
                    err("`std.target` enum `" + e.first + "` member `" + want[i] + "` must have value " +
                            std::to_string(i) + " (the compiler's table)",
                        ed);
                }
            }
        }

        for (auto& g : ui.globals) {
            GlobalVar* var = g.second;
            if (!var->is_const) continue;
            long long v = 0;
            if (!builtin_const(g.first, v)) continue;
            if (v < 0) {
                err("the build configuration has no value for `" + g.first + "`; check `--platform`",
                    var);
            }
            const std::string ty = var->spec ? var->spec->name : "";
            std::vector<std::string> members;
            auto ie = std::make_unique<InitExpr>();
            ie->line = var->line;
            ie->col = var->col;
            if (!ty.empty() && builtin_enum_members(ty, members)) {
                auto m = std::make_unique<Member>();
                auto base = std::make_unique<Name>();
                base->ident = ty;
                base->line = var->line;
                base->col = var->col;
                m->obj = std::move(base);
                m->name = members[static_cast<size_t>(v)];
                m->line = var->line;
                m->col = var->col;
                ie->expr = std::move(m);
            } else if (ty == "bool") {
                auto b = std::make_unique<BoolLit>();
                b->value = v != 0;
                ie->expr = std::move(b);
            } else {
                auto n = std::make_unique<IntLit>();
                n->value = v;
                ie->expr = std::move(n);
            }
            var->init = std::move(ie);
        }
    }
}  // namespace muc_detail
