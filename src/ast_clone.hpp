// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// AST deep copy (used for generic instantiation).
//
// Only **syntactic fields** are copied; the results filled in by sema (Expr::ty, Decl::rty, *_cname, Sym,
// MethodSig pointers, etc.) are all reset to their default values — the instantiated result goes through the checks again.
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

// Clones any declaration by dynamic type; returns nullptr for an unknown type.
std::unique_ptr<Decl> clone_decl(const Decl& d);

}  // namespace muc_detail
