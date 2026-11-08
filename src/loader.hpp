// unit 加载
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"

// 从入口文件出发，按 use 递归解析同名 .mu；返回依赖在前、入口在后的 unit 列表。
// entry_name 被写入入口 unit 名。失败抛 MuError。
std::vector<std::unique_ptr<Unit>> load_units(const std::string& entry_path,
                                              const std::vector<std::string>& search_dirs,
                                              std::string& entry_name);
