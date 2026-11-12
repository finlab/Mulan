// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// unit loading
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"

// Starting from the entry file, recursively resolve same-named .mu through use; returns the unit list with dependencies first and the entry last.
// entry_name receives the entry unit name. Throws MuError on failure.
std::vector<std::unique_ptr<Unit>> load_units(const std::string& entry_path,
                                              const std::vector<std::string>& search_dirs,
                                              std::string& entry_name);
