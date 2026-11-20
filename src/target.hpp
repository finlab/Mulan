// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Target triples, the host target and target/platform consistency.
//
// owns:      `TargetInfo` and its API: triple parsing and normalization, host detection, the
//            cross-target checks and the post-link checks (output format, ELF interpreter, ABI)
// not:       any compiler phase; argument parsing and the build driver live in main.cpp
// pass:      driver
// concept:   target, platform

#ifndef MUC_TARGET_HPP
#define MUC_TARGET_HPP

#include <filesystem>
#include <string>
#include <vector>

#include "common.hpp"
#include "loader.hpp"  // PlatformSpec

// A build produces one object file, for the target named by `--target` (the host by default).
// The platform (`--platform os[-arch[-abi]]`) selects the unit set that goes into it. Both have to
// describe the same machine: units written for another architecture or another C runtime describe a
// program that cannot be the object file, so `build` refuses them instead of producing a binary that
// only looks right.
struct TargetInfo {
    std::string os;    // windows / linux / macos ("" = unknown)
    std::string arch;  // x86_64 / aarch64 ("" = unknown)
    std::string abi;   // gnu / musl / msvc / ucrt ("" = unknown)
};

bool posix_libc_target(const std::string& target);
int arch_ptr_bits(const std::string& arch);
std::string normalize_arch(const std::string& arch);
std::string normalize_abi(const std::string& abi);
bool windows_abi(const std::string& abi);
// Whether a triple names a Windows target (windows/msvc/ucrt/mingw/cygwin all use the MS ABI).
bool is_windows_target(const std::string& triple);
std::vector<std::string> split_dash(const std::string& s);
TargetInfo parse_triple(const std::string& triple);
void check_explicit_target(const std::string& triple);
TargetInfo host_target();
bool is_cross_target(const std::string& triple);
std::string suggest_target(const std::string& os, const std::string& arch, const std::string& abi);
void check_output_format(const std::filesystem::path& out, const std::string& target);
std::string elf_interpreter(const std::filesystem::path& path);
void check_linked_abi(const std::filesystem::path& out, const std::string& target);
void check_target_abi_os(const std::string& triple);
void check_platform_matches_target(const std::string& platform_spec, const std::string& option,
                                   const PlatformSpec& spec, const TargetInfo& target);

#endif  // MUC_TARGET_HPP
