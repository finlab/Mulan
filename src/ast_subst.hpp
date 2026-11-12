// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// AST type parameter substitution (used for generic instantiation): replaces `T` in the template in place with concrete type syntax.
#pragma once

#include <string>
#include <unordered_map>

#include "ast.hpp"

namespace muc_detail {

// Type parameter name → type argument syntax (points at the TypeSpec owned by the caller; it is deep-copied during substitution)
using TypeBindings = std::unordered_map<std::string, const TypeSpec*>;

void subst_type_spec(TypeSpec& s, const TypeBindings& b);
void subst_expr(Expr& e, const TypeBindings& b);
void subst_init(Init& i, const TypeBindings& b);
void subst_stmt(Stmt& s, const TypeBindings& b);
void subst_block(Block& blk, const TypeBindings& b);

// Substitutes all type syntax in a declaration (fields, parameters, return type, local variables in method bodies, cast/sizeof/new).
// Clearing `type_params` is not handled here; the instantiator decides.
void subst_decl(Decl& d, const TypeBindings& b);

}  // namespace muc_detail
