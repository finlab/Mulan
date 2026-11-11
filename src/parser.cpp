// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 语法分析 —— 对外接口（parse_source / parse_file）与 Parser 构造
#include "parser.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include "parser_impl.hpp"

using namespace muc_detail;

// ---------------------------------------------------------------- 语法分析器

Parser::Parser(const std::string& src, const std::string& filename)
    : filename_(filename), toks_(tokenize(src, filename)) {
    // std.string 提供的字符串类型（库在 sema 前不可见）
    type_names_.insert("string");
    // std.string 总是隐式依赖（字符串字面量需要它的类型）
    module_names_.insert("std.string");
}

// ---------------------------------------------------------------- 对外接口

std::unique_ptr<Unit> parse_source(const std::string& src, const std::string& filename) {
    Parser parser(src, filename);
    return parser.parse_unit();
}

std::unique_ptr<Unit> parse_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw MuError("cannot read or write file " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_source(ss.str(), path);
}
