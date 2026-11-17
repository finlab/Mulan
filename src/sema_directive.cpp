// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// `std.comptime` validation and the `#if` pass (P3).
//
// The parser keeps every arm of a `#if` chain as data (DirectiveDecl / DirectiveStmt / MemberGroup),
// so the conditions are evaluated here: after the units are parsed and the compile-time constants are
// known. `collect` splices declaration and member arms, `check_block` splices statement arms.
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

    DirectiveScope Sema::directive_scope(Unit* u, const UserConstTable* own) {
        DirectiveScope scope;
        std::unordered_map<std::string, std::string> src;  // bare name -> declaring unit
        for (auto& use : u->uses) {
            if (use->unit_name == "std.comptime") {
                UserConstTable builtins;
                add_builtin_constants(builtins);
                for (const auto& [k, v] : builtins) {
                    scope.table.emplace(k, v);
                    src.emplace(k, "std.comptime");
                }
                continue;
            }
            const UserConstTable* t = unit_public_consts(use->unit_name);
            if (!t) continue;
            for (const auto& [k, v] : *t) {
                auto it = src.find(k);
                if (it == src.end()) src[k] = use->unit_name;
                else if (it->second != use->unit_name) scope.ambiguous.insert(k);
                scope.table.emplace(k, v);                      // a bare name; the first unit wins
                scope.table[use->unit_name + "." + k] = v;      // the qualified form `unit.name`
            }
        }
        if (own) {
            for (const auto& [k, v] : *own) {
                scope.table[k] = v;  // this unit wins; a same-name constant is not ambiguous
                scope.ambiguous.erase(k);
            }
        }
        return scope;
    }

    DirectiveDecl::Arm* Sema::choose_decl_arm(DirectiveDecl& dir, const DirectiveScope& scope) {
        for (auto& arm : dir.arms) {
            if (!arm.cond) return &arm;  // `#else`: only reached when nothing above matched
            std::string why;
            if (eval_directive_cond(*arm.cond, why, &scope.table, &scope.ambiguous)) return &arm;
            if (!why.empty()) err("`#if` condition: " + why, arm.cond.get());
        }
        return nullptr;
    }

    DirectiveStmt::Arm* Sema::choose_stmt_arm(DirectiveStmt& dir, const DirectiveScope& scope) {
        for (auto& arm : dir.arms) {
            if (!arm.cond) return &arm;
            std::string why;
            if (eval_directive_cond(*arm.cond, why, &scope.table, &scope.ambiguous)) return &arm;
            if (!why.empty()) err("`#if` condition: " + why, arm.cond.get());
        }
        return nullptr;
    }

    DirectiveMembers::Arm* Sema::choose_member_arm(DirectiveMembers& dir, const DirectiveScope& scope) {
        for (auto& arm : dir.arms) {
            if (!arm.cond) return &arm;
            std::string why;
            if (eval_directive_cond(*arm.cond, why, &scope.table, &scope.ambiguous)) return &arm;
            if (!why.empty()) err("`#if` condition: " + why, arm.cond.get());
        }
        return nullptr;
    }

    void Sema::expand_member_body(MemberGroup& body, const DirectiveScope& scope, const MemberSink& sink,
                                  const Node* at) {
        size_t fi = 0, ki = 0, mi = 0, ci = 0;
        // Take the members declared before the next group; a group's selected arm is spliced in place.
        auto take = [&](size_t fend, size_t kend, size_t mend, size_t cend) {
            for (; fi < fend; ++fi) {
                const std::string& name = body.fields[fi].first;
                bool vis = true;
                auto vit = body.field_visibility.find(name);
                if (vit != body.field_visibility.end()) vis = vit->second;
                if (sink.field_visibility) (*sink.field_visibility)[name] = vis;
                if (sink.fields) sink.fields->push_back(std::move(body.fields[fi]));
            }
            for (; ki < kend; ++ki) {
                if (sink.const_members) sink.const_members->push_back(std::move(body.const_members[ki]));
            }
            for (; mi < mend; ++mi) {
                if (sink.methods) sink.methods->push_back(std::move(body.methods[mi]));
            }
            for (; ci < cend; ++ci) {
                if (sink.ctors) sink.ctors->push_back(std::move(body.ctors[ci]));
            }
        };
        for (auto& g : body.groups) {
            take(g.fields_before, g.consts_before, g.methods_before, g.ctors_before);
            DirectiveMembers::Arm* chosen = choose_member_arm(*g.dir, scope);
            if (!chosen) continue;
            for (auto& e : chosen->members.errors) err("#error " + e->message, e.get());
            expand_member_body(chosen->members, scope, sink, at);
        }
        take(body.fields.size(), body.const_members.size(), body.methods.size(), body.ctors.size());
        for (auto& e : body.errors) err("#error " + e->message, e.get());
        if (body.dtor) {
            if (sink.dtor) {
                if (sink.dtor->get()) {
                    std::string what = "type";
                    if (auto* sd = dynamic_cast<const StructDecl*>(at)) {
                        what = (sd->is_union ? "union `" : "struct `") + sd->name + "`";
                    } else if (auto* cd = dynamic_cast<const ClassDecl*>(at)) {
                        what = "class `" + cd->name + "`";
                    }
                    err(what + " can only have one destructor", at);
                }
                *sink.dtor = std::move(body.dtor);
            }
        }
        body.groups.clear();
    }

    void Sema::expand_type_decl(Decl* d, Unit* u, const UserConstTable* own) {
        MemberGroup* body = nullptr;
        MemberSink sink;
        if (auto* sd = dynamic_cast<StructDecl*>(d)) {
            body = &sd->body;
            sink = MemberSink{&sd->fields, &sd->field_visibility, &sd->const_members, &sd->methods,
                              &sd->ctors, &sd->dtor};
        } else if (auto* cd = dynamic_cast<ClassDecl*>(d)) {
            body = &cd->body;
            sink = MemberSink{&cd->fields, &cd->field_visibility, &cd->const_members, &cd->methods,
                              &cd->ctors, &cd->dtor};
        } else if (auto* id = dynamic_cast<InterfaceDecl*>(d)) {
            body = &id->body;
            sink.methods = &id->methods;
        }
        if (!body) return;
        // The body is the only place the members live, so this always runs; the constants are needed
        // only when there is a conditional group to evaluate.
        DirectiveScope scope;
        if (!body->groups.empty()) scope = directive_scope(u, own);
        expand_member_body(*body, scope, sink, d);
    }

    void Sema::expand_directive_stmt(Block* b, size_t& i) {
        auto* ds = dynamic_cast<DirectiveStmt*>(b->stmts[i].get());
        const DirectiveScope scope = directive_scope(cur_, unit_all_consts(cur_->name));
        std::vector<StmtPtr> picked;
        if (DirectiveStmt::Arm* chosen = choose_stmt_arm(*ds, scope)) picked = std::move(chosen->stmts);
        b->stmts.erase(b->stmts.begin() + static_cast<long>(i));
        b->stmts.insert(b->stmts.begin() + static_cast<long>(i),
                        std::make_move_iterator(picked.begin()), std::make_move_iterator(picked.end()));
    }
}  // namespace muc_detail
