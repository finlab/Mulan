// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Build configuration and `#if` condition evaluation.
//
// The condition language is deliberately tiny: constants, integer/bool/float literals, the comparison
// and arithmetic operators, and `and` / `or` / `not`. The conditions are evaluated by the semantic
// pass, once the units are parsed and the compile-time constants are known (sema_directive.cpp); this
// file only evaluates an expression against a table of visible constants. Anything that would need a
// type (a call other than the `comptime` builtins, a variable, a `sizeof` form) is rejected with a
// message that says what to write instead.
//
// owns:      build configuration and the #if condition language (constants, comparisons, boolean operators) shared by the parser and the #if pass
// not:       parsing (parser_directive.cpp) or applying the arms (sema_directive.cpp)
// pass:      shared by parsing and the #if pass
// concept:   directives, build-config
#include "directive.hpp"
#include "target.hpp"

#include <algorithm>
#include <climits>
#include <string>
#include <vector>

// `a`, `a.b`, `a.b.c` → the dotted name; empty for anything else (a call, an index, ...). Shared with the
// semantic pass, which resolves the same qualified forms (`std.comptime.ptr_bits`) outside `#if`.
namespace {
std::string dotted_name(const Expr& e) {
    if (const auto* n = dynamic_cast<const Name*>(&e)) return n->ident;
    if (const auto* m = dynamic_cast<const Member*>(&e)) {
        if (!m->type_args.empty()) return {};
        std::string base = dotted_name(*m->obj);
        if (base.empty()) return {};
        return base + "." + m->name;
    }
    return {};
}
}  // namespace

namespace {

enum class VK { Int, Bool, Float };
struct V {
    VK k = VK::Int;
    long long i = 0;
    double f = 0;
    // The enum a value came from (`OS`, `Arch`, `Abi`, `Endian`), empty for plain numbers and bools.
    // `#if` follows the same rule as ordinary code: enum values compare only with the same enum.
    std::string enum_ns;
    // Integer width of the value: an `int` operand wraps at 32 bits, a `long` at 64. Constant folding
    // must follow the language's arithmetic, or `#if MAX + 1 == MIN` with `const int`s answers as if the
    // sum were 64-bit (#71, #70, #76).
    int bits = 32;
    bool is_signed = true;
};

// Two's-complement wrap to `bits`, the layout the language fixes for integer arithmetic (SPEC 8.2).
long long wrap_to(long long v, int bits, bool is_signed) {
    if (bits >= 64) return v;
    if (bits <= 0) bits = 32;
    const unsigned long long mask = (1ull << bits) - 1;
    unsigned long long u = static_cast<unsigned long long>(v) & mask;
    if (is_signed && (u >> (bits - 1)) & 1ull) u |= ~mask;
    return static_cast<long long>(u);
}

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

// The unit that declares the compiler-provided compile-time names (SPEC 9.3).
const std::string kComptimeUnit = "std.comptime";

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
    // Equality and ordering above accept two values of the same enum; everything below is arithmetic
    // or bit manipulation, which ordinary code rejects on an enum without an explicit `as` (SPEC 3.7.2),
    // and `#if` follows the same rules (#73). Without this, `OS.Windows + OS.Linux` silently read the
    // member indices as integers and selected a platform arm that the type system would not allow.
    if (!l.enum_ns.empty() || !r.enum_ns.empty()) {
        why = "`" + op + "` needs integers in a `#if` condition; an enum value must be converted "
              "explicitly (write `X as int`)";
        return false;
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
        // The common integer type of the operands: the wider one, unsigned if either is unsigned. The
        // operation runs in `long long` through unsigned arithmetic (well-defined wrap) and is then
        // narrowed to that type, which is what the runtime would produce (#70, #71).
        const int bits = std::max(l.bits, r.bits);
        const bool sgn = l.is_signed && r.is_signed;
        long long res = 0;
        if (op == "+") res = static_cast<long long>(static_cast<unsigned long long>(l.i) +
                                                    static_cast<unsigned long long>(r.i));
        else if (op == "-") res = static_cast<long long>(static_cast<unsigned long long>(l.i) -
                                                         static_cast<unsigned long long>(r.i));
        else if (op == "*") res = static_cast<long long>(static_cast<unsigned long long>(l.i) *
                                                         static_cast<unsigned long long>(r.i));
        else if (op == "/") res = l.i / r.i;
        else if (op == "%") res = l.i % r.i;
        else if (op == "&") res = l.i & r.i;
        else if (op == "|") res = l.i | r.i;
        else if (op == "^") res = l.i ^ r.i;
        else if (op == "<<") {
            if (r.i < 0 || r.i >= bits) {
                why = "shift count out of range in a `#if` condition";
                return false;
            }
            res = static_cast<long long>(static_cast<unsigned long long>(l.i) << r.i);
        } else if (op == ">>") {
            if (r.i < 0 || r.i >= bits) {
                why = "shift count out of range in a `#if` condition";
                return false;
            }
            res = l.i >> r.i;
        } else res = l.i >> r.i;
        out = vint(wrap_to(res, bits, sgn));
        out.bits = bits;
        out.is_signed = sgn;
        return true;
    }
    why = "`" + op + "` is not allowed in a `#if` condition";
    return false;
}

// The user constants visible to the condition being evaluated (set by `eval_directive_cond`).
const UserConstTable* g_user_consts = nullptr;
// Bare names that came from several `use`d units (a qualified reference is required).
const std::unordered_set<std::string>* g_ambiguous = nullptr;
// Whether `use std.comptime` is visible at this point: the compile-time functions need it, like any
// other name.
bool g_comptime_visible = true;

bool eval(const Expr& e, V& out, std::string& why) {
    if (const auto* p = dynamic_cast<const IntLit*>(&e)) {
        // A `2^63` magnitude is only valid directly after unary `-` (there it becomes LLONG_MIN), so
        // one reaching the `#if` evaluator is out of range — never silently 0.
        if (p->min_magnitude) {
            why = "integer literal is too large (maximum 2^63-1): a 2^63 magnitude is only valid "
                  "directly after unary `-`";
            return false;
        }
        out = vint(p->value);
        // A literal takes the narrowest of the two fixed widths that holds it: `1` is an `int`, a value
        // past the 32-bit range is a `long` (SPEC 3.2 fixes `int`/`long` at 32/64 bits and gives no
        // suffix). Without this, `-9223372036854775807 - 1` would be narrowed to 32 bits (#70).
        out.bits = (p->value >= -2147483647LL - 1 && p->value <= 2147483647LL) ? 32 : 64;
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
        // 16777217.0f is a float32 literal: round it before the comparison, exactly as the runtime
        // does, or #if 16777217.0f == 16777216.0 disagrees with the same test in ordinary code (#79).
        out = vflt(p->is_float32 ? static_cast<double>(static_cast<float>(p->value)) : p->value);
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
                out = it->second.is_float  ? vflt(it->second.fvalue)
                      : it->second.is_bool ? vbool(it->second.value != 0)
                                           : vint(it->second.value);
                out.enum_ns = it->second.enum_ns;
                out.bits = it->second.is_bool ? 1 : it->second.bits;
                out.is_signed = it->second.is_signed;
                return true;
            }
        }
        why = "`" + p->ident + "` is not a visible compile-time constant; a `#if` condition resolves "
              "names normally (write `use std.comptime;` for the build constants)";
        return false;
    }
    if (const auto* p = dynamic_cast<const Member*>(&e)) {
        // A qualified constant: `unit.name` (and `std.comptime.os` for the build constants).
        const std::string full = dotted_name(e);
        if (!full.empty() && g_user_consts) {
            auto it = g_user_consts->find(full);
            if (it != g_user_consts->end()) {
                out = it->second.is_float  ? vflt(it->second.fvalue)
                      : it->second.is_bool ? vbool(it->second.value != 0)
                                           : vint(it->second.value);
                out.enum_ns = it->second.enum_ns;
                out.bits = it->second.is_bool ? 1 : it->second.bits;
                out.is_signed = it->second.is_signed;
                return true;
            }
        }
        if (full.rfind("comptime.", 0) == 0) {
            why = "`comptime` is not a unit name: the compile-time unit is `std.comptime` (write `std." +
                  full + "`)";
            return false;
        }
        why = "`" + (full.empty() ? std::string(p->name) : full) +
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
            if (p->op == "-") { out = vint(wrap_to(static_cast<long long>(0ull - static_cast<unsigned long long>(v.i)), v.bits, v.is_signed)); out.bits = v.bits; out.is_signed = v.is_signed; return true; }
            if (p->op == "~") { out = vint(wrap_to(~v.i, v.bits, v.is_signed)); out.bits = v.bits; out.is_signed = v.is_signed; return true; }
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
        const std::string& n = p->spec->name;
        const int bits = n == "byte" || n == "char" || n == "bool"  ? 8
                         : n == "short" || n == "ushort"            ? 16
                         : n == "long" || n == "ulong" || n == "usize" || n == "isize" ? 64
                                                                    : 32;
        const bool sgn = n != "uint" && n != "ulong" && n != "usize" && n != "byte" && n != "bool";
        out = vint(wrap_to(v.k == VK::Float ? static_cast<long long>(v.f) : v.i, bits, sgn));
        out.bits = bits;
        out.is_signed = sgn;
        return true;
    }
    if (dynamic_cast<const SizeofExpr*>(&e)) {
        why = "`sizeof` is not available in a `#if` condition: bind it first (`const int N = sizeof(T) as "
              "int;`) and compare `N`";
        return false;
    }
    if (const auto* p = dynamic_cast<const Call*>(&e)) {
        // A compiler-provided compile-time function: `is_windows()` (after `use std.comptime`) or the
        // qualified `std.comptime.is_windows()`.
        std::string fn;
        if (const auto* nm = dynamic_cast<const Name*>(p->callee.get())) {
            fn = nm->ident;
        } else {
            const std::string full = dotted_name(*p->callee);
            if (full.rfind(kComptimeUnit + ".", 0) == 0) {
                fn = full.substr(kComptimeUnit.size() + 1);
            } else if (full.rfind("comptime.", 0) == 0) {
                why = "`comptime` is not a unit name: the compile-time unit is `std.comptime` (write "
                      "`std." + full + "()`)";
                return false;
            }
        }
        if (!fn.empty() && !g_comptime_visible) {
            why = "`" + fn + "` is not visible here: write `use std.comptime;` before this `#if` to use "
                  "the compiler-provided compile-time functions";
            return false;
        }
        if (!fn.empty() && (!p->args.empty() || !p->named.empty())) {
            why = "`" + fn + "` takes no arguments: a `#if` condition may only call the "
                  "`std.comptime` compile-time functions (`is_windows()`, `is_linux()`, `is_macos()`)";
            return false;
        }
        if (!fn.empty()) {
            long long v = 0;
            if (builtin_func(fn, {}, v)) {
                out = vbool(v != 0);
                return true;
            }
            why = "`" + fn + "` is not a compile-time function; `std.comptime` declares "
                  "`is_windows()`, `is_linux()` and `is_macos()`";
            return false;
        }
        why = "a function call is not a build constant; a `#if` condition may only use compile-time "
              "constants and call the `std.comptime` compile-time functions";
        return false;
    }
    why = "`#if` conditions may only use compile-time constants, literals and arithmetic/comparison "
          "operators";
    return false;
}

}  // namespace

namespace muc_detail {
// The anonymous-namespace helper above; exposed for the semantic pass (see directive.hpp).
std::string expr_dotted_name(const Expr& e) { return dotted_name(e); }
}  // namespace muc_detail

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
    if (name == "abi") {
        const std::string abi = normalize_abi(c.abi);
        value = abi.empty() ? index_of(kAbi, "None") : index_of_ci(kAbi, abi);
        return true;
    }
    if (name == "endian") { value = c.big_endian ? 1 : 0; return true; }
    if (name == "ptr_bits") { value = c.ptr_bits; return true; }
    if (name == "debug") { value = c.debug ? 1 : 0; return true; }
    if (name == "bounds") { value = c.bounds ? 1 : 0; return true; }
    if (name == "null_check") { value = c.null_check ? 1 : 0; return true; }
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
    s += std::string("null_check = ") + (c.null_check ? "true" : "false") + "\n";
    s += std::string("single_thread = ") + (c.single_thread ? "true" : "false") + "\n";
    return s;
}

bool eval_directive_cond(const Expr& e, std::string& why, const UserConstTable* user_consts,
                         const std::unordered_set<std::string>* ambiguous, bool comptime_visible) {
    g_user_consts = user_consts;
    g_ambiguous = ambiguous;
    g_comptime_visible = comptime_visible;
    V v;
    const bool ok = eval(e, v, why);
    g_user_consts = nullptr;
    g_ambiguous = nullptr;
    g_comptime_visible = true;
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
    out.is_float = v.k == VK::Float;
    out.fvalue = v.k == VK::Float ? v.f : 0.0;
    out.enum_ns = v.enum_ns;
    return true;
}

void add_builtin_constants(UserConstTable& table) {    static const char* ints[] = {"ptr_bits"};
    static const char* bools[] = {"debug", "bounds", "null_check", "single_thread"};
    static const char* targets[] = {"os", "arch", "abi", "endian"};
    static const char* enums[] = {"OS", "Arch", "Abi", "Endian"};
    for (const char* n : ints) {
        long long v = 0;
        if (builtin_const(n, v)) table[n] = UserConst{v, false, false, 0.0, 64, true, {}};
    }
    for (const char* n : bools) {
        long long v = 0;
        if (builtin_const(n, v)) table[n] = UserConst{v, true, false, 0.0, 1, true, {}};
    }
    for (size_t i = 0; i < 4; ++i) {
        long long v = 0;
        if (builtin_const(targets[i], v)) table[targets[i]] = UserConst{v, false, false, 0.0, 32, true, enums[i]};
    }
    for (const char* n : enums) {
        std::vector<std::string> members;
        if (!builtin_enum_members(n, members)) continue;
        for (size_t i = 0; i < members.size(); ++i) {
            table[std::string(n) + "." + members[i]] =
                UserConst{static_cast<long long>(i), false, false, 0.0, 32, true, n};
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

// All constants of a parsed unit (private included), for the collect-time expansion in that unit.
std::unordered_map<std::string, UserConstTable>& all_consts_table() {
    static std::unordered_map<std::string, UserConstTable> t;
    return t;
}

void register_unit_all_consts(const std::string& unit, const UserConstTable& table) {
    all_consts_table()[unit] = table;
}

const UserConstTable* unit_all_consts(const std::string& unit) {
    auto& t = all_consts_table();
    auto it = t.find(unit);
    return it == t.end() ? nullptr : &it->second;
}
