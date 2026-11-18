// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing
#pragma once

#include <memory>
#include <string>

#include "ast.hpp"

std::unique_ptr<Unit> parse_source(const std::string& src, const std::string& filename);
std::unique_ptr<Unit> parse_file(const std::string& path);
