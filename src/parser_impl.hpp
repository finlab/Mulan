// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 语法分析器内部实现：Parser 类声明与跨文件辅助函数声明（仅供 parser_*.cpp 使用）
//
// 递归下降；优先级层次与 Python 版完全一致（高 → 低）：
//   postfix → unary → * / % → + - → << >> → & → ^ → | → 比较 → == != → not → and → or
// 位运算高于比较；`not` 低于比较、只高于 and/or。赋值是语句，不是表达式。
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast.hpp"
#include "common.hpp"
#include "lexer.hpp"

namespace muc_detail {

// 一条声明中的多个声明符共享同一个类型语法；深度复制以各持一份所有权的 TypeSpec。
// （定义在 parser_expr.cpp；parser_unit.cpp / parser_stmt.cpp 也会调用）
TypeSpecPtr clone_spec(const TypeSpec& s);

// ---------------------------------------------------------------- 语法分析器

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
    // ------------------------------------------------------ 基础

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

    // `++`/`--` 没有返回值：出现在任何表达式上下文都是错误
    void forbid_inc_expr() const {
        if (at_op("++", "--")) {
            throw err("`++`/`--` has no value and cannot appear in an expression; it must be a standalone statement");
        }
    }

    // ------------------------------------------------------ unit

    std::unique_ptr<Decl> parse_extern(const Token& tk);
    std::unique_ptr<Decl> parse_struct(bool is_union = false);
    std::unique_ptr<Decl> parse_enum();
    std::unique_ptr<Decl> parse_class();
    std::unique_ptr<Decl> parse_interface();
    std::unique_ptr<FuncDecl> parse_operator_func(TypeSpecPtr ret, bool ret_ref, const Token& tk);
    std::unique_ptr<MethodDecl> parse_method(TypeSpecPtr ret, bool ret_ref, const Token& name_tok);
    // 接口里的运算符签名：`T operator+(T a, T b);`（无 this）
    std::unique_ptr<MethodDecl> parse_interface_operator(TypeSpecPtr ret, bool ret_ref);
    // 返回类型：`ref T` / `ref const T` / `T`。`ref` 只在返回位置被消费，不进入通用类型语法。
    TypeSpecPtr parse_return_type(bool& ret_ref);
    std::unique_ptr<CtorDecl> parse_ctor(const Token& name_tok);
    std::unique_ptr<DtorDecl> parse_dtor();
    std::vector<std::string> parse_base_list(std::vector<TypeSpecPtr>* specs = nullptr);
    std::string parse_qualified_name();
    std::vector<std::unique_ptr<GlobalVar>> parse_global();
    std::vector<std::unique_ptr<GlobalVar>> parse_global_list(TypeSpecPtr spec, bool is_const);
    std::unique_ptr<FuncDecl> parse_func(std::string name, TypeSpecPtr ret, bool ret_ref, const Token& tk);
    std::vector<std::unique_ptr<Param>> parse_params(bool* varargs = nullptr, bool allow_ref = false);

    // ------------------------------------------------------ 类型

    bool is_type_start(const Token& t) const;
    bool is_module_prefix(const std::string& s) const;
    TypeSpecPtr parse_type();
    TypeSpecPtr parse_type_base();  // 不含数组后缀（供 `new T[n]` 使用）
    TypeSpecPtr type_suffixes(TypeSpecPtr spec, const Token& t);
    // 泛型：`<T, U: IList<int> & IComparable>`（声明处）与 `<int, long>`（使用处）
    std::vector<TypeParam> parse_type_params();
    std::vector<std::unique_ptr<TypeSpec>> parse_type_args();
    // 类型实参嵌套时把词法上的 `>>` 当作两个 `>`（`Vec<Vec<int>>`）
    Token expect_gt();

    // ------------------------------------------------------ 语句

    std::unique_ptr<Block> parse_block();
    std::vector<StmtPtr> parse_stmt();
    bool looks_like_local_decl();
    std::vector<StmtPtr> parse_local(bool is_const, bool is_auto);
    InitPtr parse_init();
    std::unique_ptr<Stmt> parse_if();
    std::unique_ptr<Stmt> parse_while();
    std::unique_ptr<Stmt> parse_for();
    std::unique_ptr<Stmt> parse_switch();

    // ------------------------------------------------------ 表达式

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

    ExprPtr make_binary(const std::string& op, ExprPtr left, ExprPtr right, const Token& tok);

    std::string filename_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    int group_counter_ = 0;  // 声明语句分组计数（auto 多声明符一致性检查用）
    // 已声明的本地类型名（struct / enum），用于消解 `T x` 与表达式的歧义
    std::unordered_set<std::string> type_names_;
    // `use` 引入的 unit 名，支持 `math.Point` 这种限定类型
    std::unordered_set<std::string> module_names_;
};

}  // namespace muc_detail
