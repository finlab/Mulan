// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// unit 加载
//
// 默认库搜索路径：
//   1. 入口文件所在目录
//   2. 命令行 -I 指定的目录
//   3. 环境变量 MUC_LIBS（`;` 或 `:` 分隔）
//   4. 当前目录及其祖先目录下的 libs / libs/std
//
// 标准库 unit 用点号分层：`unit std.string;` 对应 `libs/std/string.mu`。
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

// 点号 unit 名 -> 相对路径（std.string -> std/string）
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

// 当前目录、可执行文件目录及其祖先目录中的 libs / libs/std
void add_default_lib_dirs(std::vector<std::string>& dirs) {
    if (const char* env = std::getenv("MULANC_LIBS")) {
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

    // 可执行文件所在目录及其祖先（覆盖 build/ 下运行、安装目录运行等情况）
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
        throw MuError("找不到 unit `" + un + "`；已尝试：\n  " + tried);
    };

    std::function<Unit*(const std::string&)> visit = [&](const std::string& path) -> Unit* {
        auto unit = parse_file(path);
        std::string name = unit->name;
        auto it = owned.find(name);
        if (it != owned.end()) return it->second.get();
        if (in_progress.count(name)) {
            throw MuError("unit 循环依赖：" + name);
        }
        in_progress.insert(name);
        Unit* raw = unit.get();
        owned[name] = std::move(unit);
        for (auto& use : raw->uses) {
            Unit* dep = visit(find_file(use->unit_name));
            if (dep->name != use->unit_name) {
                throw MuError(dep->filename + " 声明的 unit 是 `" + dep->name +
                                  "`，但被 `use " + use->unit_name + ";` 引用",
                              use->line, use->col, raw->filename);
            }
        }
        in_progress.erase(name);
        post_order.push_back(name);
        return raw;
    };

    // 标准库字符串 unit：总是作为隐式依赖加载（字符串字面量需要它的类型）。
    (void)visit(find_file("std.string"));

    Unit* entry_unit = visit(entry_path);
    entry_name = entry_unit->name;

    std::vector<std::unique_ptr<Unit>> result;
    result.reserve(post_order.size());
    for (auto& name : post_order) result.push_back(std::move(owned[name]));
    return result;
}
