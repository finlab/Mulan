// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Compiler directives: the build configuration an `#if` condition may read.
//
// One invocation compiles one target, so the configuration is fixed for the whole process and is
// set once by main.cpp from `--platform` / `--target` / `-g` / `--bounds` / `--single-thread`.
#pragma once

#include <string>
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

// Evaluates an `#if` condition. Returns false and fills `why` on failure; the caller attaches the position.
bool eval_directive_cond(const Expr& e, std::string& why);
