// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// AST 类型参数替换（泛型实例化用）：把模板里的 `T` 就地替换为具体类型语法。
#pragma once

#include <string>
#include <unordered_map>

#include "ast.hpp"

namespace muc_detail {

// 类型参数名 → 实参类型语法（指向调用方持有的 TypeSpec，替换时会深拷贝）
using TypeBindings = std::unordered_map<std::string, const TypeSpec*>;

void subst_type_spec(TypeSpec& s, const TypeBindings& b);
void subst_expr(Expr& e, const TypeBindings& b);
void subst_init(Init& i, const TypeBindings& b);
void subst_stmt(Stmt& s, const TypeBindings& b);
void subst_block(Block& blk, const TypeBindings& b);

// 替换声明里的全部类型语法（字段、参数、返回、方法体局部变量、cast/sizeof/new）。
// 不负责清空 `type_params`，由实例化器决定。
void subst_decl(Decl& d, const TypeBindings& b);

}  // namespace muc_detail
