// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Mulan 类型模型
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

enum class TypeKind { Void, Prim, String, Ptr, Array, Slice, Struct, Enum, Class, Interface, Module, Null, Range };

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct Field {
    std::string name;
    TypePtr type;
    bool weak = false;  // weak 字段（只用于 class）
    bool is_public = true;  // struct 字段恒公开；class 字段默认私有
};

// 方法签名（class / interface）
struct MethodSig {
    std::string name;
    TypePtr ret;
    std::vector<TypePtr> params;
    bool is_abstract = false;
    bool is_override = false;
    bool is_public = true;  // 默认私有由 class 方法显式覆盖；struct/接口方法恒公开
    TypePtr owner;  // 声明该方法的 class/interface（sema 填）
};

struct Type {
    TypeKind kind = TypeKind::Void;
    // Prim / Struct / Enum / Module 用
    std::string name;
    std::string unit;
    TypePtr elem;
    size_t array_size = 0;
    int rank = 1;               // Slice：维数（1=T[]，2=T[,]，……）
    bool is_const = false;      // C 模型：该类型节点是否 const（逐层）
    std::vector<Field> fields;  // Struct/Class 用（先建占位、后填充；Class 存扁平字段）
    std::vector<MethodSig> methods;  // Class/Interface 用
    std::vector<std::vector<TypePtr>> ctors;  // Class 构造器参数表（重载）
    std::vector<bool> ctor_is_public;        // 与 ctors 平行：构造器是否 public
    TypePtr base;               // Class 的基类
    std::vector<TypePtr> interfaces;  // Class/Interface 实现的接口
    bool is_abstract = false;   // Class 有未实现方法
    bool has_dtor = false;      // 用户声明了析构器（struct/class）
    bool is_packed = false;     // Struct：packed（字段紧排，无内部/尾部填充，align 默认 1）
    bool is_union = false;      // Struct：union（所有成员偏移 0，size = max(成员)）
    int align_req = 0;          // Struct：align(N) 的 N（0 = 未指定）

    static TypePtr make_void();
    static TypePtr make_prim(const std::string& name);
    static TypePtr make_string();
    static TypePtr make_ptr(const TypePtr& elem, bool is_const = false);
    static TypePtr make_array(const TypePtr& elem, size_t size);
    static TypePtr make_slice(const TypePtr& elem, bool is_const = false, int rank = 1);
    static TypePtr make_struct(const std::string& unit, const std::string& name);
    static TypePtr make_enum(const std::string& unit, const std::string& name);
    static TypePtr make_class(const std::string& unit, const std::string& name);
    static TypePtr make_interface(const std::string& unit, const std::string& name);
    static TypePtr make_module(const std::string& unit);
    static TypePtr make_null();
    static TypePtr make_range();

    std::string str() const;
    bool is_integer() const;   // 整数或枚举
    bool is_float() const;
    bool is_bool() const;
    bool is_numeric() const;   // 整数 / 浮点 / 枚举
    int bits() const;          // 整数位宽（usize/isize=64）
    bool is_signed() const;
    bool needs_cleanup() const;  // 值是否含需要 RC 清理的字段（递归）
    TypePtr field_type(const std::string& fname) const;  // 不存在返回 nullptr
    int field_index(const std::string& fname) const;     // 不存在返回 -1
};

// 全局单例
TypePtr t_void();
TypePtr t_string();
TypePtr t_int();
TypePtr t_bool();
TypePtr t_null();
TypePtr t_range();

const std::vector<std::string>& prim_names();
bool is_prim_name(const std::string& n);

bool same_type(const TypePtr& a, const TypePtr& b);

// 布局（Struct 的 packed / align(N) 只在这里实现；codegen 委托调用，避免两份布局漂移）
size_t type_align(const TypePtr& t);
size_t type_size(const TypePtr& t);
bool is_class_type(const TypePtr& t);
bool is_interface_type(const TypePtr& t);
bool is_reference_type(const TypePtr& t);  // class / interface
bool is_subclass_of(const TypePtr& sub, const TypePtr& base);
bool implements_interface(const TypePtr& t, const TypePtr& iface);
bool implicitly_convertible(const TypePtr& src, const TypePtr& dst);
bool promote_int_literal(long long v, const TypePtr& dst);
bool widens_int(const TypePtr& src, const TypePtr& dst);
TypePtr usual_arith(const TypePtr& a, const TypePtr& b);  // 失败返回 nullptr
