// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// unit loading
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"

// Starting from the entry file, recursively resolve same-named .mu through use; returns the unit list with dependencies first and the entry last.
// entry_name receives the entry unit name. `platform_os` selects the `platform/<os>/` shadow directories
// (empty = host). Throws MuError on failure.
std::vector<std::unique_ptr<Unit>> load_units(const std::string& entry_path,
                                              const std::vector<std::string>& search_dirs,
                                              std::string& entry_name,
                                              const std::string& platform_os = "",
                                              bool verbose = false);

// The `platform/<os>/` directory name of the host (windows / linux / macos).
std::string host_platform_os();

// The host CPU architecture in the platform spec spelling (`x86_64` / `aarch64`).
std::string host_platform_arch();

// The host C runtime family. Only `gnu` (glibc) can be identified from the headers this compiler was
// built against; every other host returns empty ("unknown"), and an unknown ABI is never reported as
// a mismatch.
std::string host_platform_abi();

// A platform spec is `os[-arch[-abi]]` (e.g. `windows-x86_64-ucrt`): the OS selects the
// `platform/<os>/` shadow directories, the arch and the abi narrow the selection further.
struct PlatformSpec {
    std::string os;
    std::string arch;
    std::string abi;
};

// Parse `os[-arch[-abi]]`; throws MuError on a bad spec. An empty request means the host.
PlatformSpec parse_platform_spec(const std::string& requested);
