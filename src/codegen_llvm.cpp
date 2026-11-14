// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Public interface of the LLVM backend: emits textual IR (--emit-ir) and COFF object files.
//
// The actual code generation lives in the Emitter class declared in codegen_impl.hpp,
// whose member functions are split by functionality across the seven translation units:
// codegen_support / codegen_runtime / codegen_expr / codegen_expr2 / codegen_stmt / codegen_class / codegen_func.
#include "codegen_llvm.hpp"

#include "codegen_impl.hpp"

#include <string>

using namespace llvm;

std::string llvm_emit_text(Program& prog, bool bounds, const std::vector<std::string>& exports) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    muc_detail::Emitter em{prog, false, bounds};
    for (auto& e : exports) em.exports.insert(e);
    em.run();
    std::string out;
    raw_string_ostream os(out);
    em.mod->print(os, nullptr);
    os.flush();
    return out;
}

void llvm_emit_object(Program& prog, const std::string& obj_path, bool debug_info, bool bounds,
                      const std::vector<std::string>& exports) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    muc_detail::Emitter em{prog, debug_info, bounds};
    for (auto& e : exports) em.exports.insert(e);
    em.run();
    std::error_code ec;
    raw_fd_ostream dest(obj_path, ec, llvm::sys::fs::OF_None);
    if (ec) throw MuError("cannot write object file " + obj_path + ": " + ec.message());
    llvm::legacy::PassManager pm;
    if (em.tm->addPassesToEmitFile(pm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        throw MuError("LLVM backend: target does not support object file emission");
    }
    pm.run(*em.mod);
    dest.flush();
}
