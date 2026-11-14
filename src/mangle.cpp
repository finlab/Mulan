// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

#include "mangle.hpp"

#include "ast.hpp"

// Symbol name sanitization: characters like `<`, `>`, `,` and spaces in a generic instance name (like `Box<int>`) cannot appear in symbols
static std::string ident_safe(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            out += static_cast<char>(c);
        } else {
            out += '_';
        }
    }
    return out;
}

std::string type_code(const TypePtr& t) {
    if (!t) return "v";
    switch (t->kind) {
        case TypeKind::Void: return "v";
        case TypeKind::Prim: return t->name;
        case TypeKind::Ptr: return "p_" + type_code(t->elem);
        case TypeKind::Ref: return "r_" + type_code(t->elem);
        case TypeKind::Array: return "a" + std::to_string(t->array_size) + "_" + type_code(t->elem);
        case TypeKind::Slice:
            return "s" + std::to_string(t->rank) + "_" + type_code(t->elem);
        case TypeKind::Struct: return "S_" + t->unit + "_" + ident_safe(t->name);
        case TypeKind::Enum: return "E_" + t->unit + "_" + ident_safe(t->name);
        case TypeKind::Class: return "C_" + t->unit + "_" + ident_safe(t->name);
        case TypeKind::Interface: return "I_" + t->unit + "_" + ident_safe(t->name);
        case TypeKind::Module: return "m_" + t->unit;
        case TypeKind::Null: return "null";
        case TypeKind::Range: return "rng";
        case TypeKind::Func: {
            std::string s = "fn";
            for (const auto& p : t->fparams) s += "_" + type_code(p);
            s += "__" + type_code(t->fret);
            return s;
        }
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
    return ident_safe(name);  // `f<int>` → `f_int_` (keeps generic instances unique in symbol names)
}

std::string mangle_unit(const std::string& unit, const std::string& name) {
    return "mu_" + unit + "_" + name;
}

std::string func_cname(const std::string& unit, FuncDecl* f) {
    if (f->is_extern) return f->name;
    std::string out = mangle_unit(unit, func_code_name(f->name));
    for (size_t i = 0; i < f->pty.size(); ++i) {
        // A borrowed parameter is T* on the C side and must be distinguished in the symbol name, otherwise f(T) and f(ref T) would collide
        const bool by_ref = i < f->pty_ref.size() && f->pty_ref[i];
        out += by_ref ? "__r" : "__";
        out += type_code(f->pty[i]);
    }
    return out;
}

std::string method_cname(const std::string& unit, const std::string& owner,
                         const std::string& name, const std::vector<TypePtr>& params) {
    std::string out = "mu_" + unit + "_M" + ident_safe(owner) + "_" + func_code_name(name);
    for (auto& t : params) out += "__" + type_code(t);
    return out;
}

std::string ctor_cname(const std::string& unit, const std::string& owner,
                       const std::vector<TypePtr>& params) {
    std::string out = "mu_" + unit + "_C" + ident_safe(owner);
    for (auto& t : params) out += "__" + type_code(t);
    return out;
}

std::string dtor_cname(const std::string& unit, const std::string& owner) {
    return "mu_" + unit + "_D" + ident_safe(owner);
}

std::string destroy_cname(const std::string& unit, const std::string& owner) {
    return "mu_" + unit + "_destroy_" + ident_safe(owner);
}

std::string class_desc_name(const std::string& unit, const std::string& owner) {
    return "mu_" + unit + "_desc_" + ident_safe(owner);
}

std::string class_vtable_name(const std::string& unit, const std::string& owner) {
    return "mu_" + unit + "_vt_" + ident_safe(owner);
}

std::string unit_inited_cname(const std::string& unit) { return "muc$unit$inited$" + unit; }

std::string unit_init_cname(const std::string& unit) { return "muc$unit$init$" + unit; }

std::string unit_fini_cname(const std::string& unit) { return "muc$unit$fini$" + unit; }
