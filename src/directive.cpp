// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Build configuration and `#if` condition evaluation.
//
// The condition language is deliberately tiny: build constants, integer/bool/float literals, the
// comparison and arithmetic operators, and `and` / `or` / `not`. Anything that would need the type
// system (`sizeof`, calls, variables) is rejected with a message that says why: a `#if` is executed
// while parsing, before name resolution and layout exist.
#include "directive.hpp"

#include <climits>
#include <string>
#include <vector>

namespace {

enum class VK { Int, Bool, Float };

struct V {
    VK k = VK::Int;
    long long i = 0;
    double f = 0;
    // The enum a value came from (`OS`, `Arch`, `Abi`, `Endian`), empty for plain numbers and bools.
    // `#if` follows the same rule as ordinary code: enum values compare only with the same enum.
    std::string enum_ns;
};

V vint(long long x) {
    V v;
    v.i = x;
    return v;
}
V vbool(bool b) {
    V v;
    v.k = VK::Bool;
    v.i = b ? 1 : 0;
    return v;
}
V vflt(double d) {
    V v;
    v.k = VK::Float;
    v.f = d;
    return v;
}

bool numeric(const V& v) { return v.k != VK::Bool; }
double as_double(const V& v) { return v.k == VK::Float ? v.f : static_cast<double>(v.i); }

// Declaration order is the value order: `OS.Windows == 0`. `std.comptime` must declare exactly these
// members in this order, and sema checks it (see check_target_unit).
const std::vector<std::string> kOs = {"Windows", "Linux", "MacOS"};
const std::vector<std::string> kArch = {"X86_64", "AArch64"};
const std::vector<std::string> kAbi = {"Msvc", "Ucrt", "Gnu", "Musl", "None"};
const std::vector<std::string> kEndian = {"Little", "Big"};

int index_of(const std::vector<std::string>& v, const std::string& s) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == s) return static_cast<int>(i);
    }
    return -1;
}

std::string lowered(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// The CLI and the platform spec spell values in lower case (`windows`, `x86_64`); the enum members
// are PascalCase (`OS.Windows`). Match case-insensitively, and treat an unknown ABI as `Abi.None`.
int index_of_ci(const std::vector<std::string>& v, const std::string& s) {
    const std::string t = lowered(s);
    for (size_t i = 0; i < v.size(); ++i) {
        if (lowered(v[i]) == t) return static_cast<int>(i);
    }
    return -1;
}

BuildConfig g_config;

const char* const kConstNames =
    "os, arch, abi, ptr_bits, endian, debug, bounds, single_thread";

bool builtin_enum(const std::string& type, const std::vector<std::string>*& out) {
    if (type == "OS") { out = &kOs; return true; }
    if (type == "Arch") { out = &kArch; return true; }
    if (type == "Abi") { out = &kAbi; return true; }
    if (type == "Endian") { out = &kEndian; return true; }
    return false;
}

bool is_int_type_name(const std::string& n) {
    return n == "int" || n == "uint" || n == "long" || n == "ulong" || n == "short" ||
           n == "ushort" || n == "byte" || n == "char" || n == "usize" || n == "isize";
}

bool eval(const Expr& e, V& out, std::string& why);

bool need_bool(const V& v, const char* where, std::string& why) {
    if (v.k == VK::Bool) return true;
    why = std::string("`") + where + "` needs a bool operand; write a comparison such as `os == OS.Windows`";
    return false;
}

bool eval_binary(const Binary& b, V& out, std::string& why) {
    V l, r;
    if (!eval(*b.left, l, why) || !eval(*b.right, r, why)) return false;
    const std::string& op = b.op;
    if (op == "and" || op == "or") {
        if (!need_bool(l, op.c_str(), why) || !need_bool(r, op.c_str(), why)) return false;
        out = vbool(op == "and" ? (l.i && r.i) : (l.i || r.i));
        return true;
    }
    if ((!l.enum_ns.empty() || !r.enum_ns.empty()) && l.enum_ns != r.enum_ns) {
        // Ordinary code rejects this too ("enum `OS` can only be compared ... with values of the same
        // enum type"); without the check `os == Endian.Little` silently matched on raw member index.
        why = "`#if` compares " + (l.enum_ns.empty() ? "an integer" : l.enum_ns) + " with " +
              (r.enum_ns.empty() ? "an integer" : r.enum_ns) +
              "; enum values can only be compared with values of the same enum type";
        return false;
    }
    if (op == "==" || op == "!=") {
        bool eq;
        if (l.k == VK::Float || r.k == VK::Float) eq = as_double(l) == as_double(r);
        else if (numeric(l) && numeric(r)) eq = l.i == r.i;
        else if (l.k == VK::Bool && r.k == VK::Bool) eq = l.i == r.i;
        else {
            why = "`==` compares an integer with a bool; both operands must be the same kind";
            return false;
        }
        out = vbool(op == "==" ? eq : !eq);
        return true;
    }
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        if (!numeric(l) || !numeric(r)) {
            why = "`" + op + "` compares numbers; use `not` for bools";
            return false;
        }
        const bool use_float = l.k == VK::Float || r.k == VK::Float;
        const double a = use_float ? as_double(l) : static_cast<double>(l.i);
        const double c = use_float ? as_double(r) : static_cast<double>(r.i);
        // Compare exactly for integers: a double round-trip loses precision above 2^53.
        bool res;
        if (!use_float) {
            const long long x = l.i, y = r.i;
            res = op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : x >= y;
        } else {
            res = op == "<" ? a < c : op == "<=" ? a <= c : op == ">" ? a > c : a >= c;
        }
        out = vbool(res);
        return true;
    }
    // Integer arithmetic (floats are accepted for the four basic operators only).
    const bool arith = op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
    if (arith && (l.k == VK::Float || r.k == VK::Float)) {
        const double a = as_double(l), c = as_double(r);
        if ((op == "/" || op == "%") && c == 0) {
            why = "division by zero in a `#if` condition";
            return false;
        }
        out = vflt(op == "+" ? a + c : op == "-" ? a - c : op == "*" ? a * c : op == "/" ? a / c : 0);
        if (op == "%") {
            why = "`%` is not defined for floating point in a `#if` condition";
            return false;
        }
        return true;
    }
    if (arith || op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
        if (l.k != VK::Int || r.k != VK::Int) {
            why = "`" + op + "` needs integer operands in a `#if` condition";
            return false;
        }
        if ((op == "/" || op == "%") && r.i == 0) {
            why = "division by zero in a `#if` condition";
            return false;
        }
        // `MIN / -1` and `MIN % -1` are UB in C++; the compiler must not execute them.
        if ((op == "/" || op == "%") && r.i == -1 && l.i == LLONG_MIN) {
            why = "signed overflow in a `#if` condition (the most negative value divided by -1)";
            return false;
        }
        if (op == "+") out = vint(l.i + r.i);
        else if (op == "-") out = vint(l.i - r.i);
        else if (op == "*") out = vint(l.i * r.i);
        else if (op == "/") out = vint(l.i / r.i);
        else if (op == "%") out = vint(l.i % r.i);
        else if (op == "&") out = vint(l.i & r.i);
        else if (op == "|") out = vint(l.i | r.i);
        else if (op == "^") out = vint(l.i ^ r.i);
        else if (op == "<<") out = vint(l.i << r.i);
        else out = vint(l.i >> r.i);
        return true;
    }
    why = "`" + op + "` is not allowed in a `#if` condition";
    return false;
}

// The user constants visible to the condition being evaluated (set by `eval_directive_cond`).
const UserConstTable* g_user_consts = nullptr;
// Bare names that came from several `use`d units (a qualified reference is required).
const std::unordered_set<std::string>* g_ambiguous = nullptr;

bool eval(const Expr& e, V& out, std::string& why) {
    if (const auto* p = dynamic_cast<const IntLit*>(&e)) {
        out = vint(p->value);
        return true;
    }
    if (const auto* p = dynamic_cast<const CharLit*>(&e)) {
        out = vint(p->value);
        return true;
    }
    if (const auto* p = dynamic_cast<const BoolLit*>(&e)) {
        out = vbool(p->value);
        return true;
    }
    if (const auto* p = dynamic_cast<const FloatLit*>(&e)) {
        out = vflt(p->value);
        return true;
    }
    if (const auto* p = dynamic_cast<const Name*>(&e)) {
        // `#if` has no special "build constant" source: the name is whatever is visible at this
        // point (a `use std.comptime;` records the compiler-provided constants as ordinary names).
        if (g_ambiguous && g_ambiguous->count(p->ident)) {
            why = "`" + p->ident + "` is ambiguous: several `use`d units declare it; write a qualified "
                  "name such as `unit." + p->ident + "`";
            return false;
        }
        if (g_user_consts) {
            auto it = g_user_consts->find(p->ident);
            if (it != g_user_consts->end()) {
                out = it->second.is_bool ? vbool(it->second.value != 0) : vint(it->second.value);
                out.enum_ns = it->second.enum_ns;
                return true;
            }
        }
        why = "`" + p->ident + "` is not a visible compile-time constant; a `#if` condition resolves "
              "names normally (write `use comptime;` for the build constants)";
        return false;
    }
    if (const auto* p = dynamic_cast<const Member*>(&e)) {
        const auto* base = dynamic_cast<const Name*>(p->obj.get());
        if (base && g_user_consts) {
            auto it = g_user_consts->find(base->ident + "." + p->name);
            if (it != g_user_consts->end()) {
                out = it->second.is_bool ? vbool(it->second.value != 0) : vint(it->second.value);
                out.enum_ns = it->second.enum_ns;
                return true;
            }
        }
        why = "`" + (base ? base->ident + "." + p->name : p->name) +
              "` is not a visible compile-time constant";
        return false;
    }
    if (const auto* p = dynamic_cast<const NotExpr*>(&e)) {
        V v;
        if (!eval(*p->operand, v, why)) return false;
        if (!need_bool(v, "not", why)) return false;
        out = vbool(v.i == 0);
        return true;
    }
    if (const auto* p = dynamic_cast<const Unary*>(&e)) {
        V v;
        if (!eval(*p->operand, v, why)) return false;
        if (p->op == "!") {
            if (!need_bool(v, "!", why)) return false;
            out = vbool(v.i == 0);
            return true;
        }
        if (v.k == VK::Int) {
            if (p->op == "+") { out = v; return true; }
            if (p->op == "-") { out = vint(-v.i); return true; }
            if (p->op == "~") { out = vint(~v.i); return true; }
        }
        if (v.k == VK::Float && (p->op == "+" || p->op == "-")) {
            out = vflt(p->op == "-" ? -v.f : v.f);
            return true;
        }
        why = "`" + p->op + "` is not allowed on this operand in a `#if` condition";
        return false;
    }
    if (const auto* p = dynamic_cast<const Binary*>(&e)) return eval_binary(*p, out, why);
    if (const auto* p = dynamic_cast<const Cast*>(&e)) {
        V v;
        if (!eval(*p->expr, v, why)) return false;
        if (!p->spec || p->spec->kind != TypeSpec::K::Named || !is_int_type_name(p->spec->name)) {
            why = "a `#if` condition only supports `as` to an integer type";
            return false;
        }
        out = vint(v.k == VK::Float ? static_cast<long long>(v.f) : v.i);
        return true;
    }
    if (dynamic_cast<const SizeofExpr*>(&e)) {
        why = "`sizeof` is not available in a `#if` condition: the condition is evaluated before types "
              "are laid out; use `ptr_bits` instead of `sizeof(void*)`";
        return false;
    }
    if (const auto* p = dynamic_cast<const Call*>(&e)) {
        // A compiler-provided compile-time function (`is_windows()`), no arguments so far.
        const auto* nm = dynamic_cast<const Name*>(p->callee.get());
        if (nm && p->args.empty() && p->named.empty()) {
            long long v = 0;
            if (builtin_func(nm->ident, {}, v)) {
                out = vbool(v != 0);
                return true;
            }
        }
        why = "a function call is not a build constant; a `#if` condition may only use " +
              std::string(kConstNames) + " and the `std.comptime` compile-time functions";
        return false;
    }
    why = "`#if` conditions may only use build constants, literals and arithmetic/comparison operators";
    return false;
}

}  // namespace

const BuildConfig& build_config() { return g_config; }
void set_build_config(const BuildConfig& cfg) { g_config = cfg; }

bool builtin_enum_members(const std::string& type, std::vector<std::string>& members) {
    const std::vector<std::string>* m = nullptr;
    if (!builtin_enum(type, m)) return false;
    members = *m;
    return true;
}

bool builtin_const(const std::string& name, long long& value) {
    const BuildConfig& c = g_config;
    if (name == "os") { value = index_of_ci(kOs, c.os); return true; }
    if (name == "arch") { value = index_of_ci(kArch, c.arch); return true; }
    if (name == "abi") { value = c.abi.empty() ? index_of(kAbi, "None") : index_of_ci(kAbi, c.abi); return true; }
    if (name == "endian") { value = c.big_endian ? 1 : 0; return true; }
    if (name == "ptr_bits") { value = c.ptr_bits; return true; }
    if (name == "debug") { value = c.debug ? 1 : 0; return true; }
    if (name == "bounds") { value = c.bounds ? 1 : 0; return true; }
    if (name == "single_thread") { value = c.single_thread ? 1 : 0; return true; }
    return false;
}

std::string describe_build_config() {
    const BuildConfig& c = g_config;
    auto spelled = [](const char* type, const std::vector<std::string>& members, const std::string& v) {
        const int idx = index_of_ci(members, v);
        if (idx >= 0) return std::string(type) + "." + members[static_cast<size_t>(idx)];
        return "<unknown \"" + v + "\">";
    };
    std::string s;
    s += "os = " + spelled("OS", kOs, c.os) + "\n";
    s += "arch = " + spelled("Arch", kArch, c.arch) + "\n";
    s += "abi = " + spelled("Abi", kAbi, c.abi.empty() ? "None" : c.abi) + "\n";
    s += "ptr_bits = " + std::to_string(c.ptr_bits) + "\n";
    s += "endian = " + spelled("Endian", kEndian, c.big_endian ? "Big" : "Little") + "\n";
    s += std::string("debug = ") + (c.debug ? "true" : "false") + "\n";
    s += std::string("bounds = ") + (c.bounds ? "true" : "false") + "\n";
    s += std::string("single_thread = ") + (c.single_thread ? "true" : "false") + "\n";
    return s;
}

bool eval_directive_cond(const Expr& e, std::string& why, const UserConstTable* user_consts,
                         const std::unordered_set<std::string>* ambiguous) {
    g_user_consts = user_consts;
    g_ambiguous = ambiguous;
    V v;
    const bool ok = eval(e, v, why);
    g_user_consts = nullptr;
    g_ambiguous = nullptr;
    if (!ok) return false;
    if (v.k != VK::Bool) {
        why = "a `#if` condition must be a bool; write a comparison such as `os == OS.Windows`";
        return false;
    }
    return v.i != 0;
}

bool eval_user_const(const Expr& e, const UserConstTable& table, UserConst& out, std::string& why) {
    g_user_consts = &table;
    V v;
    const bool ok = eval(e, v, why);
    g_user_consts = nullptr;
    if (!ok) return false;
    out.is_bool = v.k == VK::Bool;
    out.value = v.k == VK::Bool ? (v.i != 0 ? 1 : 0) : v.i;
    return true;
}

void add_builtin_constants(UserConstTable& table) {    static const char* ints[] = {"ptr_bits"};
    static const char* bools[] = {"debug", "bounds", "single_thread"};
    static const char* targets[] = {"os", "arch", "abi", "endian"};
    static const char* enums[] = {"OS", "Arch", "Abi", "Endian"};
    for (const char* n : ints) {
        long long v = 0;
        if (builtin_const(n, v)) table[n] = UserConst{v, false};
    }
    for (const char* n : bools) {
        long long v = 0;
        if (builtin_const(n, v)) table[n] = UserConst{v, true};
    }
    for (size_t i = 0; i < 4; ++i) {
        long long v = 0;
        if (builtin_const(targets[i], v)) table[targets[i]] = UserConst{v, false, enums[i]};
    }
    for (const char* n : enums) {
        std::vector<std::string> members;
        if (!builtin_enum_members(n, members)) continue;
        for (size_t i = 0; i < members.size(); ++i) {
            table[std::string(n) + "." + members[i]] = UserConst{static_cast<long long>(i), false, n};
        }
    }
}

bool builtin_func(const std::string& name, const std::vector<long long>& args, long long& value) {
    if (!args.empty()) return false;
    const std::string os = lowered(g_config.os);
    if (name == "is_windows") { value = os == "windows" ? 1 : 0; return true; }
    if (name == "is_linux") { value = os == "linux" ? 1 : 0; return true; }
    if (name == "is_macos") { value = os == "macos" ? 1 : 0; return true; }
    return false;
}

// Public constants of units already parsed in this invocation. `#if` in a later unit can read them
// through its `use` (the loader parses dependencies first, P3).
std::unordered_map<std::string, UserConstTable>& public_consts_table() {
    static std::unordered_map<std::string, UserConstTable> t;
    return t;
}

void register_unit_public_consts(const std::string& unit, const UserConstTable& table) {
    public_consts_table()[unit] = table;
}

const UserConstTable* unit_public_consts(const std::string& unit) {
    auto& t = public_consts_table();
    auto it = t.find(unit);
    return it == t.end() ? nullptr : &it->second;
}

void reset_unit_public_consts() { public_consts_table().clear(); }
