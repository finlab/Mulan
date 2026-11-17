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
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "common.hpp"
#include "parser.hpp"

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

bool one_of(const std::string& v, const std::vector<std::string>& allowed) {
    for (const auto& a : allowed)
        if (a == v) return true;
    return false;
}

// The shadow directories a platform spec selects, most specific first.
std::vector<std::string> platform_shadow_keys(const PlatformSpec& spec) {
    std::vector<std::string> keys;
    if (!spec.arch.empty() && !spec.abi.empty()) keys.push_back(spec.os + "-" + spec.arch + "-" + spec.abi);
    if (!spec.arch.empty()) keys.push_back(spec.os + "-" + spec.arch);
    keys.push_back(spec.os);
    return keys;
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
#elif defined(__APPLE__)
    // macOS has no /proc; `_NSGetExecutablePath` returns a possibly relative, symlink-laden path.
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        fs::path p = fs::canonical(buf, ec);
        if (!ec) return p.parent_path();
    }
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
        // Windows paths contain a drive colon (`C:\a;D:\b`), so only `;` separates there; POSIX uses `:`.
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        std::string s = env;
        std::string cur;
        for (char c : s) {
            if (c == separator) {
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

// A platform spec is `os[-arch[-abi]]` (e.g. `windows-x86_64-ucrt`). The loader resolves each unit
// against the most specific shadow directory that exists:
//   platform/<os>-<arch>-<abi>/  ->  platform/<os>-<arch>/  ->  platform/<os>/
// All three levels are optional except the OS; a unit only needs the level where it really differs.
PlatformSpec parse_platform_spec(const std::string& requested) {
    PlatformSpec spec;
    if (requested.empty()) {
        spec.os = host_os();
        return spec;
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t dash = requested.find('-', start);
        parts.push_back(requested.substr(start, dash == std::string::npos ? std::string::npos : dash - start));
        if (dash == std::string::npos) break;
        start = dash + 1;
    }
    if (parts.empty() || parts.size() > 3 || parts[0].empty()) {
        throw MuError("bad platform `" + requested + "`: expected `os[-arch[-abi]]`");
    }
    if (!is_platform_os(parts[0])) {
        std::string known;
        for (const auto& o : platform_os_names()) known += (known.empty() ? "" : ", ") + o;
        throw MuError("unknown platform OS `" + parts[0] + "` (known: " + known + ")");
    }
    spec.os = parts[0];
    if (parts.size() >= 2) {
        if (!one_of(parts[1], {"x86_64", "aarch64"})) {
            throw MuError("unknown platform arch `" + parts[1] + "` (known: x86_64, aarch64)");
        }
        spec.arch = parts[1];
    }
    if (parts.size() >= 3) {
        if (parts[2].empty()) throw MuError("bad platform `" + requested + "`: empty ABI");
        spec.abi = parts[2];
    }
    return spec;
}

// Unit files are resolved through the host filesystem, which on Windows is case-insensitive: `unit
// Foo.Bar;` happily finds `libs/foo/bar.mu` there and then fails on Linux. When a resolved path differs
// from the requested spelling only by case, report it as an error (agents/portability-plan.md #13).
static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

static std::string case_mismatch(const std::string& path) {
    fs::path p = fs::path(path).lexically_normal();
    fs::path cur = p.root_path();
    for (const auto& part : p.relative_path()) {
        const std::string wanted = part.string();
        const fs::path next = cur.empty() ? fs::path(wanted) : cur / wanted;
        std::error_code ec;
        bool exact = false;
        std::string other;
        for (fs::directory_iterator it(cur.empty() ? fs::path(".") : cur, ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name == wanted) { exact = true; break; }
            if (other.empty() && iequals(name, wanted)) other = name;
        }
        if (!exact && !other.empty()) return next.string() + " (on disk: " + other + ")";
        if (ec) return "";
        cur = next;
    }
    return "";
}

std::string host_platform_os() { return host_os(); }

// The CPU architecture the compiler itself runs on, in the platform spec spelling. The MSVC and
// the GCC/Clang macros are both checked, because the two toolchains spell the same CPU
// differently (`_M_ARM64` vs `__aarch64__`).
std::string host_platform_arch() {
#if defined(_M_ARM64) || defined(_M_ARM64EC) || defined(__aarch64__)
    return "aarch64";
#else
    // The platform spec only knows the 64-bit architectures, so every other host (x86-64 on
    // MSVC and on GCC alike) is `x86_64`.
    return "x86_64";
#endif
}

// The C runtime the host uses. Only glibc can be identified reliably (from the headers the
// compiler was built against): on Windows the runtime is decided by the link driver rather than
// by the compiler, and musl does not define a macro that tells it apart from other libcs. An
// empty answer means "unknown", and an unknown ABI must never be reported as a mismatch.
std::string host_platform_abi() {
#if defined(_WIN32)
    return "";
#elif defined(__GLIBC__)
    return "gnu";
#else
    return "";
#endif
}

std::vector<std::unique_ptr<Unit>> load_units(const std::string& entry_path,
                                              const std::vector<std::string>& search_dirs,
                                              std::string& entry_name, const std::string& platform_os,
                                              bool verbose) {
    std::vector<std::string> dirs;
    add_unique(dirs, dirname_of(entry_path));
    for (auto& d : search_dirs) add_unique(dirs, d);
    add_default_lib_dirs(dirs);

    // A `-I` directory that happens to contain `std/` would silently replace a trusted standard
    // library unit (layout and ABI included). Keep the precedence but say so: `std.*` units are the
    // compiler's implicit L3 surface, never a user extension point.
    std::set<std::string> user_roots;
    auto canonical = [](const std::string& p) {
        std::error_code ec;
        fs::path c = fs::weakly_canonical(fs::path(p), ec);
        return ec ? p : c.string();
    };
    for (const auto& d : search_dirs) user_roots.insert(canonical(d));

    const PlatformSpec spec = parse_platform_spec(platform_os);
    const std::vector<std::string> shadow_keys = platform_shadow_keys(spec);
    const std::string os_name = spec.os;

    std::map<std::string, std::unique_ptr<Unit>> owned;
    std::set<std::string> in_progress;
    std::vector<std::string> post_order;

    // Accepted file names per unit: the flat dotted name (`std.thread.backend.mu`) and the slashed
    // path (`std/thread/backend.mu`). Inside a platform shadow directory the segments already present
    // in the directory path may be dropped (`libs/cstd/platform/windows/stdio.mu` = `cstd.stdio`);
    // the full name is still accepted, and having both is an error.
    struct Cand {
        std::string dir;
        size_t dropped;  // leading unit segments represented by `dir`
    };

    auto try_dir = [&](const std::string& d, const std::string& un, size_t dropped) -> std::string {
        std::vector<std::string> names{un};
        if (dropped > 0) {
            auto seg = split_unit(un);
            if (dropped < seg.size()) {
                std::string short_name = seg[dropped];
                for (size_t i = dropped + 1; i < seg.size(); ++i) short_name += "." + seg[i];
                names.push_back(short_name);
            }
        }
        std::string found;
        for (const auto& n : names) {
            const std::string flat = d + "/" + n + ".mu";
            const std::string slashed = d + "/" + dotted_path(n) + ".mu";
            const bool has_flat = file_exists(flat);
            const bool has_slashed = slashed != flat && file_exists(slashed);
            if (has_flat && has_slashed) {
                throw MuError("unit `" + un + "` is ambiguous: both " + flat + " and " + slashed +
                              " exist (keep one)");
            }
            if (!has_flat && !has_slashed) continue;
            const std::string hit = has_flat ? flat : slashed;
            if (std::string mismatch = case_mismatch(hit); !mismatch.empty()) {
                throw MuError("unit `" + un + "` is spelled with a different case on disk: " + mismatch +
                              "; this would fail on a case-sensitive file system");
            }
            if (!found.empty()) {
                throw MuError("unit `" + un + "` is ambiguous: both " + found + " and " + hit +
                              " exist (keep one)");
            }
            found = hit;
        }
        return found;
    };

    std::function<std::string(const std::string&)> find_file = [&](const std::string& un) {
        const auto seg = split_unit(un);
        // prefixes[k] = the directory holding the first k-1 segments; k = n is "next to the unit",
        // k = 1 is "at the search root", so a nearer shadow wins over a farther one.
        auto candidates = [&](const std::string& root) {
            std::vector<Cand> out;
            std::vector<std::string> prefixes(seg.size() + 1);
            prefixes[1] = root;
            for (size_t k = 2; k <= seg.size(); ++k) prefixes[k] = prefixes[k - 1] + "/" + seg[k - 2];
            for (size_t k = seg.size(); k >= 1; --k)
                for (const auto& key : shadow_keys)
                    out.push_back({prefixes[k] + "/platform/" + key, k - 1});
            out.push_back({root, 0});
            return out;
        };
        for (const auto& root : dirs) {
            for (const auto& c : candidates(root)) {
                if (std::string p = try_dir(c.dir, un, c.dropped); !p.empty()) {
                    if (un.rfind("std.", 0) == 0 && user_roots.count(canonical(root)) != 0) {
                        std::cerr << "warning: unit `" << un << "` is shadowed by " << p
                                  << " (a `-I` directory takes precedence over the standard library; "
                                     "remove it or rename the directory)\n";
                    }
                    return p;
                }
            }
        }
        std::string list;
        size_t shown = 0;
        const std::string rel = dotted_path(un) + ".mu";
        for (const auto& root : dirs) {
            if (!fs::exists(root)) continue;  // do not report roots that are not there at all
            for (const auto& c : candidates(root)) {
                if (shown == 24) {
                    list += "\n  ...";
                    break;
                }
                list += "\n  " + c.dir + "/" + rel;
                ++shown;
            }
        }
        throw MuError("cannot find unit `" + un + "` for platform `" + os_name +
                      "`; looked for (platform shadow first, then the portable unit):" + list);
    };

    // A cheap lexical pre-scan: the unit name and its `use` list (comments stripped). It lets the
    // loader finish the dependencies before parsing the current unit, so `#if` can read their public
    // compile-time constants (P3). A `use` inside a dead `#if` arm is not known here; a missing
    // dependency is ignored and reported only if the live parse reaches it.
    auto prescan = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string clean;
        clean.reserve(text.size());
        for (size_t i = 0; i < text.size();) {
            if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                i += 2;
                while (i < text.size() && text[i] != '\n') ++i;
                continue;
            }
            if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
                i += 2;
                while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
                i += 2;
                continue;
            }
            clean += text[i++];
        }
        std::string name;
        std::vector<std::string> uses;
        for (size_t i = 0; i < clean.size();) {
            const char c = clean[i];
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t j = i + 1;
                while (j < clean.size() &&
                       (std::isalnum(static_cast<unsigned char>(clean[j])) || clean[j] == '_' || clean[j] == '.'))
                    ++j;
                const std::string word = clean.substr(i, j - i);
                if (word == "unit" || word == "use") {
                    size_t k = j;
                    while (k < clean.size() && std::isspace(static_cast<unsigned char>(clean[k]))) ++k;
                    size_t m = k;
                    while (m < clean.size() &&
                           (std::isalnum(static_cast<unsigned char>(clean[m])) || clean[m] == '_' || clean[m] == '.'))
                        ++m;
                    size_t s = m;
                    while (s < clean.size() && std::isspace(static_cast<unsigned char>(clean[s]))) ++s;
                    if (m > k && s < clean.size() && clean[s] == ';') {
                        const std::string id = clean.substr(k, m - k);
                        if (word == "unit" && name.empty()) name = id;
                        else if (word == "use") uses.push_back(id);
                    }
                }
                i = j;
                continue;
            }
            ++i;
        }
        return std::make_pair(name, uses);
    };
    std::set<std::string> prescan_stack;

    std::function<Unit*(const std::string&)> visit = [&](const std::string& path) -> Unit* {
        const auto pre = prescan(path);
        if (!pre.first.empty()) {
            if (prescan_stack.count(pre.first)) {
                throw MuError("unit circular dependency: " + pre.first);
            }
            if (auto done = owned.find(pre.first); done != owned.end()) return done->second.get();
            prescan_stack.insert(pre.first);
            for (const auto& u : pre.second) {
                std::string dep_path;
                try {
                    dep_path = find_file(u);
                } catch (const MuError&) {
                    continue;  // may be a dead-branch use; the live parse reports it if reached
                }
                (void)visit(dep_path);
            }
            prescan_stack.erase(pre.first);
        }
        auto unit = parse_file(path);
        std::string name = unit->name;
        check_unit_name(name, unit->line, unit->col, unit->filename);
        auto it = owned.find(name);
        // The cycle check must come first: the unit is inserted into `owned` before its dependencies
        // are visited, so a back edge would otherwise return the half-built unit and hide the cycle.
        if (in_progress.count(name)) {
            throw MuError("unit circular dependency: " + name);
        }
        if (it != owned.end()) return it->second.get();
        if (verbose) std::cerr << "unit " << name << " <- " << path << "\n";
        in_progress.insert(name);
        Unit* raw = unit.get();
        owned[name] = std::move(unit);
        // A `use` inside a conditional declaration arm is not in `raw->uses` yet (the arm is selected
        // by the semantic pass), but its dependency must still be available. Load every candidate
        // first; a missing one is reported only if the selected arm actually uses it (P3.3).
        std::vector<std::string> candidate_uses;
        std::function<void(Decl*)> scan_uses = [&](Decl* d) {
            if (!d) return;
            if (auto* dd = dynamic_cast<DirectiveDecl*>(d)) {
                for (auto& arm : dd->arms) {
                    for (auto& inner : arm.decls) scan_uses(inner.get());
                }
                return;
            }
            if (auto* u = dynamic_cast<UseDecl*>(d)) candidate_uses.push_back(u->unit_name);
        };
        for (auto& d : raw->decls) scan_uses(d.get());
        for (const auto& cu : candidate_uses) {
            std::string dep_path;
            try {
                dep_path = find_file(cu);
            } catch (const MuError&) {
                continue;  // the semantic pass reports it only if the arm is selected
            }
            (void)visit(dep_path);
        }
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
