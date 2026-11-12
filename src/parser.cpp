// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing — public interface (parse_source / parse_file) and Parser construction
#include "parser.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include "parser_impl.hpp"

using namespace muc_detail;

// ---------------------------------------------------------------- parser

Parser::Parser(const std::string& src, const std::string& filename)
    : filename_(filename), toks_(tokenize(src, filename)) {
    // string type provided by std.string (the library is not visible before sema)
    type_names_.insert("string");
    // std.string is always an implicit dependency (string literals need its type)
    module_names_.insert("std.string");
    module_names_.insert("std.exception");
}

// ---------------------------------------------------------------- public interface

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
