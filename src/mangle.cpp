// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong
//
// owns:      symbol name mangling (units, types, generics, and the unit init/fini hooks)
// not:       semantic decisions; it only encodes names
// pass:      used by every emitting pass
// concept:   mangling

#include "mangle.hpp"

#include "ast.hpp"

// Symbol name sanitization. It must be **reversible**: `_` is doubled and every other
// non-alphanumeric byte becomes `_XX` (uppercase hex), so `Box<int>` and `Box_int_` no longer
// collapse to the same symbol (issue #35). `$` separates the components downstream and cannot be
// produced here, which makes the concatenations below injective.
static std::string ident_safe(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (c == '_') {
            out += "__";
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out += static_cast<char>(c);
        } else {
            out += '_';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string type_code_impl(const TypePtr& t, bool top);

static std::string type_code_impl(const TypePtr& t, bool top) {
    if (!t) return "v";
    const bool cv = t->is_const && !top;  // top-level const is ignored in signatures (C++ rule)
    switch (t->kind) {
        case TypeKind::Void: return "v";
        case TypeKind::Prim: return (cv ? std::string("K$") : std::string()) + t->name;
        case TypeKind::Ptr: return (cv ? std::string("K$") : std::string()) + "p$" + type_code_impl(t->elem, false);
        case TypeKind::Ref: return (cv ? std::string("K$") : std::string()) + "r$" + type_code_impl(t->elem, false);
        case TypeKind::Array: return (cv ? std::string("K$") : std::string()) + "a" + std::to_string(t->array_size) + "$" + type_code_impl(t->elem, false);
        case TypeKind::Slice:
            return (cv ? std::string("K$") : std::string()) + "s" + std::to_string(t->rank) + "$" + type_code_impl(t->elem, false);
        case TypeKind::Struct: return (cv ? std::string("K$") : std::string()) + "S$" + t->unit + "$" + ident_safe(t->name);
        case TypeKind::Enum: return (cv ? std::string("K$") : std::string()) + "E$" + t->unit + "$" + ident_safe(t->name);
        case TypeKind::Class: return (cv ? std::string("K$") : std::string()) + "C$" + t->unit + "$" + ident_safe(t->name);
        case TypeKind::Interface: return (cv ? std::string("K$") : std::string()) + "I$" + t->unit + "$" + ident_safe(t->name);
        case TypeKind::Sum: return (cv ? std::string("K$") : std::string()) + "U$" + t->unit + "$" + ident_safe(t->name);
        case TypeKind::Module: return "m$" + t->unit;
        case TypeKind::Null: return "null";
        case TypeKind::Range: return "rng";
        case TypeKind::Func:
        case TypeKind::Delegate: {
            // The arity is part of the code, so the parameter/return boundary is unambiguous.
            std::string s = t->kind == TypeKind::Delegate ? "dlg" : "fn";
            s += std::to_string(t->fparams.size()) + "$";
            for (const auto& p : t->fparams) s += type_code_impl(p, true) + "$";
            s += type_code_impl(t->fret, true);
            return s;
        }
    }
    return "x";
}

std::string type_code(const TypePtr& t) { return type_code_impl(t, /*top=*/true); }

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
    return "mu$" + unit + "$" + name;
}

std::string func_cname(const std::string& unit, FuncDecl* f) {
    if (f->is_extern) return f->name;
    std::string out = mangle_unit(unit, func_code_name(f->name));
    for (size_t i = 0; i < f->pty.size(); ++i) {
        // A borrowed parameter is T* on the C side and must be distinguished in the symbol name, otherwise f(T) and f(ref T) would collide
        const bool by_ref = i < f->pty_ref.size() && f->pty_ref[i];
        out += by_ref ? "$R$" : "$";
        out += type_code(f->pty[i]);
    }
    return out;
}

std::string method_cname(const std::string& unit, const std::string& owner,
                         const std::string& name, const std::vector<TypePtr>& params,
                         bool is_const) {
    std::string out = "mu$" + unit + "$M" + ident_safe(owner) + "$" + func_code_name(name);
    if (is_const) out += "$K";
    for (auto& t : params) out += "$" + type_code(t);
    return out;
}

std::string ctor_cname(const std::string& unit, const std::string& owner,
                       const std::vector<TypePtr>& params) {
    std::string out = "mu$" + unit + "$C" + ident_safe(owner);
    for (auto& t : params) out += "$" + type_code(t);
    return out;
}

std::string dtor_cname(const std::string& unit, const std::string& owner) {
    return "mu$" + unit + "$D" + ident_safe(owner);
}

std::string destroy_cname(const std::string& unit, const std::string& owner) {
    return "mu$" + unit + "$destroy$" + ident_safe(owner);
}

std::string dtor_partial_cname(const std::string& unit, const std::string& owner) {
    return "mu$" + unit + "$D$partial$" + ident_safe(owner);
}
std::string destroy_partial_cname(const std::string& unit, const std::string& owner) {
    return "mu$" + unit + "$destroy$partial$" + ident_safe(owner);
}

std::string move_ctor_cname(const std::string& unit, const std::string& owner) {
    return "mu$" + unit + "$move$" + ident_safe(owner);
}

std::string copy_ctor_cname(const std::string& unit, const std::string& owner) {
    return "mu$" + unit + "$copy$" + ident_safe(owner);
}

std::string class_desc_name(const std::string& unit, const std::string& owner) {
    return "mu$" + unit + "$desc$" + ident_safe(owner);
}

std::string class_vtable_name(const std::string& unit, const std::string& owner) {
    return "mu$" + unit + "$vt$" + ident_safe(owner);
}

std::string unit_inited_cname(const std::string& unit) { return "muc$unit$inited$" + unit; }

std::string unit_init_cname(const std::string& unit) { return "muc$unit$init$" + unit; }

std::string unit_fini_cname(const std::string& unit) { return "muc$unit$fini$" + unit; }
