// 语义分析
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"
#include "types.hpp"

struct EnumConst {
    std::string unit;
    std::string enum_name;
    std::string member;
    long long value = 0;
    TypePtr ty;
};

struct UnitInfo {
    Unit* unit = nullptr;
    std::unordered_map<std::string, FuncDecl*> funcs;
    std::unordered_map<std::string, GlobalVar*> globals;
    std::unordered_map<std::string, StructDecl*> structs;
    std::unordered_map<std::string, EnumDecl*> enums;
};

struct Program {
    std::vector<std::unique_ptr<Unit>> units;
    std::unordered_map<std::string, UnitInfo> infos;
    std::string entry;
};

std::string mangle(const std::string& unit, const std::string& name);

// 失败时抛 SemaError
Program analyze(std::vector<std::unique_ptr<Unit>> units, const std::string& entry);

// 供 codegen 使用：把 Type 转成 mu-lan 源码写法
std::string type_str(const TypePtr& t);
