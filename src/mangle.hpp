// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Symbol mangling: shared by sema and codegen so that both sides compute the same name.
#pragma once

#include <string>
#include <vector>

#include "types.hpp"

struct FuncDecl;

// Type encoding (lets parameter/return types take part in the symbol name to avoid overload conflicts)
std::string type_code(const TypePtr& t);

// Operator symbol -> identifier fragment
std::string op_code(const std::string& sym);

// Free function name (without the unit prefix)
std::string func_code_name(const std::string& name);

// Complete symbol name
std::string mangle_unit(const std::string& unit, const std::string& name);
std::string func_cname(const std::string& unit, FuncDecl* f);

// Method / constructor / destructor
std::string method_cname(const std::string& unit, const std::string& owner,
                         const std::string& name, const std::vector<TypePtr>& params);
std::string ctor_cname(const std::string& unit, const std::string& owner,
                       const std::vector<TypePtr>& params);
std::string dtor_cname(const std::string& unit, const std::string& owner);
// RC destroy entry (user destructor + field release + free), used by the runtime and by exports
std::string destroy_cname(const std::string& unit, const std::string& owner);

// Global symbols for the class descriptor and vtable
std::string class_desc_name(const std::string& unit, const std::string& owner);
std::string class_vtable_name(const std::string& unit, const std::string& owner);

// 9.1: per-unit guard/init/fini hooks. `$` cannot appear in a user identifier, so these names
// cannot collide with a mangled user symbol.
std::string unit_inited_cname(const std::string& unit);
std::string unit_init_cname(const std::string& unit);
std::string unit_fini_cname(const std::string& unit);
