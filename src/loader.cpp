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
#include <iostream>
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

const char* host_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// Platform shadow directories are `<dir>/platform/<os>/`: every unit below `<dir>` is looked for
// there first (nearest directory first). The loader only looks at one OS at a time; `--os` picks it
// explicitly so a program can be checked against another platform's units on any host.
const std::vector<std::string>& platform_os_names() {
    static const std::vector<std::string> kOses{"windows", "linux", "macos"};
    return kOses;
}

bool is_platform_os(const std::string& s) {
    for (const auto& o : platform_os_names())
        if (o == s) return true;
    return false;
}

std::string resolve_platform(const std::string& requested) {
    if (requested.empty()) return host_os();
    if (!is_platform_os(requested)) {
        std::string known;
        for (const auto& o : platform_os_names()) known += (known.empty() ? "" : ", ") + o;
        throw MuError("unknown platform `" + requested + "` (known: " + known + ")");
    }
    return requested;
}

std::vector<std::string> split_unit(const std::string& un) {
    std::vector<std::string> seg;
    size_t start = 0;
    while (true) {
        size_t dot = un.find('.', start);
        if (dot == std::string::npos) {
            seg.push_back(un.substr(start));
            break;
        }
        seg.push_back(un.substr(start, dot - start));
        start = dot + 1;
    }
    return seg;
}

// `platform` names the shadow directories, so it cannot also name a unit namespace, and a
// `platform.<os>` pair would collide with a shadow path inside a unit tree.
void check_unit_name(const std::string& un, int line, int col, const std::string& file) {
    const auto seg = split_unit(un);
    if (!seg.empty() && seg[0] == "platform") {
        throw MuError("unit name `" + un + "` starts with `platform`, which is reserved for the "
                      "platform shadow directories (`<dir>/platform/<os>/`); pick another name",
                      line, col, file);
    }
    for (size_t i = 0; i + 1 < seg.size(); ++i) {
        if (seg[i] == "platform" && is_platform_os(seg[i + 1])) {
            throw MuError("unit name `" + un + "` contains `platform." + seg[i + 1] +
                              "`, which collides with the platform shadow form "
                              "(`<dir>/platform/<os>/`)",
                          line, col, file);
        }
    }
}

// Generic library tree only: platform shadows are resolved per unit, not as extra search roots.
void add_search_dir(std::vector<std::string>& dirs, const fs::path& base) {
    add_unique(dirs, (base / "libs").string());
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

std::string host_platform_os() { return host_os(); }

std::vector<std::unique_ptr<Unit>> load_units(const std::string& entry_path,
                                              const std::vector<std::string>& search_dirs,
                                              std::string& entry_name, const std::string& platform_os,
                                              bool verbose) {
    std::vector<std::string> dirs;
    add_unique(dirs, dirname_of(entry_path));
    for (auto& d : search_dirs) add_unique(dirs, d);
    add_default_lib_dirs(dirs);

    const std::string os_name = resolve_platform(platform_os);

    std::map<std::string, std::unique_ptr<Unit>> owned;
    std::set<std::string> in_progress;
    std::vector<std::string> post_order;

    // Two accepted file names per unit: the flat dotted name (`std.thread.backend.mu`) and the
    // slashed path (`std/thread/backend.mu`). Both are also accepted inside a platform shadow
    // directory, where the file keeps the full unit name.
    auto try_dir = [&](const std::string& d, const std::string& un) -> std::string {
        const std::string flat = d + "/" + un + ".mu";
        const std::string slashed = d + "/" + dotted_path(un) + ".mu";
        const bool has_flat = file_exists(flat);
        const bool has_slashed = slashed != flat && file_exists(slashed);
        if (has_flat && has_slashed) {
            throw MuError("unit `" + un + "` is ambiguous: both " + flat + " and " + slashed +
                          " exist (keep one)");
        }
        if (has_flat) return flat;
        if (has_slashed) return slashed;
        return "";
    };

    std::function<std::string(const std::string&)> find_file = [&](const std::string& un) {
        const auto seg = split_unit(un);
        // prefixes[k] = the directory holding the first k-1 segments; k = n is "next to the unit",
        // k = 1 is "at the search root", so a nearer shadow wins over a farther one.
        auto candidates = [&](const std::string& root) {
            std::vector<std::string> out;
            std::vector<std::string> prefixes(seg.size() + 1);
            prefixes[1] = root;
            for (size_t k = 2; k <= seg.size(); ++k) prefixes[k] = prefixes[k - 1] + "/" + seg[k - 2];
            for (size_t k = seg.size(); k >= 1; --k)
                out.push_back(prefixes[k] + "/platform/" + os_name);
            out.push_back(root);
            return out;
        };
        for (const auto& root : dirs) {
            for (const auto& d : candidates(root)) {
                if (std::string p = try_dir(d, un); !p.empty()) return p;
            }
        }
        std::string list;
        size_t shown = 0;
        const std::string rel = dotted_path(un) + ".mu";
        for (const auto& root : dirs) {
            if (!fs::exists(root)) continue;  // do not report roots that are not there at all
            for (const auto& d : candidates(root)) {
                if (shown == 24) {
                    list += "\n  ...";
                    break;
                }
                list += "\n  " + d + "/" + rel;
                ++shown;
            }
        }
        throw MuError("cannot find unit `" + un + "` for platform `" + os_name +
                      "`; looked for (platform shadow first, then the portable unit):" + list);
    };

    std::function<Unit*(const std::string&)> visit = [&](const std::string& path) -> Unit* {
        auto unit = parse_file(path);
        std::string name = unit->name;
        check_unit_name(name, unit->line, unit->col, unit->filename);
        auto it = owned.find(name);
        if (it != owned.end()) return it->second.get();
        if (in_progress.count(name)) {
            throw MuError("unit circular dependency: " + name);
        }
        if (verbose) std::cerr << "unit " << name << " <- " << path << "\n";
        in_progress.insert(name);
        Unit* raw = unit.get();
        owned[name] = std::move(unit);
        for (auto& use : raw->uses) {
            check_unit_name(use->unit_name, use->line, use->col, raw->filename);
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
    // Exception is the root of every throwable class; import it implicitly like std.string.
    (void)visit(find_file("std.exception"));
    // `para` lowers to std.task primitives, so the front-end is always available (zero runtime
    // cost when no `para` is used; the platform backend is selected by the search path).
    (void)visit(find_file("std.task"));
    // `std.thread` stays implicitly available as well (thread/mutex/condvar primitives).
    (void)visit(find_file("std.thread"));
    // Compiler-facing contracts for `for-in` and the built-in capabilities (SPEC 6.4.1/6.4.2,
    // 3.15.7); declarations only.
    (void)visit(find_file("std.interfaces"));

    Unit* entry_unit = visit(entry_path);
    entry_name = entry_unit->name;

    std::vector<std::unique_ptr<Unit>> result;
    result.reserve(post_order.size());
    for (auto& name : post_order) result.push_back(std::move(owned[name]));
    return result;
}
