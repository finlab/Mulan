// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Compiler directives (`#if` / `#elif` / `#else` / `#error`).
//
// The parser keeps every arm as data (`DirectiveDecl`, `DirectiveStmt`, `MemberGroup::Group`); the
// semantic pass evaluates the conditions once the compile-time constants are known (P3.3) and splices
// the selected arm into the enclosing list. The parser splices nothing and evaluates nothing, so a
// condition may use any constant the semantic pass can see (a `sizeof` constant, a `use`d unit).
// Dead arms are still parsed, so their syntax is checked and nothing else happens.
#include "parser_impl.hpp"

namespace muc_detail {

void Parser::directive_header() {
    const Token hash = advance();  // '#'
    if (!hash.bol) {
        throw err_at("`#` starts a compiler directive and must be the first character on its line", hash);
    }
    if (at_eof()) throw err_at("expected a directive name after `#`", hash);
    const Token nm = advance();
    const std::string d = nm.text;

    if (d == "error") {
        if (peek().kind != TokKind::Str) throw err_at("`#error` needs a string literal message", peek());
        const std::string msg = advance().bytes;
        if (at_op(";")) advance();
        // Inside a conditional arm the error is raised only if that arm is selected (P3.3).
        if (!groups_.empty() && cur_stmts_) {
            auto es = std::make_unique<ErrorStmt>();
            es->message = msg;
            es->line = nm.line;
            es->col = nm.col;
            push_stmt(std::move(es));
            return;
        }
        if (!groups_.empty() && cur_members_) {
            auto ed = std::make_unique<ErrorDecl>();
            ed->message = msg;
            ed->line = nm.line;
            ed->col = nm.col;
            groups_.back().members.errors.push_back(std::move(ed));
            return;
        }
        if (!groups_.empty() && cur_decls_) {
            auto ed = std::make_unique<ErrorDecl>();
            ed->message = msg;
            ed->line = nm.line;
            ed->col = nm.col;
            push_decl(std::move(ed));
            return;
        }
        throw err_at("#error " + msg, nm);
    }

    if (d == "if") {
        if (groups_.size() >= kMaxIfNesting) {
            throw err_at("`#if` chains are nested too deeply (limit " + std::to_string(kMaxIfNesting) +
                             " levels); simplify the conditional structure",
                         nm);
        }
        groups_.emplace_back();
        DirectiveGroup& g = groups_.back();
        // The arm's elements collect into their own container; the selected one is spliced by the
        // semantic pass at the position of the directive node.
        if (cur_stmts_) {
            g.stmt_mode = true;
            g.outer_stmts = cur_stmts_;
            cur_stmts_ = &g.stmts;
            g.stmt_node = std::make_unique<DirectiveStmt>();
            g.stmt_node->line = nm.line;
            g.stmt_node->col = nm.col;
        } else if (cur_members_) {
            g.member_mode = true;
            g.outer_members = cur_members_;
            g.fields_before = cur_members_->fields.size();
            g.consts_before = cur_members_->const_members.size();
            g.methods_before = cur_members_->methods.size();
            g.ctors_before = cur_members_->ctors.size();
            cur_members_ = &g.members;
            g.member_node = std::make_unique<DirectiveMembers>();
            g.member_node->line = nm.line;
            g.member_node->col = nm.col;
        } else if (cur_decls_) {
            g.decl_mode = true;
            g.outer_decls = cur_decls_;
            cur_decls_ = &g.decls;
            g.node = std::make_unique<DirectiveDecl>();
            g.node->line = nm.line;
            g.node->col = nm.col;
        } else {
            throw err_at("`#if` is not allowed here", hash);
        }
        g.arm_cond = parse_expr();
        expect_op("{");
        return;
    }

    if (d == "elif" || d == "else") {
        if (groups_.empty() || !groups_.back().awaiting) {
            throw err_at("`#" + d + "` without a preceding `#if` / `#elif` arm", nm);
        }
        DirectiveGroup& g = groups_.back();
        if (g.saw_else) throw err_at("`#" + d + "` cannot follow `#else`", nm);
        g.awaiting = false;
        if (d == "elif") g.arm_cond = parse_expr();
        else g.saw_else = true;
        expect_op("{");
        return;
    }

    throw err_at("unknown compiler directive `#" + d + "`; the directives are `#if`, `#elif`, `#else` and `#error`", nm);
}

void Parser::directive_arm_end() {
    advance();  // '}'
    DirectiveGroup& g = groups_.back();
    if (g.stmt_mode) {
        g.stmt_node->arms.emplace_back(std::move(g.arm_cond), std::move(g.stmts));
        g.stmts.clear();
    } else if (g.member_mode) {
        g.member_node->arms.emplace_back(std::move(g.arm_cond), std::move(g.members));
        g.members = MemberGroup{};
    } else if (g.decl_mode && g.node) {
        g.node->arms.emplace_back(std::move(g.arm_cond), std::move(g.decls));
        g.decls.clear();
    }
    g.arm_cond.reset();
    // Another arm of the same chain follows: `directive_header` continues it on the next iteration.
    if (peek().is_op("#") && (peek(1).text == "elif" || peek(1).text == "else")) {
        g.awaiting = true;
        return;
    }
    // The chain is finished: place the node in the enclosing list. An empty arm list means "nothing
    // selected", which the semantic pass treats as an empty splice.
    if (g.stmt_mode) {
        g.outer_stmts->push_back(std::move(g.stmt_node));
        cur_stmts_ = g.outer_stmts;
    }
    if (g.member_mode) {
        g.outer_members->groups.push_back(MemberGroup::Group{g.fields_before, g.consts_before,
                                                             g.methods_before, g.ctors_before,
                                                             std::move(g.member_node)});
        cur_members_ = g.outer_members;
    }
    if (g.decl_mode) {
        if (g.node) g.outer_decls->push_back(std::move(g.node));
        cur_decls_ = g.outer_decls;
    }
    groups_.pop_back();
}

}  // namespace muc_detail
