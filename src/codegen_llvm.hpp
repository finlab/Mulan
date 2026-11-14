// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// LLVM backend (stage B): generate COFF object files via the LLVM C++ API
#pragma once

#include <string>
#include <vector>

#include "sema.hpp"

// Emit textual IR (debug / --emit-ir). `exports` keeps those symbol names externally visible.
std::string llvm_emit_text(Program& prog, bool bounds = true,
                           const std::vector<std::string>& exports = {});

// Emit a COFF object file; when debug_info is true, DWARF is attached (-g).
// An executable has no external consumers, so every definition except `main` and `exports` is
// internalized before optimization (whole-program dead-code elimination, SPEC/toolchain plan).
void llvm_emit_object(Program& prog, const std::string& obj_path, bool debug_info = false,
                      bool bounds = true, const std::vector<std::string>& exports = {});
