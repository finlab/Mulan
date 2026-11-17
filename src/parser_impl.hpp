// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parser internals: Parser class declaration and cross-file helper declarations (used only by parser_*.cpp)
//
// Recursive descent; precedence levels match the Python version exactly (high → low):
//   postfix → unary → * / % → + - → << >> → & → ^ → | → comparison → == != → not → and → or
// Bitwise operators bind tighter than comparison; `not` is looser than comparison and tighter than and/or. Assignment is a statement, not an expression.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast.hpp"
#include "common.hpp"
#include "directive.hpp"
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

    // Two budgets, because they exhaust the stack for different reasons (measured on this host,
    // MSYS2/UCRT, with the 8 MB main-thread reserve from CMakeLists.txt):
    //   * recursive nesting: a parenthesised subexpression costs **3** guard levels (expression →
    //     unary → primary), a unary operator or a nested block costs 1. ~2.5 KB of parser frames per
    //     level, so 320 levels ≈ 100 nested parentheses ≈ 0.8 MB — far below the 8 MB reserve.
    //     Hand-written code rarely goes past ~10 levels; the cap exists so absurd input gets a
    //     diagnostic instead of a stack overflow.
    //   * a chain built in a loop (`a.b.c…`, `1+1+1…`, `int***…`): no parser recursion, one AST
    //     level per link; the AST walkers (sema, codegen) recurse over it and overflow past 5k.
    static constexpr int kMaxNestDepth = 320;
    static constexpr int kMaxChainLinks = 2048;

    // One level of nesting for the lifetime of the guard. Throws before incrementing, so a guard
    // that is never constructed cannot unbalance the counter.
    class NestGuard {
    public:
        NestGuard(Parser& p, const char* what) : p_(p) {
            if (p_.nest_depth_ >= kMaxNestDepth) throw p_.nest_too_deep(what);
            ++p_.nest_depth_;
        }
        ~NestGuard() { --p_.nest_depth_; }
        NestGuard(const NestGuard&) = delete;
        NestGuard& operator=(const NestGuard&) = delete;

    private:
        Parser& p_;
    };

    ParserError nest_too_deep(const char* what) const {
        return err(std::string(what) + " nesting too deep (limit " + std::to_string(kMaxNestDepth) +
                   " levels); simplify the construct");
    }

    // A chain built in a loop adds one tree level per link with no recursion at all; `links` counts
    // them within the current construct, which bounds the depth handed to sema and codegen.
    void note_link(int& links, const char* what) {
        if (++links > kMaxChainLinks) {
            throw err(std::string(what) + " is too long (limit " + std::to_string(kMaxChainLinks) +
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
    void directive_header();
    void directive_arm_end();
    void directive_begin_arm(const Expr* cond, const Token& at);

    struct DirectiveGroup {
        bool taken = false;      // some arm of this chain is selected
        bool arm_taken = false;  // the arm being parsed is selected
        bool saw_else = false;
        bool awaiting = false;   // the arm just closed; `#elif` / `#else` may follow
        // P3.1: a declaration-list arm is parsed into its own container and merged only if selected.
        // The statement/member positions still use the old discard scheme until the next slice.
        bool decl_mode = false;
        std::vector<std::unique_ptr<Decl>>* outer_decls = nullptr;
        std::vector<std::unique_ptr<Decl>> decls;
        // The node that carries the declaration arms to the semantic pass (P3.2) and the condition of
        // the arm being parsed.
        std::unique_ptr<DirectiveDecl> node;
        ExprPtr arm_cond;
        // A statement-list arm (P3.1, statement slice).
        bool stmt_mode = false;
        std::vector<StmtPtr>* outer_stmts = nullptr;
        std::vector<StmtPtr> stmts;
        // A type-body arm (P3.1, member slice).
        bool member_mode = false;
        MemberGroup* outer_members = nullptr;
        MemberGroup members;
    };
    std::vector<DirectiveGroup> groups_;
    int discard_depth_ = 0;  // > 0: this arm is not compiled, so its nested conditions are not evaluated

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
    // Unit-level compile-time constants already declared in this unit: a later `#if` may read them
    // (the same "declared before use" rule as ordinary constant initializers).
    UserConstTable user_consts_;
    std::unordered_map<std::string, std::string> const_source_;  // bare name -> declaring unit
    std::unordered_set<std::string> ambiguous_consts_;           // bare names from several used units

    // One list element is parsed at a time; while the element sits in an arm that is not compiled the
    // arm must still be parsed (syntax is checked) but nothing may reach the AST. The undo action
    // removes whatever the element appended, and it runs on every exit path of the element's parse.
    class ArmDrop {
    public:
        ArmDrop(Parser& p, std::function<void()> undo) : p_(p), undo_(std::move(undo)) {}
        ~ArmDrop() {
            if (p_.discard_depth_ > 0 && undo_) undo_();
        }
        ArmDrop(const ArmDrop&) = delete;
        ArmDrop& operator=(const ArmDrop&) = delete;

    private:
        Parser& p_;
        std::function<void()> undo_;
    };

    // ------------------------------------------------------ unit

    std::unique_ptr<Decl> parse_extern(const Token& tk);
    // `builtin const T name;` / `builtin T f(...);` — compiler-provided compile-time declarations (9.3)
    std::unique_ptr<Decl> parse_builtin(const Token& tk);
    std::unique_ptr<Decl> parse_struct(bool is_union = false);
    std::unique_ptr<Decl> parse_enum();
    std::unique_ptr<Decl> parse_class();
    std::unique_ptr<Decl> parse_interface();
    std::unique_ptr<FuncDecl> parse_operator_func(TypeSpecPtr ret, bool ret_ref, const Token& tk);
    std::unique_ptr<MethodDecl> parse_method(TypeSpecPtr ret, bool ret_ref, const Token& name_tok);
    // operator signature in an interface: `T operator+(T a, T b);` (no this)
    std::unique_ptr<MethodDecl> parse_interface_operator(TypeSpecPtr ret, bool ret_ref);
    // member operator inside a struct body: `T operator+(B other) { ... }` (implicit borrowed `this`)
    std::unique_ptr<MethodDecl> parse_member_operator(TypeSpecPtr ret, bool ret_ref);
    // Return type: `ref T` / `ref const T` / `T`. `ref` is consumed only in return position and never enters the general type syntax.
    TypeSpecPtr parse_return_type(bool& ret_ref);
    std::unique_ptr<CtorDecl> parse_ctor(const Token& name_tok);
    std::unique_ptr<DtorDecl> parse_dtor();
    std::vector<std::string> parse_base_list(std::vector<TypeSpecPtr>* specs = nullptr);
    std::string parse_qualified_name();
    std::vector<std::unique_ptr<GlobalVar>> parse_global();
    std::vector<std::unique_ptr<GlobalVar>> parse_global_list(TypeSpecPtr spec, bool is_const);
    // Record a unit-level `const` whose initializer folds, so a later `#if` in this unit can read it
    // (decided 2026-11-17).
    void record_user_const(GlobalVar* g, const std::string& unit_name);
    std::unique_ptr<FuncDecl> parse_func(std::string name, TypeSpecPtr ret, bool ret_ref, const Token& tk);
    std::vector<std::unique_ptr<Param>> parse_params(bool* varargs = nullptr, bool allow_ref = false,
                                                     bool allow_move = false);

    // ------------------------------------------------------ types

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
    std::unique_ptr<Stmt> parse_para();
    std::unique_ptr<Stmt> parse_switch();
    std::unique_ptr<Stmt> parse_try();

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
    // Current expression/block/type nesting depth; see NestGuard above.
    int nest_depth_ = 0;
};

}  // namespace muc_detail
