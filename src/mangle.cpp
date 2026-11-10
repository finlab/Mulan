// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

#include "mangle.hpp"

#include "ast.hpp"

std::string type_code(const TypePtr& t) {
    if (!t) return "v";
    switch (t->kind) {
        case TypeKind::Void: return "v";
        case TypeKind::Prim: return t->name;
        case TypeKind::String: return "str";
        case TypeKind::Ptr: return "p_" + type_code(t->elem);
        case TypeKind::Array: return "a" + std::to_string(t->array_size) + "_" + type_code(t->elem);
        case TypeKind::Slice:
            return "s" + std::to_string(t->rank) + "_" + type_code(t->elem);
        case TypeKind::Struct: return "S_" + t->unit + "_" + t->name;
        case TypeKind::Enum: return "E_" + t->unit + "_" + t->name;
        case TypeKind::Class: return "C_" + t->unit + "_" + t->name;
        case TypeKind::Interface: return "I_" + t->unit + "_" + t->name;
        case TypeKind::Module: return "m_" + t->unit;
        case TypeKind::Null: return "null";
        case TypeKind::Range: return "rng";
    }
    return "x";
}

std::string op_code(const std::string& sym) {
    if (sym == "+") return "add";
    if (sym == "-") return "sub";
    if (sym == "*") return "mul";
    if (sym == "/") return "div";
    if (sym == "%") return "mod";
    if (sym == "<") return "lt";
    if (sym == "<=") return "le";
    if (sym == ">") return "gt";
    if (sym == ">=") return "ge";
    if (sym == "==") return "eq";
    if (sym == "!=") return "ne";
    if (sym == "&") return "band";
    if (sym == "|") return "bor";
    if (sym == "^") return "bxor";
    if (sym == "<<") return "shl";
    if (sym == ">>") return "shr";
    if (sym == "[]") return "idx";
    if (sym == "[]=") return "idxset";
    return "op";
}

std::string func_code_name(const std::string& name) {
    const std::string prefix = "operator";
    if (name.rfind(prefix, 0) == 0) return "op_" + op_code(name.substr(prefix.size()));
    return name;
}

std::string mangle_unit(const std::string& unit, const std::string& name) {
    return "mu_" + unit + "_" + name;
}

std::string func_cname(const std::string& unit, FuncDecl* f) {
    if (f->is_extern) return f->name;
    std::string out = mangle_unit(unit, func_code_name(f->name));
    for (auto& t : f->pty) out += "__" + type_code(t);
    return out;
}

std::string method_cname(const std::string& unit, const std::string& owner,
                         const std::string& name, const std::vector<TypePtr>& params) {
    std::string out = "mu_" + unit + "_M" + owner + "_" + name;
    for (auto& t : params) out += "__" + type_code(t);
    return out;
}

std::string ctor_cname(const std::string& unit, const std::string& owner,
                       const std::vector<TypePtr>& params) {
    std::string out = "mu_" + unit + "_C" + owner;
    for (auto& t : params) out += "__" + type_code(t);
    return out;
}

std::string dtor_cname(const std::string& unit, const std::string& owner) {
    return "mu_" + unit + "_D" + owner;
}

std::string class_desc_name(const std::string& unit, const std::string& owner) {
    return "mu_" + unit + "_desc_" + owner;
}

std::string class_vtable_name(const std::string& unit, const std::string& owner) {
    return "mu_" + unit + "_vt_" + owner;
}
