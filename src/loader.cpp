// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// unit loading
//
// Default library search paths:
//   1. the directory containing the entry file
//   2. directories given on the command line with -I
//   3. the MUC_LIBS environment variable (separated by `;` or `:`)
//   4. libs / libs/std under the current directory and its ancestors
//
// Standard library units use dot-separated layers: `unit std.string;` maps to `libs/std/string.mu`.
#include "loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "common.hpp"
#include "parser.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;

std::string dirname_of(const std::string& p) {
    auto pos = p.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : p.substr(0, pos);
}

bool file_exists(const std::string& p) {
    std::ifstream f(p);
    return static_cast<bool>(f);
}

void add_unique(std::vector<std::string>& dirs, const std::string& d) {
    if (d.empty()) return;
    for (auto& x : dirs) {
        if (x == d) return;
    }
    dirs.push_back(d);
}

// dotted unit name -> relative path (std.string -> std/string)
std::string dotted_path(const std::string& name) {
    std::string out = name;
    for (char& c : out) {
        if (c == '.') c = '/';
    }
    return out;
}

void add_search_dir(std::vector<std::string>& dirs, const fs::path& base) {
    add_unique(dirs, (base / "libs").string());
    add_unique(dirs, (base / "libs" / "std").string());
}

fs::path executable_dir() {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (n > 0 && n < 32768) return fs::path(std::wstring(buf, n)).parent_path();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return {};
}

// libs / libs/std in the current directory, the executable directory, and their ancestors
void add_default_lib_dirs(std::vector<std::string>& dirs) {
    if (const char* env = std::getenv("MUC_LIBS")) {
        std::string s = env;
        std::string cur;
        for (char c : s) {
            if (c == ';' || c == ':') {
                if (!cur.empty()) add_unique(dirs, cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) add_unique(dirs, cur);
    }

    std::error_code ec;
    fs::path p = fs::current_path(ec);
    for (int i = 0; i < 12 && !p.empty(); ++i) {
        add_search_dir(dirs, p);
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }

    // the directory of the executable and its ancestors (covers running under build/, running from an install dir, etc.)
    p = executable_dir();
    for (int i = 0; i < 12 && !p.empty(); ++i) {
        add_search_dir(dirs, p);
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
}

}  // namespace

std::vector<std::unique_ptr<Unit>> load_units(const std::string& entry_path,
                                              const std::vector<std::string>& search_dirs,
                                              std::string& entry_name) {
    std::vector<std::string> dirs;
    add_unique(dirs, dirname_of(entry_path));
    for (auto& d : search_dirs) add_unique(dirs, d);
    add_default_lib_dirs(dirs);

    std::map<std::string, std::unique_ptr<Unit>> owned;
    std::set<std::string> in_progress;
    std::vector<std::string> post_order;

    std::function<std::string(const std::string&)> find_file = [&](const std::string& un) {
        for (auto& d : dirs) {
            for (const std::string& rel : {un + ".mu", dotted_path(un) + ".mu"}) {
                std::string p = d + "/" + rel;
                if (file_exists(p)) return p;
            }
        }
        std::string tried;
        for (auto& d : dirs) {
            if (!tried.empty()) tried += "\n  ";
            tried += d + "/" + dotted_path(un) + ".mu";
        }
        throw MuError("cannot find unit `" + un + "`; tried:\n  " + tried);
    };

    std::function<Unit*(const std::string&)> visit = [&](const std::string& path) -> Unit* {
        auto unit = parse_file(path);
        std::string name = unit->name;
        auto it = owned.find(name);
        if (it != owned.end()) return it->second.get();
        if (in_progress.count(name)) {
            throw MuError("unit circular dependency: " + name);
        }
        in_progress.insert(name);
        Unit* raw = unit.get();
        owned[name] = std::move(unit);
        for (auto& use : raw->uses) {
            Unit* dep = visit(find_file(use->unit_name));
            if (dep->name != use->unit_name) {
                throw MuError(dep->filename + " declares unit `" + dep->name +
                                  "` but it is referenced by `use " + use->unit_name + ";`",
                              use->line, use->col, raw->filename);
            }
        }
        in_progress.erase(name);
        post_order.push_back(name);
        return raw;
    };

    // standard library string unit: always loaded as an implicit dependency (string literals need its type).
    (void)visit(find_file("std.string"));

    Unit* entry_unit = visit(entry_path);
    entry_name = entry_unit->name;

    std::vector<std::unique_ptr<Unit>> result;
    result.reserve(post_order.size());
    for (auto& name : post_order) result.push_back(std::move(owned[name]));
    return result;
}
