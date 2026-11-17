// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Compiler directives (`#if` / `#elif` / `#else` / `#error`) executed while parsing.
//
// The selected arm's elements are produced by the enclosing list loop exactly as if the directive
// were not there (splice, no scope); the other arms are parsed with `discard_depth_` raised and
// dropped, so their syntax is checked but nothing else happens.
#include "directive.hpp"
#include "parser_impl.hpp"

namespace muc_detail {

void Parser::directive_begin_arm(const Expr* cond, const Token& at) {
    DirectiveGroup& g = groups_.back();
    g.awaiting = false;
    bool take = false;
    // A nested condition inside an arm that is already dropped is not evaluated: its constants (or
    // its errors) belong to a configuration that is not being compiled.
    if (!g.taken && discard_depth_ == 0) {
        if (!cond) {
            take = true;
        } else {
            std::string why;
            take = eval_directive_cond(*cond, why, &user_consts_, &ambiguous_consts_);
            if (!why.empty()) throw err_at("`#if` condition: " + why, at);
        }
    }
    g.arm_taken = take;
    if (take) g.taken = true;
    else ++discard_depth_;
}

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
        if (discard_depth_ == 0) throw err_at("#error " + msg, nm);
        return;
    }

    if (d == "if") {
        groups_.push_back(DirectiveGroup{});
        DirectiveGroup& g = groups_.back();
        // A statement list collects each arm into its own container; a declaration list does the same.
        if (cur_stmts_) {
            g.stmt_mode = true;
            g.outer_stmts = cur_stmts_;
            cur_stmts_ = &g.stmts;
        } else if (cur_members_) {
            g.member_mode = true;
            g.outer_members = cur_members_;
            cur_members_ = &g.members;
        } else if (cur_decls_) {
            g.decl_mode = true;
            g.outer_decls = cur_decls_;
            cur_decls_ = &g.decls;
            g.node = std::make_unique<DirectiveDecl>();
        }
        ExprPtr cond = parse_expr();
        expect_op("{");
        directive_begin_arm(cond.get(), nm);
        return;
    }

    if (d == "elif" || d == "else") {
        if (groups_.empty() || !groups_.back().awaiting) {
            throw err_at("`#" + d + "` without a preceding `#if` / `#elif` arm", nm);
        }
        DirectiveGroup& g = groups_.back();
        if (g.saw_else) throw err_at("`#" + d + "` cannot follow `#else`", nm);
        ExprPtr cond;
        if (d == "elif") cond = parse_expr();
        else g.saw_else = true;
        expect_op("{");
        directive_begin_arm(cond.get(), nm);
        return;
    }

    throw err_at("unknown compiler directive `#" + d + "`; the directives are `#if`, `#elif`, `#else` and `#error`", nm);
}

void Parser::directive_arm_end() {
    advance();  // '}'
    DirectiveGroup& g = groups_.back();
    if (g.member_mode && g.arm_taken) {
        MemberGroup& dst = *g.outer_members;
        MemberGroup& src = g.members;
        for (auto& f : src.fields) dst.fields.push_back(std::move(f));
        for (auto& [k, v] : src.field_visibility) dst.field_visibility[k] = v;
        for (auto& cm : src.const_members) dst.const_members.push_back(std::move(cm));
        for (auto& m : src.methods) dst.methods.push_back(std::move(m));
        for (auto& c : src.ctors) dst.ctors.push_back(std::move(c));
        if (src.dtor) dst.dtor = std::move(src.dtor);
        src = MemberGroup{};
    }
    if (g.stmt_mode && g.arm_taken) {
        for (auto& s : g.stmts) g.outer_stmts->push_back(std::move(s));
        g.stmts.clear();
    }
    if (g.decl_mode && g.arm_taken && g.node) {
        // The parser still selects the arm (P3.2 keeps the previous behaviour); the node records the
        // selected declarations so the semantic pass can splice them after the name/layout pass.
        g.node->arms.emplace_back(DirectiveDecl::Arm{nullptr, std::move(g.decls)});
        g.decls.clear();
    }
    if (!g.arm_taken) --discard_depth_;
    g.arm_taken = false;
    // Another arm of the same chain follows: `directive_header` continues it on the next iteration.
    if (peek().is_op("#") && (peek(1).text == "elif" || peek(1).text == "else")) {
        g.awaiting = true;
        return;
    }
    if (g.stmt_mode) cur_stmts_ = g.outer_stmts;
    if (g.member_mode) cur_members_ = g.outer_members;
    if (g.decl_mode) {
        // The chain is finished: place the node in the enclosing declaration list (the semantic pass
        // splices the selected arm). An empty arm list means "nothing selected".
        if (g.node) g.outer_decls->push_back(std::move(g.node));
        cur_decls_ = g.outer_decls;
    }
    groups_.pop_back();
}

}  // namespace muc_detail
