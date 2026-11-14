// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// LLVM backend (stage B): generate COFF object files via the LLVM C++ API
#pragma once

#include <string>

#include "sema.hpp"

// Emit textual IR (debug / --emit-ir)
std::string llvm_emit_text(Program& prog, bool bounds = true);

// Emit a COFF object file; when debug_info is true, DWARF is attached (-g)
void llvm_emit_object(Program& prog, const std::string& obj_path, bool debug_info = false,
                      bool bounds = true);
