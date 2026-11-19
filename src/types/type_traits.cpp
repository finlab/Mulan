// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Mulan type model — the predicate family (definitions of Type members).
//
// owns:      the classification predicates the passes share: integer / float / bool / numeric, bit
//            width and signedness, and the value-semantics queries needs_cleanup / needs_dtor /
//            trivially_copyable / is_pod / needs_ctor_glue / copyable / has_user_ctor / movable /
//            needs_ctor
// not:      construction, `str()`, layout (types.cpp)
// pass:      used by every pass
// concept:   types, value-semantics
#include "types.hpp"

#include <array>
#include <string>

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

}  // namespace


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

// Integer-only predicate: reports whether the *integer* type is signed. Floats and non-primitive
// kinds answer `true` (parity with the Python prototype's INT_SIGNED.get(name, True)), so a caller
// that may see a float must decide the conversion itself instead of reading a sign off this call —
// see convertTo's float -> integer path, which is always a signed truncation.
bool Type::is_signed() const {
    if (kind != TypeKind::Prim) return true;  // consistent with INT_SIGNED.get(name, True)
    if (const IntInfo* p = find_int(name)) return p->is_signed;
    return true;  // float / double: no sign bit, callers must not rely on this
}

bool Type::needs_cleanup() const { return needs_dtor(); }

// A value needs destruction exactly when it owns an RC handle or has a user destructor. `trivially_copyable`
// is the complement of this, except for the kinds that can never be a value at all (Ref / Module / Range /
// Void), which are non-trivial on purpose so that nothing spreads them across the C boundary by accident.
bool Type::needs_dtor() const {
    switch (kind) {
        case TypeKind::Class:
        case TypeKind::Interface: return true;
        case TypeKind::Struct:
            if (has_dtor) return true;
            for (const auto& f : fields) {
                if (f.weak) continue;
                if (f.type && f.type->needs_dtor()) return true;
            }
            return false;
        case TypeKind::Array:
            return elem && elem->needs_dtor();
        case TypeKind::Slice: return true;  // dynamic array: the handle holds an RC
        case TypeKind::Delegate: return true;  // the environment is an RC object
        case TypeKind::Sum:
            // a sum owns its active payload: cleanup is needed when any alternative needs it
            for (const auto& f : sum_fields) {
                for (const auto& ft : f) {
                    if (ft && ft->needs_dtor()) return true;
                }
            }
            return false;
        default: return false;
    }
}

bool Type::trivially_copyable() const {
    switch (kind) {
        case TypeKind::Prim:
        case TypeKind::Ptr:
        case TypeKind::Enum:
        case TypeKind::Func:
        case TypeKind::Null: return true;
        case TypeKind::Struct:
            if (has_dtor) return false;
            for (const auto& f : fields) {
                // A weak field is registered in the weak registry by its address, so a bitwise copy
                // would leave a stale registration behind.
                if (f.weak) return false;
                if (f.type && !f.type->trivially_copyable()) return false;
            }
            return true;
        case TypeKind::Array:
            return elem && elem->trivially_copyable();
        case TypeKind::Sum:
            for (const auto& f : sum_fields) {
                for (const auto& ft : f) {
                    if (ft && !ft->trivially_copyable()) return false;
                }
            }
            return true;
        default: return false;  // Slice / Class / Interface / Delegate / Ref / Module / Range / Void
    }
}

// Arrays and sums have no constructor of their own, so their triviality comes from the element/payload.
bool Type::is_pod() const {
    switch (kind) {
        case TypeKind::Array: return elem && elem->is_pod();
        case TypeKind::Sum:
            for (const auto& f : sum_fields) {
                for (const auto& ft : f) {
                    if (ft && !ft->is_pod()) return false;
                }
            }
            return trivially_copyable();
        default: return trivially_copyable() && ctors.empty();
    }
}

// A nested value that declares a user destructor owns a resource the compiler does not understand, so it
// cannot ride on the host's copy/move glue: that glue reproduces reference-counted handles field by field
// but never invokes a user constructor of a nested value (agents/value-semantics.md §3). Handles
// (class / interface / delegate / slice) and reference-counted fields are unaffected: they are copied by
// retaining, not by running a constructor. The host must declare its own copy/move constructor
// (rule of three, §8.1) and assign through copy-and-swap.
bool Type::needs_ctor_glue() const {
    switch (kind) {
        case TypeKind::Struct:
            if (has_dtor) return true;
            for (const auto& f : fields) {
                if (f.weak) continue;
                if (f.type && f.type->needs_ctor_glue()) return true;
            }
            return false;
        case TypeKind::Array: return elem && elem->needs_ctor_glue();
        case TypeKind::Sum:
            for (const auto& f : sum_fields) {
                for (const auto& ft : f) {
                    if (ft && ft->needs_ctor_glue()) return true;
                }
            }
            return false;
        default: return false;
    }
}

// The copy axis is independent of the destruction axis: a struct with RC fields needs destruction but is
// copyable (the copy retains), while a struct with a user destructor owns nothing the compiler can copy,
// so it stays uncopyable until copy construction lands (agents/value-semantics.md §8).
bool Type::copyable() const {
    switch (kind) {
        case TypeKind::Struct:
            if (nocopy) return false;
            if (has_copy_ctor) return true;  // the user supplies the deep copy (§8.1)
            if (has_dtor) return false;
            for (const auto& f : fields) {
                if (f.weak) continue;
                if (!f.type) continue;
                if (f.type->needs_ctor_glue()) return false;
                if (!f.type->copyable()) return false;
            }
            return true;
        case TypeKind::Array:
            if (elem && elem->needs_ctor_glue()) return false;
            return elem && elem->copyable();
        case TypeKind::Sum:
            for (const auto& f : sum_fields) {
                for (const auto& ft : f) {
                    if (ft && !ft->copyable()) return false;
                }
            }
            return true;
        default: return true;  // class/interface/delegate/slice copy by retaining; scalars are trivial
    }
}

bool Type::has_user_ctor() const { return !ctors.empty(); }

// A value may be moved when the compiler can perform the transfer itself: copy the bytes and zero the
// source (which is exactly an ownership transfer for a handle, and a harmless copy for a scalar). A type
// with a user destructor owns something the compiler does not understand, so it must say how to transfer
// it (`T(move T other)`); `nocopy` opts out of the transfer as well, which is what makes an "immovable"
// value possible (agents/value-semantics.md §8.2, §8.9).
bool Type::movable() const {
    switch (kind) {
        case TypeKind::Class:
        case TypeKind::Interface:
        case TypeKind::Delegate:
        case TypeKind::Slice: return true;
        case TypeKind::Struct:
            // Every value is movable by default: the compiler moves it by copying the bits and emptying
            // the source (which is then not destroyed), and that is correct for raw pointers, RC handles
            // and nested destructor-bearing fields alike — the destination owns what it received and
            // nothing is left for the source to release. `nocopy` forbids copying only; `nomove` is the
            // explicit opt-out (the address is part of the value); a user move constructor takes over
            // the transfer rules (and then the source is destroyed normally, C++ style).
            if (has_move_ctor) return true;
            if (nomove) return false;
            for (const auto& f : fields) {
                if (f.weak) continue;
                if (!f.type) continue;
                if (!f.type->movable()) return false;
            }
            return true;
        case TypeKind::Array:
            if (elem && elem->needs_ctor_glue()) return false;
            return elem && elem->movable();
        case TypeKind::Sum:
            for (const auto& f : sum_fields) {
                for (const auto& ft : f) {
                    if (ft && !ft->movable()) return false;
                }
            }
            return true;
        default: return true;  // scalars, pointers, enums, function values
    }
}bool Type::has_default_ctor() const {
    for (const auto& p : ctors) {
        if (p.empty()) return true;
    }
    return false;
}

// The constructor-side dual of `needs_dtor`: constructing the value implicitly has to run a user
// constructor somewhere inside it. A class field is a handle (null), and a sum/slice/delegate is built by
// the expression that produces it, so neither contributes.
bool Type::needs_ctor() const {
    switch (kind) {
        case TypeKind::Struct:
            if (has_default_ctor()) return true;
            for (const auto& f : fields) {
                if (f.weak) continue;
                if (f.type && f.type->needs_ctor()) return true;
            }
            return false;
        case TypeKind::Array:
            return elem && elem->needs_ctor();
        default: return false;
    }
}
