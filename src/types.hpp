// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Mulan type model
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Ref: a borrow (`ref T` / `ref const T`). It appears only in the return type position of a function/method,
// and in parameter patterns (the parameter side is still recorded with the pty_ref boolean, see FuncDecl). It cannot be used as a variable/field/element type.
enum class TypeKind { Void, Prim, Ptr, Array, Slice, Struct, Enum, Class, Interface, Module, Null, Range, Ref, Func, Delegate, Sum };

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct Field {
    std::string name;
    TypePtr type;
    bool weak = false;  // weak field (class only)
    bool is_public = true;  // struct fields are always public; class fields are private by default
};

// Method signature (class / interface)
struct MethodSig {
    std::string name;
    TypePtr ret;
    std::vector<TypePtr> params;
    bool is_abstract = false;
    bool is_override = false;
    // `virtual` (or abstract): the method occupies a vtable slot and calls on a class receiver may
    // dispatch dynamically. Non-virtual methods never enter the vtable (closed-world devirtualization
    // in codegen still upgrades virtual calls whose signature no subclass overrides).
    bool is_virtual = false;
    bool is_public = true;  // the private-by-default is explicitly overridden by class methods; struct/interface methods are always public
    TypePtr owner;  // the class/interface that declares this method (filled by sema)
    // The two forms of interface members (U34 M3):
    //   is_operator = true  → static signature (`T operator+(T,T)`), no this, used only for constraints
    //   ret_self / params_self → the position comes from `Self` and is replaced by the type argument during constraint checking
    bool is_operator = false;
    bool ret_self = false;
    std::vector<bool> params_self;
    // Operator members only: the receiver (the first operand) may be declared `ref Self` (mutate in
    // place) or `ref const Self` (read-only) by a constraint. An implementation takes the receiver
    // either by value or as a borrow; `recv_cref` is true only for the read-only form.
    bool recv_ref = false;
    bool recv_cref = false;
};

struct Type {
    TypeKind kind = TypeKind::Void;
    // Used by Prim / Struct / Enum / Module
    std::string name;
    std::string unit;
    TypePtr elem;
    size_t array_size = 0;
    int rank = 1;               // Slice: rank (1=T[], 2=T[,], ...)
    bool is_const = false;      // C model: whether this type node is const (per level)
    std::vector<Field> fields;  // used by Struct/Class (a placeholder is created first and filled in later; Class stores flattened fields)
    std::vector<MethodSig> methods;  // used by Class/Interface
    std::vector<std::vector<TypePtr>> ctors;  // class constructor parameter lists (overloads)
    std::vector<bool> ctor_is_public;        // parallel to ctors: whether the constructor is public
    std::vector<bool> ctor_is_implicit;      // parallel to ctors: `implicit` (usable as a conversion, struct only)
    TypePtr base;               // the base class of a Class
    std::vector<TypePtr> interfaces;  // the interfaces implemented by a Class/Interface
    bool is_abstract = false;   // a Class has unimplemented methods
    bool has_dtor = false;      // the user declared a destructor (struct/class)
    bool is_packed = false;     // Struct: packed (the fields are tightly packed, with no internal or tail padding, and the default align is 1)
    bool is_union = false;      // Struct: union (all members at offset 0, size = max(member))
    bool nocopy = false;        // Struct: `nocopy` - the value must never be copied (agents/value-semantics.md §8.6)
    bool has_move_ctor = false; // Struct: the user declared `T(move T other)`; the type is movable
    bool has_copy_ctor = false; // Struct: the user declared `T(ref T other)`; the type is copyable
    int align_req = 0;          // Struct: the N of align(N) (0 = unspecified)
    // Func (`(T1, T2) => R`): a one-word function value (C function pointer compatible)
    std::vector<TypePtr> fparams;
    TypePtr fret;
    // Sum type (`type Value = Int(int) | Text(string) | None;`): tag + one active payload.
    // `sum_layout` is the synthesized `struct { i32 tag; union payload }` that owns the real layout, so
    // `type_size` / `type_align` / `lt` delegate to it; `sum_payload[i]` is alternative i's payload type
    // (the union member: the field itself, an anonymous struct for several fields, or none).
    std::vector<std::string> sum_names;
    std::vector<std::vector<TypePtr>> sum_fields;
    std::vector<TypePtr> sum_payload;
    TypePtr sum_layout;

    static TypePtr make_void();
    static TypePtr make_prim(const std::string& name);
    static TypePtr make_ptr(const TypePtr& elem, bool is_const = false);
    static TypePtr make_ref(const TypePtr& elem, bool is_const = false);
    static TypePtr make_array(const TypePtr& elem, size_t size);
    static TypePtr make_slice(const TypePtr& elem, bool is_const = false, int rank = 1);
    static TypePtr make_struct(const std::string& unit, const std::string& name);
    static TypePtr make_enum(const std::string& unit, const std::string& name);
    static TypePtr make_class(const std::string& unit, const std::string& name);
    static TypePtr make_interface(const std::string& unit, const std::string& name);
    static TypePtr make_module(const std::string& unit);
    static TypePtr make_null();
    static TypePtr make_range();
    static TypePtr make_func(const std::vector<TypePtr>& params, const TypePtr& ret);
// `delegate (T1, T2) => R`: a two-word value `{code, env}` whose environment is an RC object (3.16).
static TypePtr make_delegate(const std::vector<TypePtr>& params, const TypePtr& ret);
// Sum type: `type Value = Int(int) | Text(string) | None;` (SPEC 10.1). `payload[i]` is the union
// member type of alternative i (null for a unit alternative); `layout` is the synthesized
// `struct { i32 tag; union { ... } }` that owns the layout.
static TypePtr make_sum(const std::string& unit, const std::string& name,
                        const std::vector<std::string>& names,
                        const std::vector<std::vector<TypePtr>>& fields,
                        const std::vector<TypePtr>& payload, const TypePtr& layout);

    std::string str() const;
    bool is_integer() const;   // integer or enum
    bool is_float() const;
    bool is_bool() const;
    bool is_numeric() const;   // integer / float / enum
    int bits() const;          // integer bit width (usize/isize=64)
    // Sign of an *integer* type only: floats and non-primitive kinds answer true (prototype parity).
    // Callers that can see a float must take the float branch first.
    bool is_signed() const;
    bool needs_cleanup() const;  // whether the value contains fields that need RC cleanup (recursive)
    // Value-semantics predicate family (authoritative definition: agents/value-semantics.md §1-§2).
    // All of them recurse through the field/element/payload structure; none of them is cached.
    bool needs_dtor() const;          // destroying the value is observable (RC release or a user destructor)
    bool trivially_copyable() const;  // a bitwise copy is correct: no user destructor, no RC handle
    bool is_pod() const;              // trivially copyable and no user constructor, so construction is a memset
    bool copyable() const;            // may be copied by value: `class` copies its handle, a struct copies its fields
    bool movable() const;             // may be moved: a declared move constructor, or a bit copy that is a legal copy
    bool has_user_ctor() const;       // the user declared at least one constructor
    bool has_default_ctor() const;    // ... and one of them takes no arguments
    bool needs_ctor() const;          // constructing the value implicitly has to run user code
    TypePtr field_type(const std::string& fname) const;  // returns nullptr if it does not exist
    int field_index(const std::string& fname) const;     // returns -1 if it does not exist
};

// Global singletons
TypePtr t_void();
TypePtr t_int();
TypePtr t_bool();
TypePtr t_null();
TypePtr t_range();

// One definition for the whole compiler: an identical inline body in sema_impl.hpp and
// codegen_support.cpp used to violate the ODR and broke the link on GCC 16.
inline bool is_void(const TypePtr& t) { return t && t->kind == TypeKind::Void; }
// A value that can be called: a plain function value (`fn`) or a capturing one (`delegate`).
inline bool is_callable_value(const TypePtr& t) {
    return t && (t->kind == TypeKind::Func || t->kind == TypeKind::Delegate);
}

const std::vector<std::string>& prim_names();
bool is_prim_name(const std::string& n);

bool same_type(const TypePtr& a, const TypePtr& b);

// Layout (a Struct's packed / align(N) is implemented only here; codegen delegates to it so that the two layouts cannot drift apart)
size_t type_align(const TypePtr& t);
size_t type_size(const TypePtr& t);
// Byte offset of struct/union member `idx` (0 for every union member). The single layout definition: the
// codegen Emitter and the constant folder both call this, so `offsetof` and field addressing cannot drift.
uint64_t type_member_offset(const TypePtr& t, size_t idx);
bool is_class_type(const TypePtr& t);
bool is_interface_type(const TypePtr& t);
bool is_reference_type(const TypePtr& t);  // class / interface
bool is_borrow_type(const TypePtr& t);     // ref T (a borrow)
TypePtr referent_of(const TypePtr& t);     // ref T -> T, everything else is returned unchanged
bool is_subclass_of(const TypePtr& sub, const TypePtr& base);
bool implements_interface(const TypePtr& t, const TypePtr& iface);
bool implicitly_convertible(const TypePtr& src, const TypePtr& dst);
bool promote_int_literal(long long v, const TypePtr& dst);
bool widens_int(const TypePtr& src, const TypePtr& dst);
TypePtr usual_arith(const TypePtr& a, const TypePtr& b);  // returns nullptr on failure