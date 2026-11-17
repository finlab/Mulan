// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Compiler directives: the build configuration an `#if` condition may read.
//
// One invocation compiles one target, so the configuration is fixed for the whole process and is
// set once by main.cpp from `--platform` / `--target` / `-g` / `--bounds` / `--single-thread`.
#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.hpp"

struct BuildConfig {
    std::string os = "windows";    // OS.Windows / OS.Linux / OS.MacOS
    std::string arch = "x86_64";   // Arch.X86_64 / Arch.AArch64
    std::string abi;               // Abi.Msvc / Abi.Ucrt / Abi.Gnu / Abi.None ("" = unknown)
    int ptr_bits = 64;
    bool big_endian = false;
    bool debug = false;
    bool bounds = true;
    bool single_thread = false;
};

const BuildConfig& build_config();
void set_build_config(const BuildConfig& cfg);

// One `<name> = <value>` line per build constant (`muc --dump-config`): the reproduction key of a build.
std::string describe_build_config();

// The canonical members of a target enum, in declaration order (`OS`, `Arch`, `Abi`, `Endian`).
// sema cross-checks `std.target` against these lists so the library declaration cannot drift.
bool builtin_enum_members(const std::string& type, std::vector<std::string>& members);

// The compiler's value of a build constant (`os`, `arch`, `ptr_bits`, ...); false when `name` is not one.
bool builtin_const(const std::string& name, long long& value);

// A user compile-time constant visible to `#if` (decided 2026-11-17): a unit-level `const` whose
// initializer folds in the directive evaluator. `#if` conditions use the same name resolution as
// constant initializers: a declaration-list position sees only the constants declared before it; a
// function body is checked after the unit is collected and sees every constant of the unit.
struct UserConst {
    long long value = 0;
    bool is_bool = false;
    // Non-empty when the constant is an enum value (`os`, `OS.Windows`): the evaluator keeps the
    // enum identity so `#if` compares enum values only with the same enum type.
    std::string enum_ns;
};
using UserConstTable = std::unordered_map<std::string, UserConst>;

// Evaluates an `#if` condition (the semantic pass owns the arm selection: sema_directive.cpp). Returns
// false and fills `why` on failure; the caller attaches the position.
// `user_consts` extends the evaluator with the constants visible at this point (may be null);
// `ambiguous` names constants that came from several `use`d units and need a qualified reference.
bool eval_directive_cond(const Expr& e, std::string& why, const UserConstTable* user_consts = nullptr,
                         const std::unordered_set<std::string>* ambiguous = nullptr);

// Evaluates a unit-level const initializer into `out` (used to record it for later `#if` conditions).
bool eval_user_const(const Expr& e, const UserConstTable& table, UserConst& out, std::string& why);

// Fills `table` with the compiler-provided compile-time constants (`os`, `arch`, `ptr_bits`, ...) and
// their enum members (`OS.Windows`, ...). Called when a unit writes `use std.comptime;` — before that
// the names are not visible (decided 2026-11-17).
void add_builtin_constants(UserConstTable& table);

// The compiler-provided compile-time functions (`is_windows`, `is_linux`, `is_macos`). `args` is the
// folded argument list; only the no-argument forms exist so far.
bool builtin_func(const std::string& name, const std::vector<long long>& args, long long& value);

// The constant tables are registered by the parser (a bootstrap for template bodies) and re-registered
// by `collect` with the values of the declarations that survived `#if` selection.
// Public compile-time constants of a unit, available to a later `use` in another unit (P3).
void register_unit_public_consts(const std::string& unit, const UserConstTable& table);
const UserConstTable* unit_public_consts(const std::string& unit);
// All compile-time constants of a unit (private ones included), used by the `#if` expansion inside that
// unit (declaration lists follow text order; a function body is expanded after collect).
void register_unit_all_consts(const std::string& unit, const UserConstTable& table);
const UserConstTable* unit_all_consts(const std::string& unit);
void reset_unit_public_consts();
