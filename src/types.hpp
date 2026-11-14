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
enum class TypeKind { Void, Prim, Ptr, Array, Slice, Struct, Enum, Class, Interface, Module, Null, Range, Ref, Func };

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
    bool is_public = true;  // the private-by-default is explicitly overridden by class methods; struct/interface methods are always public
    TypePtr owner;  // the class/interface that declares this method (filled by sema)
    // The two forms of interface members (U34 M3):
    //   is_operator = true  → static signature (`T operator+(T,T)`), no this, used only for constraints
    //   ret_self / params_self → the position comes from `Self` and is replaced by the type argument during constraint checking
    bool is_operator = false;
    bool ret_self = false;
    std::vector<bool> params_self;
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
    int align_req = 0;          // Struct: the N of align(N) (0 = unspecified)
    // Func (`(T1, T2) => R`): a one-word function value (C function pointer compatible)
    std::vector<TypePtr> fparams;
    TypePtr fret;

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

    std::string str() const;
    bool is_integer() const;   // integer or enum
    bool is_float() const;
    bool is_bool() const;
    bool is_numeric() const;   // integer / float / enum
    int bits() const;          // integer bit width (usize/isize=64)
    bool is_signed() const;
    bool needs_cleanup() const;  // whether the value contains fields that need RC cleanup (recursive)
    TypePtr field_type(const std::string& fname) const;  // returns nullptr if it does not exist
    int field_index(const std::string& fname) const;     // returns -1 if it does not exist
};

// Global singletons
TypePtr t_void();
TypePtr t_int();
TypePtr t_bool();
TypePtr t_null();
TypePtr t_range();

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