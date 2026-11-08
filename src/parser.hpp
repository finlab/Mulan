// 语法分析
#pragma once

#include <memory>
#include <string>

#include "ast.hpp"

std::unique_ptr<Unit> parse_source(const std::string& src, const std::string& filename);
std::unique_ptr<Unit> parse_file(const std::string& path);
