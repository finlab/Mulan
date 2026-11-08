// mu-lan 类型模型
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
};

// 方法签名（class / interface）
struct MethodSig {
    std::string name;
    TypePtr ret;
    std::vector<TypePtr> params;
    bool is_abstract = false;
    bool is_override = false;
};

struct Type {
    TypeKind kind = TypeKind::Void;
    // Prim / Struct / Enum / Module 用
    std::string name;
    std::string unit;
    TypePtr elem;
    size_t array_size = 0;
    bool readonly = false;
    std::vector<Field> fields;  // Struct/Class 用（先建占位、后填充；Class 存扁平字段）
    std::vector<MethodSig> methods;  // Class/Interface 用
    std::vector<std::vector<TypePtr>> ctors;  // Class 构造器参数表（重载）
    TypePtr base;               // Class 的基类
    std::vector<TypePtr> interfaces;  // Class/Interface 实现的接口
    bool is_abstract = false;   // Class 有未实现方法

    static TypePtr make_void();
    static TypePtr make_prim(const std::string& name);
    static TypePtr make_string();
    static TypePtr make_ptr(const TypePtr& elem, bool readonly = false);
    static TypePtr make_array(const TypePtr& elem, size_t size);
    static TypePtr make_slice(const TypePtr& elem, bool readonly = false);
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
bool is_class_type(const TypePtr& t);
bool is_interface_type(const TypePtr& t);
bool is_reference_type(const TypePtr& t);  // class / interface
bool is_subclass_of(const TypePtr& sub, const TypePtr& base);
bool implements_interface(const TypePtr& t, const TypePtr& iface);
bool implicitly_convertible(const TypePtr& src, const TypePtr& dst);
bool promote_int_literal(long long v, const TypePtr& dst);
bool widens_int(const TypePtr& src, const TypePtr& dst);
TypePtr usual_arith(const TypePtr& a, const TypePtr& b);  // 失败返回 nullptr
