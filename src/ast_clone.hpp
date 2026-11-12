// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// AST 深拷贝（泛型实例化用）。
//
// 只复制**语法字段**；sema 填充的结果（Expr::ty、Decl::rty、*_cname、Sym、
// MethodSig 指针等）一律重置为默认值 —— 实例化产物会重新跑一遍检查。
#pragma once

#include <memory>

#include "ast.hpp"

namespace muc_detail {

TypeSpecPtr clone_type_spec(const TypeSpec& s);

ExprPtr clone_expr(const Expr& e);
InitPtr clone_init(const Init& i);
StmtPtr clone_stmt(const Stmt& s);
std::unique_ptr<Block> clone_block(const Block& b);

std::unique_ptr<Param> clone_param(const Param& p);
std::unique_ptr<MethodDecl> clone_method(const MethodDecl& m);
std::unique_ptr<CtorDecl> clone_ctor(const CtorDecl& c);
std::unique_ptr<DtorDecl> clone_dtor(const DtorDecl& d);
std::unique_ptr<ConstMember> clone_const_member(const ConstMember& c);

// 按动态类型克隆任意声明；未知类型返回 nullptr。
std::unique_ptr<Decl> clone_decl(const Decl& d);

}  // namespace muc_detail
