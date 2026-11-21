// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Mulan type model
//
// Design: TypePtr points to immutable value objects; a Struct's fields are created as a placeholder first and filled in later
// (mirroring the two-stage construction of StructType in the Python version), so no const is used here.
//
// owns:      construction (`make_*`), `str()`, `field_type` / `field_index`, the layout queries
//            type_size / type_align and the integer-promotion helpers
// not:       the predicate family and the built-in type name table (type_traits.cpp); any pass
// pass:      used by every pass
// concept:   types, layout
#include "types.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {


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

TypePtr Type::make_delegate(const std::vector<TypePtr>& params, const TypePtr& ret) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Delegate;
    t->fparams = params;
    t->fret = ret;
    return t;
}

TypePtr Type::make_sum(const std::string& unit, const std::string& name,
                       const std::vector<std::string>& names,
                       const std::vector<std::vector<TypePtr>>& fields,
                       const std::vector<TypePtr>& payload, const TypePtr& layout) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Sum;
    t->unit = unit;
    t->name = name;
    t->sum_names = names;
    t->sum_fields = fields;
    t->sum_payload = payload;
    t->sum_layout = layout;
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
            return is_const ? base + " const" : base;
        }
        case TypeKind::Ref: {
            std::string base = elem_str(elem);
            return is_const ? "const ref " + base : "ref " + base;
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
            return is_const ? base + " const" : base;
        }
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Class:
        case TypeKind::Interface:
        case TypeKind::Sum: {
            std::string base = unit.empty() ? name : unit + "." + name;
            return is_const ? "const " + base : base;
        }
        case TypeKind::Module:
            return unit;
        case TypeKind::Null:
            return "null";
        case TypeKind::Range:
            return "range";
        case TypeKind::Func:
        case TypeKind::Delegate: {
            std::string out = kind == TypeKind::Delegate ? "delegate (" : "(";
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

// `type_size` / `type_align` recurse over the by-value nesting of a type. The counter is a stack guard: a
// by-value cycle (which has no finite layout) must not blow the compiler's stack. A program that reaches
// the limit, or that contains a cycle, is rejected by `check_layouts`, so an accepted program never keeps
// a silently wrong layout.
constexpr int kMaxTypeNesting = 512;
bool g_type_depth_exceeded = false;
bool g_type_cycle = false;
std::string g_cycle_name;
// The types currently on the recursion path: a repeat means the type contains itself by value. The
// ordered stack keeps the path so the diagnostic can spell the cycle out (`A -> B -> A`).
std::unordered_set<const Type*> g_path;
std::vector<const Type*> g_stack;

void note_cycle(const TypePtr& t) {
    g_type_cycle = true;
    if (!g_cycle_name.empty()) return;
    // The path in order, from the first occurrence of the repeated type. Synthesized types
    // (`$sum$L`, `$payload$L`) are left out: they are an implementation detail of the layout, not
    // something the reader declared, and they would drown the actual cycle.
    std::vector<std::string> names;
    bool started = false;
    for (const Type* p : g_stack) {
        if (!started) {
            if (p != t.get()) continue;
            started = true;
        }
        std::string name = p->str();
        if (name.find(".$") != std::string::npos) continue;
        names.push_back(std::move(name));
    }
    names.push_back(t->str());
    std::string out;
    for (auto& n : names) {
        if (!out.empty()) out += " -> ";
        out += n;
    }
    g_cycle_name = out;
}

// Keeps `g_path` in sync with the recursion: `inserted` is false when the type was already on the path.
struct PathGuard {
    const Type* t = nullptr;
    bool inserted = false;
    explicit PathGuard(const TypePtr& tp) : t(tp.get()), inserted(g_path.insert(tp.get()).second) {
        if (inserted) g_stack.push_back(tp.get());
    }
    ~PathGuard() {
        if (inserted) {
            g_path.erase(t);
            g_stack.pop_back();
        }
    }
    PathGuard(const PathGuard&) = delete;
    PathGuard& operator=(const PathGuard&) = delete;
};

size_t align_body(const TypePtr& t, int depth);

// Only a composite type loses layout information to the depth guard: a leaf (primitive, pointer, enum,
// interface, delegate) has no nested layout, so reaching the guard at a leaf is normal — a chain of
// exactly `kMaxTypeNesting` containers ends with a leaf one level past the budget.
bool has_nested_layout(const TypePtr& t) {
    return t->kind == TypeKind::Struct || t->kind == TypeKind::Array || t->kind == TypeKind::Sum;
}

size_t align_impl(const TypePtr& t, int depth) {
    if (!t) return 1;
    if (depth >= kMaxTypeNesting && has_nested_layout(t)) {
        g_type_depth_exceeded = true;
        return 1;
    }
    PathGuard guard(t);
    if (!guard.inserted) {
        note_cycle(t);
        return 1;
    }
    return align_body(t, depth);
}

size_t align_body(const TypePtr& t, int depth) {
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
        case TypeKind::Sum:
            // tag + payload: the synthesized layout type owns the real rules
            return t->sum_layout ? align_impl(t->sum_layout, depth + 1) : 8;
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

size_t size_body(const TypePtr& t, int depth);

size_t size_impl(const TypePtr& t, int depth) {
    if (!t) return 0;
    if (depth >= kMaxTypeNesting && has_nested_layout(t)) {
        g_type_depth_exceeded = true;
        return 0;
    }
    PathGuard guard(t);
    if (!guard.inserted) {
        note_cycle(t);
        return 0;
    }
    return size_body(t, depth);
}

size_t size_body(const TypePtr& t, int depth) {
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
        case TypeKind::Delegate: return 16;   // {code, env}
        case TypeKind::Sum:
            return t->sum_layout ? size_impl(t->sum_layout, depth + 1) : 0;
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
bool type_depth_exceeded() { return g_type_depth_exceeded; }
bool type_cycle_detected() { return g_type_cycle; }
std::string type_cycle_name() { return g_cycle_name; }
void clear_type_depth_exceeded() {
    g_type_depth_exceeded = false;
    g_type_cycle = false;
    g_cycle_name.clear();
    g_path.clear();
    g_stack.clear();
}

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

// ---------------------------------------------------------------- Type equality

static bool same_type_impl(const TypePtr& a, const TypePtr& b, bool ignore_top_const) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (!ignore_top_const && a->is_const != b->is_const) return false;
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
            return a->rank == b->rank && same_type_impl(a->elem, b->elem, false);
        case TypeKind::Array:
            return a->array_size == b->array_size && same_type_impl(a->elem, b->elem, false);
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Class:
        case TypeKind::Interface:
        case TypeKind::Sum:
            // struct/enum/class/interface/sum are compared by (unit, name)
            return a->unit == b->unit && a->name == b->name;
        case TypeKind::Module:
            return a->unit == b->unit;
        case TypeKind::Func:
        case TypeKind::Delegate:
            // A function type's parameter/return top-level const is ignored (C++: it is not part of the
            // function type); a pointee const still participates.
            if (!same_type_impl(a->fret, b->fret, true)) return false;
            if (a->fparams.size() != b->fparams.size()) return false;
            for (size_t i = 0; i < a->fparams.size(); ++i) {
                if (!same_type_impl(a->fparams[i], b->fparams[i], true)) return false;
            }
            return true;
    }
    return false;
}

bool same_type(const TypePtr& a, const TypePtr& b) {
    return same_type_impl(a, b, false);
}

bool same_type_sig(const TypePtr& a, const TypePtr& b) {
    return same_type_impl(a, b, true);
}

bool same_param_sig(const std::vector<TypePtr>& a, const std::vector<TypePtr>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!same_type_sig(a[i], b[i])) return false;
    }
    return true;
}

bool same_param_sig(const std::vector<TypePtr>& a, const std::vector<bool>& a_ref,
                    const std::vector<TypePtr>& b, const std::vector<bool>& b_ref) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        // A top-level `const` is not part of the signature (SPEC 3.7).
        if (!same_type_sig(a[i], b[i])) return false;
        const bool ar = i < a_ref.size() && a_ref[i];
        const bool br = i < b_ref.size() && b_ref[i];
        if (ar != br) return false;
    }
    return true;
}

bool read_only_view(const TypePtr& t) {
    if (!t) return false;
    if (t->is_const) return true;
    if (t->kind != TypeKind::Interface) return false;
    for (const auto& m : t->methods) {
        if (m.is_operator) {
            // An operator signature is static: only a mutable receiver can write through the view.
            if (m.recv_ref && !m.recv_cref) return false;
            continue;
        }
        if (!m.is_const) return false;
    }
    for (const auto& b : t->interfaces) {
        if (!read_only_view(b)) return false;
    }
    return true;
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
        // A const view of the base is reached by upcasting (adding cv is implicit), so the comparison
        // ignores the top-level cv of both sides.
        if (same_type_sig(cur, base)) return true;
        cur = cur->base;
    }
    return false;
}

static bool iface_extends(const TypePtr& a, const TypePtr& b, int depth) {
    if (!a || !b || depth > 256) return false;
    if (same_type_sig(a, b)) return true;  // the cv of an interface view is not part of the relation
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

    if (ss && !ds) return false;  // signed -> unsigned: never allowed implicitly
    return db > sb;               // same sign, wider destination loses nothing
}

bool implicitly_convertible(const TypePtr& src, const TypePtr& dst) {
    // C model: const can only be relaxed, never tightened (same_type ignores const, so it must be rejected
    // first). A read-only view never widens to a mutable one implicitly — this covers pointer/slice levels
    // and, for handles, base and interface conversions (`as` is the escape, SPEC 3.6/3.7).
    if (src && dst) {
        const bool handle_src = src->kind == TypeKind::Class || src->kind == TypeKind::Interface;
        const bool handle_dst = dst->kind == TypeKind::Class || dst->kind == TypeKind::Interface;
        if (handle_src && handle_dst) {
            const bool src_ro = read_only_view(src);
            if (src_ro && !read_only_view(dst)) return false;
        }
    }
    if (src && dst && src->kind == dst->kind) {
        if (src->kind == TypeKind::Ptr || src->kind == TypeKind::Slice) {
            if (src->elem && dst->elem && src->elem->is_const && !dst->elem->is_const) {
                return false;
            }
        }
        // A class/interface handle names a shared object, so dropping a `const` view would grant write
        // access through the same object: that needs an explicit `as` (SPEC 3.7).
        if ((src->kind == TypeKind::Class || src->kind == TypeKind::Interface) && src->is_const &&
            !dst->is_const) {
            return false;
        }
    }
    if (same_type_sig(src, dst)) return true;
    if (!src || !dst) return false;

    // null -> pointer / slice / class / interface / function value / delegate
    if (src->kind == TypeKind::Null) {
        return dst->kind == TypeKind::Ptr || dst->kind == TypeKind::Slice ||
               dst->kind == TypeKind::Class ||
               dst->kind == TypeKind::Interface || dst->kind == TypeKind::Func ||
               dst->kind == TypeKind::Delegate;
    }

    // A function value is a pointer: it converts to `void*` implicitly (the reverse needs `as`, 3.16)
    if (src->kind == TypeKind::Func) {
        if (dst->kind == TypeKind::Ptr && dst->elem && dst->elem->kind == TypeKind::Void) return true;
        // `fn` -> `delegate`: the environment is null, so a plain function works as a delegate
        // (the missing `env` argument is supplied by a thunk, 3.16).
        if (dst->kind == TypeKind::Delegate) {
            if (!same_type(src->fret, dst->fret)) return false;
            if (src->fparams.size() != dst->fparams.size()) return false;
            for (size_t i = 0; i < src->fparams.size(); ++i) {
                if (!same_type(src->fparams[i], dst->fparams[i])) return false;
            }
            return true;
        }
        return false;
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
        return same_type_sig(src->elem, dst->elem);
    }

    // Array -> pointer (C99 6.3.2.1 decay): the array's first element address; same element type,
    // const may only be relaxed. `void*` is accepted like any other pointer target.
    if (src->kind == TypeKind::Array && dst->kind == TypeKind::Ptr) {
        if (!dst->elem) return false;
        if (dst->elem->kind == TypeKind::Void) return true;
        if (src->elem && src->elem->is_const && !dst->elem->is_const) return false;
        return same_type_sig(src->elem, dst->elem);
    }

    // Array -> slice
    if (src->kind == TypeKind::Array && dst->kind == TypeKind::Slice) {
        return same_type_sig(src->elem, dst->elem);
    }

    // Slice: T[] -> const T[]; the reverse requires an explicit as
    if (src->kind == TypeKind::Slice && dst->kind == TypeKind::Slice) {
        if (!same_type_sig(src->elem, dst->elem)) return false;
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
