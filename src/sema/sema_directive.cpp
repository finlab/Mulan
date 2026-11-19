// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// `std.comptime` validation and the `#if` pass (P3).
//
// The parser keeps every arm of a `#if` chain as data (DirectiveDecl / DirectiveStmt / MemberGroup),
// so the conditions are evaluated here: after the units are parsed and the compile-time constants are
// known. `collect` splices declaration and member arms, `check_block` splices statement arms.
//
// owns:      std.comptime validation and the #if pass that picks one arm of a directive chain
// not:       parsing the arms (parser_directive.cpp) or evaluating conditions (directive.cpp)
// pass:      after declarations are placed, before bodies
// concept:   directives, comptime
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

    DirectiveScope Sema::directive_scope(Unit* u, const UserConstTable* own, int before_line) {
        DirectiveScope scope;
        std::unordered_map<std::string, std::string> src;  // bare name -> declaring unit
        for (auto& use : u->uses) {
            // A `use` written after the declaration being processed is not visible to its `#if`
            // (SPEC 9.2: the condition sees the constants visible at that source position).
            if (before_line > 0 && use->line >= before_line) continue;
            if (use->unit_name == "std.comptime") {
                scope.comptime = true;
                UserConstTable builtins;
                add_builtin_constants(builtins);
                for (const auto& [k, v] : builtins) {
                    scope.table.emplace(k, v);                 // bare (`os`, `is_windows`)
                    scope.table["std.comptime." + k] = v;      // qualified (`std.comptime.os`)
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

    const UserConstTable* Sema::directive_const_table() {
        if (!cur_) return nullptr;
        if (collect_consts_) return collect_consts_;  // still collecting: text order, see the member
        if (const_table_unit_ != cur_) {
            const_table_unit_ = cur_;
            const_table_ = directive_scope(cur_, unit_all_consts(cur_->name)).table;
        }
        return &const_table_;
    }

    DirectiveDecl::Arm* Sema::choose_decl_arm(DirectiveDecl& dir, const DirectiveScope& scope) {        for (auto& arm : dir.arms) {
            if (!arm.cond) return &arm;  // `#else`: only reached when nothing above matched
            std::string why;
            if (eval_directive_cond(*arm.cond, why, &scope.table, &scope.ambiguous, scope.comptime)) return &arm;
            if (!why.empty()) err("`#if` condition: " + why, arm.cond.get());
        }
        return nullptr;
    }

    DirectiveStmt::Arm* Sema::choose_stmt_arm(DirectiveStmt& dir, const DirectiveScope& scope) {
        for (auto& arm : dir.arms) {
            if (!arm.cond) return &arm;
            std::string why;
            if (eval_directive_cond(*arm.cond, why, &scope.table, &scope.ambiguous, scope.comptime)) return &arm;
            if (!why.empty()) err("`#if` condition: " + why, arm.cond.get());
        }
        return nullptr;
    }

    DirectiveMembers::Arm* Sema::choose_member_arm(DirectiveMembers& dir, const DirectiveScope& scope) {
        for (auto& arm : dir.arms) {
            if (!arm.cond) return &arm;
            std::string why;
            if (eval_directive_cond(*arm.cond, why, &scope.table, &scope.ambiguous, scope.comptime)) return &arm;
            if (!why.empty()) err("`#if` condition: " + why, arm.cond.get());
        }
        return nullptr;
    }

    void Sema::expand_member_body(MemberGroup& body, const DirectiveScope& scope, const MemberSink& sink,
                                  const Node* at) {
        size_t fi = 0, ki = 0, mi = 0, ci = 0, pi = 0;
        // Take the members declared before the next group; a group's selected arm is spliced in place.
        auto take = [&](size_t fend, size_t kend, size_t mend, size_t cend, size_t pend) {
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
            for (; pi < pend; ++pi) {
                if (sink.promotes) sink.promotes->push_back(std::move(body.promotes[pi]));
            }
        };
        for (auto& g : body.groups) {
            take(g.fields_before, g.consts_before, g.methods_before, g.ctors_before, g.promotes_before);
            DirectiveMembers::Arm* chosen = choose_member_arm(*g.dir, scope);
            if (!chosen) continue;
            for (auto& e : chosen->members.errors) err("#error " + e->message, e.get());
            expand_member_body(chosen->members, scope, sink, at);
        }
        take(body.fields.size(), body.const_members.size(), body.methods.size(), body.ctors.size(),
             body.promotes.size());
        for (auto& e : body.errors) err("#error " + e->message, e.get());
        // Default member initializers are keyed by field name, so the order is not significant.
        if (sink.field_default) {
            for (auto& [fname, init] : body.field_default) {
                if (init) (*sink.field_default)[fname] = std::move(init);
            }
        }
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

    void Sema::expand_type_decl(Decl* d, Unit* u, const UserConstTable* own) {        MemberGroup* body = nullptr;
        MemberSink sink;
        if (auto* sd = dynamic_cast<StructDecl*>(d)) {
            body = &sd->body;
            sink = MemberSink{&sd->fields, &sd->field_visibility, &sd->field_default, &sd->const_members,
                              &sd->methods, &sd->ctors, &sd->dtor, &sd->promotes};
        } else if (auto* cd = dynamic_cast<ClassDecl*>(d)) {
            body = &cd->body;
            sink = MemberSink{&cd->fields, &cd->field_visibility, &cd->field_default, &cd->const_members,
                              &cd->methods, &cd->ctors, &cd->dtor, &cd->promotes};
        } else if (auto* id = dynamic_cast<InterfaceDecl*>(d)) {
            body = &id->body;
            sink.methods = &id->methods;
        }
        if (!body) return;
        // The body is the only place the members live, so this always runs; the constants are needed
        // only when there is a conditional group to evaluate. The `use` set is limited to the ones
        // written before the type declaration (SPEC 9.2).
        DirectiveScope scope;
        if (!body->groups.empty()) scope = directive_scope(u, own, d->line);
        expand_member_body(*body, scope, sink, d);
    }

    void Sema::check_directive_locals(const Expr* e) {
        if (!e) return;
        if (auto* n = dynamic_cast<const Name*>(e)) {
            if (LocalVar* lv = lookup_local(n->ident)) {
                err("a `#if` condition cannot reference the local variable `" + n->ident +
                        "`; a compile-time constant of the unit is required",
                    lv);
            }
            return;
        }
        if (auto* m = dynamic_cast<const Member*>(e)) {
            check_directive_locals(m->obj.get());
            return;
        }
        if (auto* u = dynamic_cast<const Unary*>(e)) {
            check_directive_locals(u->operand.get());
            return;
        }
        if (auto* n = dynamic_cast<const NotExpr*>(e)) {
            check_directive_locals(n->operand.get());
            return;
        }
        if (auto* b = dynamic_cast<const Binary*>(e)) {
            check_directive_locals(b->left.get());
            check_directive_locals(b->right.get());
            return;
        }
        if (auto* c = dynamic_cast<const Cast*>(e)) {
            check_directive_locals(c->expr.get());
            return;
        }
        if (auto* c = dynamic_cast<const Call*>(e)) {
            check_directive_locals(c->callee.get());
            for (const auto& a : c->args) check_directive_locals(a.get());
        }
    }

    void Sema::expand_directive_stmt(Block* b, size_t& i) {
        auto* ds = dynamic_cast<DirectiveStmt*>(b->stmts[i].get());
        // A local variable may not appear in a `#if` condition (SPEC 9.2): a local that happens to share
        // a builtin's name must not silently resolve to the builtin.
        for (auto& arm : ds->arms) check_directive_locals(arm.cond.get());
        // A function body is checked after the unit is collected: it follows the unit's name
        // resolution, so every `use` of the unit is visible (SPEC 9.2).
        const DirectiveScope scope = directive_scope(cur_, unit_all_consts(cur_->name));
        std::vector<StmtPtr> picked;
        if (DirectiveStmt::Arm* chosen = choose_stmt_arm(*ds, scope)) picked = std::move(chosen->stmts);
        b->stmts.erase(b->stmts.begin() + static_cast<long>(i));
        b->stmts.insert(b->stmts.begin() + static_cast<long>(i),
                        std::make_move_iterator(picked.begin()), std::make_move_iterator(picked.end()));
    }
}  // namespace muc_detail
