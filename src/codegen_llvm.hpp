// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// LLVM backend (stage B): generate COFF object files via the LLVM C++ API
#pragma once

#include <string>
#include <vector>

#include "sema.hpp"

// Which declarations form the binary export surface.
//   ExportOnly: only declarations marked `export` (precise; the default)
//   PublicOnly: every `public` declaration (convenient when everything should be reachable)
enum class ExportMode { ExportOnly, PublicOnly };

struct EmitOptions {
    bool debug = false;
    bool bounds = true;
    ExportMode export_mode = ExportMode::ExportOnly;
    // Extra symbol names to keep externally visible (`--export <name>`), on top of the surface.
    std::vector<std::string> extra_exports;
};

// Emit textual IR (debug / --emit-ir).
std::string llvm_emit_text(Program& prog, const EmitOptions& opts = {});

// Emit a COFF object file. In the executable target every definition outside the export surface is
// internalized before optimization, so unreachable code is removed (agents/toolchain-plan.md §11).
void llvm_emit_object(Program& prog, const std::string& obj_path, const EmitOptions& opts = {});
