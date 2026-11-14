// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Mulan type model
//
// Design: TypePtr points to immutable value objects; a Struct's fields are created as a placeholder first and filled in later
// (mirroring the two-stage construction of StructType in the Python version), so no const is used here.
#include "types.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------- Built-in type table
// One-to-one with the Python INT_BITS / INT_SIGNED (the order is the order of prim_names()).
struct IntInfo {
    const char* name;
    int bits;
    bool is_signed;
};

constexpr std::array<IntInfo, 10> kInts{{
    {"byte", 8, false},
    {"char", 8, true},
    {"short", 16, true},
    {"ushort", 16, false},
    {"int", 32, true},
    {"uint", 32, false},
    {"long", 64, true},
    {"ulong", 64, false},
    {"usize", 64, false},
    {"isize", 64, true},
}};

const IntInfo* find_int(const std::string& n) {
    for (const auto& p : kInts) {
        if (n == p.name) return &p;
    }
    return nullptr;
}

bool is_float_name(const std::string& n) { return n == "float" || n == "double"; }

// Fallback for the type text: the elem of a pointer/array is theoretically never null, but the error path must not crash.
std::string elem_str(const TypePtr& t) { return t ? t->str() : std::string("void"); }

}  // namespace

// ---------------------------------------------------------------- Factory functions

TypePtr Type::make_void() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Void;
    return t;
}

TypePtr Type::make_prim(const std::string& name) {
    // Built-in types are immutable, so caching them by name lets `prim("int")` and t_int() share the same object.
    // The compiler is single-threaded, so a function-local static needs no locking.
    static std::unordered_map<std::string, TypePtr> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Prim;
    t->name = name;
    cache.emplace(name, t);
    return t;
}


TypePtr Type::make_ptr(const TypePtr& elem, bool is_const) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Ptr;
    t->elem = elem;
    t->is_const = is_const;
    return t;
}

TypePtr Type::make_ref(const TypePtr& elem, bool is_const) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Ref;
    t->elem = elem;
    t->is_const = is_const;
    return t;
}

TypePtr Type::make_array(const TypePtr& elem, size_t size) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Array;
    t->elem = elem;
    t->array_size = size;
    return t;
}

TypePtr Type::make_slice(const TypePtr& elem, bool is_const, int rank) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Slice;
    t->elem = elem;
    t->is_const = is_const;
    t->rank = rank < 1 ? 1 : rank;
    return t;
}

TypePtr Type::make_struct(const std::string& unit, const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Struct;
    t->unit = unit;
    t->name = name;
    return t;
}

TypePtr Type::make_enum(const std::string& unit, const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Enum;
    t->unit = unit;
    t->name = name;
    return t;
}

TypePtr Type::make_class(const std::string& unit, const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Class;
    t->unit = unit;
    t->name = name;
    return t;
}

TypePtr Type::make_interface(const std::string& unit, const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Interface;
    t->unit = unit;
    t->name = name;
    return t;
}

TypePtr Type::make_module(const std::string& unit) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Module;
    t->unit = unit;
    return t;
}

TypePtr Type::make_null() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Null;
    return t;
}

TypePtr Type::make_range() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Range;
    return t;
}

TypePtr Type::make_func(const std::vector<TypePtr>& params, const TypePtr& ret) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Func;
    t->fparams = params;
    t->fret = ret;
    return t;
}

// ---------------------------------------------------------------- Textual form

std::string Type::str() const {
    switch (kind) {
        case TypeKind::Void:
            return "void";
        case TypeKind::Prim:
            return is_const ? "const " + name : name;

        case TypeKind::Ptr: {
            std::string base = elem_str(elem) + "*";
            return is_const ? "const " + base : base;
        }
        case TypeKind::Ref: {
            std::string base = elem_str(elem);
            return is_const ? "ref const " + base : "ref " + base;
        }
        case TypeKind::Array: {
            // A nested static array is displayed as T[m,n,...]
            std::vector<std::string> dims;
            const Type* cur = this;
            while (cur->kind == TypeKind::Array) {
                dims.push_back(std::to_string(cur->array_size));
                cur = cur->elem.get();
            }
            std::string base = cur ? cur->str() : std::string("void");
            std::string out = base + "[";
            for (size_t i = 0; i < dims.size(); ++i) {
                if (i) out += ",";
                out += dims[i];
            }
            return out + "]";
        }
        case TypeKind::Slice: {
            std::string base = elem_str(elem) + "[" + std::string(rank - 1, ',') + "]";
            return is_const ? "const " + base : base;
        }
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Class:
        case TypeKind::Interface:
            return unit.empty() ? name : unit + "." + name;
        case TypeKind::Module:
            return unit;
        case TypeKind::Null:
            return "null";
        case TypeKind::Range:
            return "range";
        case TypeKind::Func: {
            std::string out = "(";
            for (size_t i = 0; i < fparams.size(); ++i) {
                if (i) out += ", ";
                out += fparams[i] ? fparams[i]->str() : std::string("void");
            }
            out += ") => ";
            out += fret ? fret->str() : std::string("void");
            return out;
        }
    }
    return "?";
}

// ---------------------------------------------------------------- Predicates

bool Type::is_integer() const {
    if (kind == TypeKind::Enum) return true;  // an enum is backed by int
    if (kind != TypeKind::Prim) return false;
    return find_int(name) != nullptr;
}

bool Type::is_float() const { return kind == TypeKind::Prim && is_float_name(name); }

bool Type::is_bool() const { return kind == TypeKind::Prim && name == "bool"; }

bool Type::is_numeric() const { return is_integer() || is_float(); }

int Type::bits() const {
    if (kind != TypeKind::Prim) return 0;  // an enum has no bit width concept (the Python version raises AttributeError here)
    if (is_float()) return name == "float" ? 32 : 64;
    if (const IntInfo* p = find_int(name)) return p->bits;
    return 0;
}

bool Type::is_signed() const {
    if (kind != TypeKind::Prim) return true;  // consistent with INT_SIGNED.get(name, True)
    if (const IntInfo* p = find_int(name)) return p->is_signed;
    return true;
}

bool Type::needs_cleanup() const {
    switch (kind) {
        case TypeKind::Class:
        case TypeKind::Interface: return true;
        case TypeKind::Struct:
            if (has_dtor) return true;
            for (const auto& f : fields) {
                if (f.weak) continue;
                if (f.type && f.type->needs_cleanup()) return true;
            }
            return false;
        case TypeKind::Array:
            return elem && elem->needs_cleanup();
        case TypeKind::Slice: return true;  // dynamic array: the handle holds an RC
        default: return false;
    }
}

TypePtr Type::field_type(const std::string& fname) const {
    if (kind != TypeKind::Struct && kind != TypeKind::Class) return nullptr;
    for (const auto& f : fields) {
        if (f.name == fname) return f.type;
    }
    return nullptr;
}

int Type::field_index(const std::string& fname) const {
    if (kind != TypeKind::Struct && kind != TypeKind::Class) return -1;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == fname) return static_cast<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------- Layout
//
// The only implementation of packed / align(N) (codegen's alignOf / sizeOf delegate here).
// The recursion has a depth limit so that a malformed type (a nonexistent by-value cycle) cannot blow the compiler's stack.

namespace {

size_t align_up(size_t v, size_t a) { return a ? (v + a - 1) / a * a : v; }

size_t align_impl(const TypePtr& t, int depth) {
    if (!t || depth > 64) return 1;
    switch (t->kind) {
        case TypeKind::Void: return 1;
        case TypeKind::Prim: {
            const std::string& n = t->name;
            if (n == "bool" || n == "byte" || n == "char") return 1;
            if (n == "short" || n == "ushort") return 2;
            if (n == "int" || n == "uint" || n == "float") return 4;
            return 8;
        }
        case TypeKind::Ptr:
        case TypeKind::Slice: return 8;
        case TypeKind::Enum: return 4;
        case TypeKind::Array: return align_impl(t->elem, depth + 1);
        case TypeKind::Struct: {
            if (t->is_packed) return t->align_req > 0 ? static_cast<size_t>(t->align_req) : 1;
            size_t natural = 1;
            for (const auto& f : t->fields) natural = std::max(natural, align_impl(f.type, depth + 1));
            // align(N) requires N >= the natural alignment (sema already validates it); taking the max here prevents it from collapsing
            if (t->align_req > 0) return std::max(natural, static_cast<size_t>(t->align_req));
            return natural;
        }
        default: return 8;
    }
}

size_t size_impl(const TypePtr& t, int depth) {
    if (!t || depth > 64) return 0;
    switch (t->kind) {
        case TypeKind::Void: return 0;
        case TypeKind::Prim: {
            const std::string& n = t->name;
            if (n == "bool" || n == "byte" || n == "char") return 1;
            if (n == "short" || n == "ushort") return 2;
            if (n == "int" || n == "uint" || n == "float") return 4;
            return 8;
        }
        case TypeKind::Ptr:
        case TypeKind::Slice: return 8;
        case TypeKind::Interface: return 16;
        case TypeKind::Enum: return 4;
        case TypeKind::Array: return size_impl(t->elem, depth + 1) * t->array_size;
        case TypeKind::Struct: {
            if (t->is_union) {
                // union: all members at offset 0, size = max(member size)
                size_t maxsz = 0, natural = 1;
                for (const auto& f : t->fields) {
                    natural = std::max(natural, align_impl(f.type, depth + 1));
                    maxsz = std::max(maxsz, size_impl(f.type, depth + 1));
                }
                if (t->is_packed) {
                    if (t->align_req > 0) maxsz = align_up(maxsz, static_cast<size_t>(t->align_req));
                    return maxsz;
                }
                size_t a = natural;
                if (t->align_req > 0) a = std::max(a, static_cast<size_t>(t->align_req));
                return align_up(maxsz, a);
            }
            size_t off = 0, natural = 1;
            for (const auto& f : t->fields) {
                size_t fa = align_impl(f.type, depth + 1);
                natural = std::max(natural, fa);
                // packed: the fields are accumulated contiguously, with no internal padding
                if (!t->is_packed) off = align_up(off, fa);
                off += size_impl(f.type, depth + 1);
            }
            if (t->is_packed) {
                // No tail padding; only align(N) rounds up to a multiple of N
                if (t->align_req > 0) off = align_up(off, static_cast<size_t>(t->align_req));
                return off;
            }
            size_t a = natural;
            if (t->align_req > 0) a = std::max(a, static_cast<size_t>(t->align_req));
            return align_up(off, a);
        }
        default: return 8;
    }
}

}  // namespace

size_t type_align(const TypePtr& t) { return align_impl(t, 0); }
size_t type_size(const TypePtr& t) { return size_impl(t, 0); }

uint64_t type_member_offset(const TypePtr& t, size_t idx) {
    if (!t) return 0;
    if (t->is_union) return 0;
    uint64_t off = 0;
    for (size_t i = 0; i <= idx && i < t->fields.size(); ++i) {
        const TypePtr& ft = t->fields[i].type;
        const uint64_t fa = type_align(ft);
        if (!t->is_packed) off = (off + fa - 1) / fa * fa;
        if (i == idx) return off;
        off += type_size(ft);
    }
    return 0;
}

// ---------------------------------------------------------------- Singletons

TypePtr t_void() {
    static TypePtr t = Type::make_void();
    return t;
}


TypePtr t_int() {
    static TypePtr t = Type::make_prim("int");
    return t;
}

TypePtr t_bool() {
    static TypePtr t = Type::make_prim("bool");
    return t;
}

TypePtr t_null() {
    static TypePtr t = Type::make_null();
    return t;
}

TypePtr t_range() {
    static TypePtr t = Type::make_range();
    return t;
}

// ---------------------------------------------------------------- Built-in type names

const std::vector<std::string>& prim_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        v.reserve(kInts.size() + 3);
        for (const auto& p : kInts) v.emplace_back(p.name);
        v.emplace_back("float");
        v.emplace_back("double");
        v.emplace_back("bool");
        return v;
    }();
    return names;
}

bool is_prim_name(const std::string& n) {
    for (const auto& s : prim_names()) {
        if (s == n) return true;
    }
    return false;
}

// ---------------------------------------------------------------- Type equality

bool same_type(const TypePtr& a, const TypePtr& b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TypeKind::Void:
        case TypeKind::Null:
        case TypeKind::Range:
            return true;
        case TypeKind::Prim:
            return a->name == b->name;
        case TypeKind::Ptr:
        case TypeKind::Slice:
        case TypeKind::Ref:
            // C model: const is only a view qualifier and does not change type equality
            return a->rank == b->rank && same_type(a->elem, b->elem);
        case TypeKind::Array:
            return a->array_size == b->array_size && same_type(a->elem, b->elem);
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Class:
        case TypeKind::Interface:
            // struct/enum/class/interface are compared by (unit, name)
            return a->unit == b->unit && a->name == b->name;
        case TypeKind::Module:
            return a->unit == b->unit;
        case TypeKind::Func:
            if (!same_type(a->fret, b->fret)) return false;
            if (a->fparams.size() != b->fparams.size()) return false;
            for (size_t i = 0; i < a->fparams.size(); ++i) {
                if (!same_type(a->fparams[i], b->fparams[i])) return false;
            }
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- Conversion rules

bool is_class_type(const TypePtr& t) { return t && t->kind == TypeKind::Class; }
bool is_interface_type(const TypePtr& t) { return t && t->kind == TypeKind::Interface; }
bool is_reference_type(const TypePtr& t) { return is_class_type(t) || is_interface_type(t); }
bool is_borrow_type(const TypePtr& t) { return t && t->kind == TypeKind::Ref; }
TypePtr referent_of(const TypePtr& t) { return is_borrow_type(t) ? t->elem : t; }

bool is_subclass_of(const TypePtr& sub, const TypePtr& base) {
    if (!sub || !base || base->kind != TypeKind::Class) return false;
    TypePtr cur = sub;
    int guard = 0;
    while (cur && cur->kind == TypeKind::Class && guard++ < 1024) {
        if (same_type(cur, base)) return true;
        cur = cur->base;
    }
    return false;
}

static bool iface_extends(const TypePtr& a, const TypePtr& b, int depth) {
    if (!a || !b || depth > 256) return false;
    if (same_type(a, b)) return true;
    for (auto& i : a->interfaces) {
        if (iface_extends(i, b, depth + 1)) return true;
    }
    return false;
}

bool implements_interface(const TypePtr& t, const TypePtr& iface) {
    if (!t || !iface || iface->kind != TypeKind::Interface) return false;
    if (t->kind == TypeKind::Interface) return iface_extends(t, iface, 0);
    if (t->kind != TypeKind::Class) return false;

    TypePtr cur = t;
    int guard = 0;
    while (cur && cur->kind == TypeKind::Class && guard++ < 1024) {
        for (auto& i : cur->interfaces) {
            if (iface_extends(i, iface, 0)) return true;
        }
        cur = cur->base;
    }
    return false;
}

bool widens_int(const TypePtr& src, const TypePtr& dst) {
    if (!src || !dst) return false;
    if (src->kind != TypeKind::Prim || dst->kind != TypeKind::Prim) return false;
    if (!src->is_integer() || !dst->is_integer()) return false;
    if (src->name == dst->name) return true;

    const int sb = src->bits();
    const int db = dst->bits();
    const bool ss = src->is_signed();
    const bool ds = dst->is_signed();

    if (ss && ds) return db > sb;
    if (!ss && !ds) return db > sb;
    if (!ss && ds) return db > sb;
    return false;  // signed -> unsigned: never allowed implicitly
}

bool implicitly_convertible(const TypePtr& src, const TypePtr& dst) {
    // C model: const can only be relaxed, never tightened (same_type ignores const, so it must be rejected first)
    if (src && dst && src->kind == dst->kind) {
        if (src->kind == TypeKind::Ptr || src->kind == TypeKind::Slice) {
            if (src->elem && dst->elem && src->elem->is_const && !dst->elem->is_const) {
                return false;
            }
        }
    }
    if (same_type(src, dst)) return true;
    if (!src || !dst) return false;

    // null -> pointer / slice / class / interface / function value
    if (src->kind == TypeKind::Null) {
        return dst->kind == TypeKind::Ptr || dst->kind == TypeKind::Slice ||
               dst->kind == TypeKind::Class ||
               dst->kind == TypeKind::Interface || dst->kind == TypeKind::Func;
    }

    // A function value is a pointer: it converts to `void*` implicitly (the reverse needs `as`, 3.16)
    if (src->kind == TypeKind::Func) {
        return dst->kind == TypeKind::Ptr && dst->elem && dst->elem->kind == TypeKind::Void;
    }

    // class / interface subtypes (single inheritance + interfaces)
    if (is_subclass_of(src, dst)) return true;
    if (implements_interface(src, dst)) return true;

    // Pointer: T* -> const T*; dropping const requires an explicit as
    if (src->kind == TypeKind::Ptr && dst->kind == TypeKind::Ptr) {
        if (src->elem && src->elem->is_const && !(dst->elem && dst->elem->is_const)) {
            return false;
        }
        if (dst->elem && dst->elem->kind == TypeKind::Void) return true;
        return same_type(src->elem, dst->elem);
    }

    // Array -> pointer (C99 6.3.2.1 decay): the array's first element address; same element type,
    // const may only be relaxed. `void*` is accepted like any other pointer target.
    if (src->kind == TypeKind::Array && dst->kind == TypeKind::Ptr) {
        if (!dst->elem) return false;
        if (dst->elem->kind == TypeKind::Void) return true;
        if (src->elem && src->elem->is_const && !dst->elem->is_const) return false;
        return same_type(src->elem, dst->elem);
    }

    // Array -> slice
    if (src->kind == TypeKind::Array && dst->kind == TypeKind::Slice) {
        return same_type(src->elem, dst->elem);
    }

    // Slice: T[] -> const T[]; the reverse requires an explicit as
    if (src->kind == TypeKind::Slice && dst->kind == TypeKind::Slice) {
        if (!same_type(src->elem, dst->elem)) return false;
        if (src->elem && src->elem->is_const && !(dst->elem && dst->elem->is_const)) {
            return false;
        }
        return true;
    }

    // Numeric: only a widening that does not lose range is allowed
    if (src->kind == TypeKind::Prim && dst->kind == TypeKind::Prim) {
        if (src->is_integer() && dst->is_integer()) return widens_int(src, dst);
        if (src->is_float() && dst->is_float()) {
            return dst->name == "double" && src->name == "float";
        }
        return false;
    }

    // An enum is a nominal type: there is no implicit conversion to or from an integer, and both directions require an explicit `as`.
    // (See SPEC §3.5; placing the check here means that parameter/return value/assignment/literal adaptation all reject enums)

    return false;
}

bool promote_int_literal(long long v, const TypePtr& dst) {
    if (!dst) return false;
    // an enum is not an implicit target for an integer: an explicit `as` is required
    if (dst->kind != TypeKind::Prim) return false;
    if (dst->is_float()) return true;
    if (!dst->is_integer()) return false;

    const int bits = dst->bits();
    if (bits <= 0) return false;
    if (bits >= 64) {
        // A signed 64-bit value can represent every long long; an unsigned 64-bit value only accepts non-negative values.
        return dst->is_signed() ? true : v >= 0;
    }
    if (dst->is_signed()) {
        const long long lo = -(1LL << (bits - 1));
        const long long hi = 1LL << (bits - 1);
        return v >= lo && v < hi;
    }
    return v >= 0 && static_cast<unsigned long long>(v) < (1ULL << bits);
}

TypePtr usual_arith(const TypePtr& a_in, const TypePtr& b_in) {
    if (!a_in || !b_in) return nullptr;
    // An enum does not take part in arithmetic/bitwise operations (except comparisons of the same type, see check_binary in sema).
    if (a_in->kind == TypeKind::Enum || b_in->kind == TypeKind::Enum) return nullptr;
    TypePtr a = a_in;
    TypePtr b = b_in;
    if (!a->is_numeric() || !b->is_numeric()) return nullptr;
    if (a->kind != TypeKind::Prim || b->kind != TypeKind::Prim) return nullptr;

    if (a->is_float() || b->is_float()) {
        if (a->name == "double" || b->name == "double") return Type::make_prim("double");
        return Type::make_prim("float");
    }

    // Anything narrower than int is promoted to int (C semantics: 8/16-bit values are all representable in int)
    if (a->bits() < 32) a = t_int();
    if (b->bits() < 32) b = t_int();

    if (a->bits() != b->bits()) return a->bits() > b->bits() ? a : b;

    // Same width: the first one is taken when both are signed or both are unsigned; for mixed signs C semantics takes the unsigned one
    if (a->is_signed() && b->is_signed()) return a;
    if (!a->is_signed() && !b->is_signed()) return a;
    return b->is_signed() ? a : b;
}
