// 符号修饰：sema 与 codegen 共享，保证两侧算出的名字一致。
#pragma once

#include <string>
#include <vector>

#include "types.hpp"

struct FuncDecl;

// 类型编码（用于参数/返回类型参与符号名，避免重载冲突）
std::string type_code(const TypePtr& t);

// 运算符符号 -> 标识符片段
std::string op_code(const std::string& sym);

// 自由函数名（不含 unit 前缀）
std::string func_code_name(const std::string& name);

// 完整符号名
std::string mangle_unit(const std::string& unit, const std::string& name);
std::string func_cname(const std::string& unit, FuncDecl* f);

// 方法 / 构造器 / 析构器
std::string method_cname(const std::string& unit, const std::string& owner,
                         const std::string& name, const std::vector<TypePtr>& params);
std::string ctor_cname(const std::string& unit, const std::string& owner,
                       const std::vector<TypePtr>& params);
std::string dtor_cname(const std::string& unit, const std::string& owner);

// 类描述符与虚表全局符号
std::string class_desc_name(const std::string& unit, const std::string& owner);
std::string class_vtable_name(const std::string& unit, const std::string& owner);
