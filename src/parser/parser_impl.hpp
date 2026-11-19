// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parser internals: Parser class declaration and cross-file helper declarations (used only by parser_*.cpp)
//
// Recursive descent; precedence levels match the Python version exactly (high → low):
//   postfix → unary → * / % → + - → << >> → & → ^ → | → comparison → == != → not → and → or
// Bitwise operators bind tighter than comparison; `not` is looser than comparison and tighter than and/or. Assignment is a statement, not an expression.
#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast.hpp"
#include "common.hpp"
#include "lexer.hpp"

namespace muc_detail {

// Multiple declarators in one declaration share a single type syntax; deep-copy so each owns its own TypeSpec.
// (defined in parser_expr.cpp; parser_unit.cpp / parser_stmt.cpp also call it)
TypeSpecPtr clone_spec(const TypeSpec& s);

// ---------------------------------------------------------------- parser

class Parser {
public:
    Parser(const std::string& src, const std::string& filename);

    std::string parse_unit_name() {
        std::string n = expect_ident("unit name").text;
        while (at_op(".")) {
            advance();
            n += ".";
            n += expect_ident("unit name").text;
        }
        return n;
    }

    std::unique_ptr<Unit> parse_unit();

private:
    // ------------------------------------------------------ basics

    const Token& peek(std::size_t k = 0) const {
        std::size_t j = pos_ + k;
        return j < toks_.size() ? toks_[j] : toks_.back();
    }

    bool at_eof() const { return peek().kind == TokKind::Eof; }

    Token advance() {
        Token t = peek();
        if (t.kind != TokKind::Eof) ++pos_;
        return t;
    }

    ParserError err(const std::string& msg) const { return err_at(msg, peek()); }

    ParserError err_at(const std::string& msg, const Token& t) const {
        return ParserError(msg, t.line, t.col, filename_);
    }

    template <class... Args>
    bool at_op(Args&&... ops) const {
        const Token& t = peek();
        if (t.kind != TokKind::Op) return false;
        return ((t.text == ops) || ...);
    }

    template <class... Args>
    bool at_kw(Args&&... kws) const {
        const Token& t = peek();
        if (t.kind != TokKind::Kw) return false;
        return ((t.text == kws) || ...);
    }

    Token expect_op(const std::string& op) {
        if (!at_op(op)) throw err("expected `" + op + "` but got " + peek().describe());
        return advance();
    }

    Token expect_kw(const std::string& kw) {
        if (!at_kw(kw)) throw err("expected `" + kw + "` but got " + peek().describe());
        return advance();
    }

    Token expect_ident(const std::string& what = "identifier") {
        if (peek().kind != TokKind::Ident) {
            throw err("expected " + what + " but got " + peek().describe());
        }
        return advance();
    }

    void semicolon() { expect_op(";"); }

    // ------------------------------------------------------ nesting guard

    // Three counters, because the constructs cost different numbers of guard levels and the standards
    // bound them separately (C++ [implimits]: 256 nesting levels of parenthesized expressions §1.4, 256
    // levels of compound/iteration/selection statements §1.1). A parenthesised subexpression costs **3**
    // expression levels (ternary → not → unary, the path a `( … )` operand takes); a block, statement or
    // initializer costs one block level; a nested type operand one type level. ~2.5 KB of parser frames
    // per level, so 768 expression levels ≈ 1.9 MB — far below the 8 MB main-thread reserve.
    //   * A chain built in a loop (`a.b.c…`, `1+1+1…`, `int***…`) has no parser recursion at all: one
    //     AST level per link, and the AST walkers (sema, codegen) recurse over it (see the link limits).
    static constexpr int kMaxParenNesting = 256;
    static constexpr int kMaxExprNest = 3 * kMaxParenNesting;
    static constexpr int kMaxBlockNesting = 256;
    // Nested type operands (`Box<Box<…>>`, `int**…` written out in a type position). Counted apart from
    // blocks so that a deep block does not use up the type budget.
    static constexpr int kMaxTypeNest = 256;
    // One operator/member chain in an expression. C++ sets no minimum for this; the AST walkers recurse
    // over a left-deep chain, so the bound is a stack guard.
    static constexpr int kMaxChainLinks = 2048;
    // Pointer / array / function declarators modifying one type (C++ [implimits] §1.3 sets 256).
    static constexpr int kMaxTypeLinks = 256;
    // Parameters in one function definition / arguments in one call (C [5.2.4.1] 127, C++ 256).
    static constexpr int kMaxParams = 256;
    // Nested `#if` chains are kept as data by the parser, but the semantic pass expands member arms
    // recursively and declaration arms through a deep call chain, so the depth is bounded here.
    static constexpr size_t kMaxIfNesting = 256;

    // One level of nesting for the lifetime of the guard. Throws before incrementing, so a guard
    // that is never constructed cannot unbalance the counter.
    class NestGuard {
    public:
        // `budget` is what the counter is checked against (a parenthesised subexpression spends three
        // expression levels, so the expression budget is 3 x the construct limit); `report` is the
        // construct limit named in the diagnostic.
        NestGuard(Parser& p, int& counter, int budget, int report, const char* what)
            : p_(p), counter_(counter), budget_(budget), report_(report) {
            if (counter_ >= budget_) throw p_.nest_too_deep(what, report_);
            ++counter_;
        }
        ~NestGuard() { --counter_; }
        NestGuard(const NestGuard&) = delete;
        NestGuard& operator=(const NestGuard&) = delete;

    private:
        Parser& p_;
        int& counter_;
        int budget_;
        int report_;
    };

    ParserError nest_too_deep(const char* what, int limit) const {
        return err(std::string(what) + " nesting too deep (limit " + std::to_string(limit) +
                   " levels); simplify the construct");
    }

    // A chain built in a loop adds one tree level per link with no recursion at all; `links` counts
    // them within the current construct, which bounds the depth handed to sema and codegen. `limit`
    // separates expression chains from type declarators (`kMaxTypeLinks`).
    void note_link(int& links, const char* what, int limit = kMaxChainLinks) {
        if (++links > limit) {
            throw err(std::string(what) + " is too long (limit " + std::to_string(limit) +
                      " chained operators or links); split it up");
        }
    }

    // `++`/`--` have no value: appearing in any expression context is an error
    void forbid_inc_expr() const {
        if (at_op("++", "--")) {
            throw err("`++`/`--` has no value and cannot appear in an expression; it must be a standalone statement");
        }
    }

    // ------------------------------------------------------ compiler directives

    // `#if` is executed while parsing: the selected arm is spliced into the enclosing list (no scope,
    // no AST node), the other arms are parsed and dropped (their syntax is still checked). A list
    // loop detects an arm boundary by comparing the size of `groups_` with the value it recorded on
    // entry, so a `}` closing a nested block is never mistaken for one.
    bool at_directive() const { return peek().is_op("#"); }
    // Whether a `struct` / `class` / `union` declaration starts at `from`, after any intervening
    // declaration modifiers (`nocopy`, `packed`, `align(N)`). `const` is a declaration modifier only in
    // that case (`const nocopy struct S` is not a `const` variable).
    bool type_decl_after_modifiers(std::size_t from) const {
        for (std::size_t i = from;;) {
            const std::string& t = peek(i).text;
            if (t == "struct" || t == "class" || t == "union") return true;
            if (t == "nocopy" || t == "packed") {
                ++i;
                continue;
            }
            if (t == "align" && peek(i + 1).is_op("(")) {
                int depth = 0;
                for (; i < toks_.size(); ++i) {
                    const Token& tk = peek(i);
                    if (tk.is_op("(")) {
                        ++depth;
                    } else if (tk.is_op(")")) {
                        if (--depth == 0) {
                            ++i;
                            break;
                        }
                    }
                }
                continue;
            }
            return false;
        }
    }
    void directive_header();
    void directive_arm_end();
    // The error for a directive at a position outside the directive list positions (SPEC 9.2).
    ParserError no_directive_here(const std::string& where) const;

    struct DirectiveGroup {
        bool saw_else = false;
        bool awaiting = false;   // the arm just closed; `#elif` / `#else` may follow
        // Every position (declaration list, statement list, type body) collects all arms; the semantic
        // pass evaluates the conditions (P3.3) and splices the selected arm.
        bool decl_mode = false;
        std::vector<std::unique_ptr<Decl>>* outer_decls = nullptr;
        std::vector<std::unique_ptr<Decl>> decls;
        std::unique_ptr<DirectiveDecl> node;
        ExprPtr arm_cond;  // the condition of the arm being parsed
        // A statement-list arm.
        bool stmt_mode = false;
        std::vector<StmtPtr>* outer_stmts = nullptr;
        std::vector<StmtPtr> stmts;
        std::unique_ptr<DirectiveStmt> stmt_node;
        // A type-body arm: the position of the group among the members, so the selected arm is spliced
        // back where it was written (field order is layout).
        bool member_mode = false;
        MemberGroup* outer_members = nullptr;
        MemberGroup members;
        std::unique_ptr<DirectiveMembers> member_node;
        size_t fields_before = 0;
        size_t consts_before = 0;
        size_t methods_before = 0;
        size_t ctors_before = 0;
    };
    // A deque keeps the references to an enclosing arm's containers valid when a nested `#if` starts.
    std::deque<DirectiveGroup> groups_;

    // The current declaration-list target (`#if` arms point it at their own container).
    std::vector<std::unique_ptr<Decl>>* cur_decls_ = nullptr;
    void push_decl(std::unique_ptr<Decl> d);
    // The current statement-list target (the statement slice).
    std::vector<StmtPtr>* cur_stmts_ = nullptr;
    void push_stmt(StmtPtr s);
    // The current type-body target (the member slice).
    MemberGroup* cur_members_ = nullptr;

    // A type body is not a declaration list: clear the target for the duration of the member list.
    struct DeclTargetScope {
        Parser& p;
        std::vector<std::unique_ptr<Decl>>* saved;
        explicit DeclTargetScope(Parser& parser)
            : p(parser), saved(parser.cur_decls_) { parser.cur_decls_ = nullptr; }
        DeclTargetScope(const DeclTargetScope&) = delete;
        DeclTargetScope& operator=(const DeclTargetScope&) = delete;
        ~DeclTargetScope() { p.cur_decls_ = saved; }
    };
    // ------------------------------------------------------ unit

    std::unique_ptr<Decl> parse_extern(const Token& tk);
    // `builtin const T name;` / `builtin T f(...);` — compiler-provided compile-time declarations (9.3)
    std::unique_ptr<Decl> parse_builtin(const Token& tk);
    std::unique_ptr<Decl> parse_struct(bool is_union = false);
    std::unique_ptr<Decl> parse_enum();
    std::unique_ptr<Decl> parse_class();
    std::unique_ptr<Decl> parse_interface();
    // `variant Name { Alt T; Pair T, U; None; }` (SPEC 10.1.1)
    std::unique_ptr<Decl> parse_variant();
    std::unique_ptr<FuncDecl> parse_operator_func(TypeSpecPtr ret, bool ret_ref, const Token& tk);
    std::unique_ptr<MethodDecl> parse_method(TypeSpecPtr ret, bool ret_ref, const Token& name_tok);
    // operator signature in an interface: `T operator+(T a, T b);` (no this)
    std::unique_ptr<MethodDecl> parse_interface_operator(TypeSpecPtr ret, bool ret_ref);
    // member operator inside a struct body: `T operator+(B other) { ... }` (implicit borrowed `this`)
    std::unique_ptr<MethodDecl> parse_member_operator(TypeSpecPtr ret, bool ret_ref);
    // Return type: `ref T` / `const ref T` / `T`. `ref` is consumed only in return position and never enters the general type syntax.
    TypeSpecPtr parse_return_type(bool& ret_ref);
    std::unique_ptr<CtorDecl> parse_ctor(const Token& name_tok);
    std::unique_ptr<DtorDecl> parse_dtor();
    std::vector<std::string> parse_base_list(std::vector<TypeSpecPtr>* specs = nullptr);
    std::string parse_qualified_name();
    std::vector<std::unique_ptr<GlobalVar>> parse_global();
    std::vector<std::unique_ptr<GlobalVar>> parse_global_list(TypeSpecPtr spec, bool is_const);
    std::unique_ptr<FuncDecl> parse_func(std::string name, TypeSpecPtr ret, bool ret_ref, const Token& tk);
    std::vector<std::unique_ptr<Param>> parse_params(bool* varargs = nullptr, bool allow_ref = false,
                                                     bool allow_move = false);

    // ------------------------------------------------------ types

    // primitive type keywords (matches `mtypes.PRIM_NAMES`; `string` is the core.Str struct)
    static const std::unordered_set<std::string>& prims();
    bool is_type_start(const Token& t) const;
    bool is_module_prefix(const std::string& s) const;
    TypeSpecPtr parse_type();
    TypeSpecPtr parse_type_base();  // without array suffixes (used by `new T[n]`)
    TypeSpecPtr type_suffixes(TypeSpecPtr spec, const Token& t);
    // generics: `<T, U: IList<int> & IComparable>` (declaration site) and `<int, long>` (use site)
    std::vector<TypeParam> parse_type_params();
    std::vector<std::unique_ptr<TypeSpec>> parse_type_args();
    // treat a lexical `>>` as two `>` when type arguments nest (`Vec<Vec<int>>`)
    Token expect_gt();

    // ------------------------------------------------------ statements

    std::unique_ptr<Block> parse_block();
    std::vector<StmtPtr> parse_stmt();
    bool looks_like_local_decl();
    std::vector<StmtPtr> parse_local(bool is_const, bool is_auto);
    InitPtr parse_init();
    std::unique_ptr<Stmt> parse_if();
    std::unique_ptr<Stmt> parse_while();
    std::unique_ptr<Stmt> parse_for();
    bool for_head_is_c_style();
    std::unique_ptr<Stmt> parse_c_for(const Token& tk);
    std::unique_ptr<Stmt> parse_para();
    std::unique_ptr<Stmt> parse_switch();
    std::unique_ptr<Stmt> parse_try();
    // `match` (SPEC 10.1.2): statement form (block arms) and expression form (expression arms).
    std::unique_ptr<Stmt> parse_match_stmt();
    ExprPtr parse_match_expr();
    ExprPtr parse_match_scrut();
    void parse_match_arms(std::vector<std::unique_ptr<MatchArm>>& arms, bool& has_else,
                          bool expression_form);

    // ------------------------------------------------------ expressions

    ExprPtr parse_expr() { return parse_ternary(); }
    ExprPtr parse_ternary();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_not();
    ExprPtr parse_eq();
    ExprPtr parse_rel();
    ExprPtr parse_bitor();
    ExprPtr parse_bitxor();
    ExprPtr parse_bitand();
    ExprPtr parse_shift();
    ExprPtr parse_add();
    ExprPtr parse_mul();
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr parse_postfix_tail(ExprPtr e);
    ExprPtr parse_call(ExprPtr callee);
    ExprPtr parse_primary();
// `(T a, T b) => body` (3.16): detection scans for the matching `)` followed by `=>`
bool looks_like_lambda() const;
ExprPtr parse_lambda();

    ExprPtr make_binary(const std::string& op, ExprPtr left, ExprPtr right, const Token& tok);

    std::string filename_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    int group_counter_ = 0;  // declaration statement group counter (for the auto multi-declarator consistency check)
    // known local type names (struct / enum), used to disambiguate `T x` from an expression
    std::unordered_set<std::string> type_names_;
    // while parsing the paren-free operand of `sizeof`/`alignof`: `as` belongs to the result, not the
    // operand (`sizeof *p as int` is `(sizeof *p) as int`, as in C)
    bool suppress_as_ = false;
    // unit names introduced by `use`, enabling qualified types such as `math.Point`
    std::unordered_set<std::string> module_names_;
    // Current nesting depth of each counter; see NestGuard above. Expression and statement nesting are
    // tracked separately because the standards bound them separately and a parenthesised expression
    // spends several expression levels.
    int nest_expr_ = 0;
    int nest_block_ = 0;
    int nest_type_ = 0;
};

}  // namespace muc_detail
