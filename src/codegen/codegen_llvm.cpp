// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Public interface of the LLVM backend: emits textual IR (--emit-ir) and COFF object files.
//
// The actual code generation lives in the Emitter class declared in codegen_impl.hpp,
// whose member functions are split by functionality across the seven translation units:
// codegen_support / codegen_runtime / codegen_expr / codegen_expr2 / codegen_stmt / codegen_class / codegen_func.
//
// owns:      the backend's public interface: textual IR (--emit-ir) and the COFF object file
// not:       code generation itself (the Emitter class)
// pass:      called once by the driver after sema
// concept:   codegen-api
#include "codegen_llvm.hpp"

#include "codegen_impl.hpp"

#include <string>

using namespace llvm;

// Register the targets this LLVM build actually contains. `InitializeAllTargets` names every target
// LLVM knows about, so an x86-only static link misses `LLVMInitializeLanai/Mips/...`. The CMake side
// defines MUC_TARGET_<ARCH> from LLVM_TARGETS_TO_BUILD; a build that reports no target list keeps the
// all-in-one path.
static void init_targets() {
#if defined(MUC_TARGET_X86) || defined(MUC_TARGET_AARCH64)
#if defined(MUC_TARGET_X86)
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
#endif
#if defined(MUC_TARGET_AARCH64)
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
#endif
#else
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
#endif
}

std::string llvm_emit_text(Program& prog, const EmitOptions& opts) {
    init_targets();
    muc_detail::Emitter em{prog, opts.debug, opts.bounds, opts.target};
    em.singleThread = opts.single_thread;
    em.exportMode = opts.export_mode;
    for (auto& e : opts.extra_exports) em.exports.insert(e);
    em.run();
    std::string out;
    raw_string_ostream os(out);
    em.mod->print(os, nullptr);
    os.flush();
    return out;
}

void llvm_emit_object(Program& prog, const std::string& obj_path, const EmitOptions& opts) {
    init_targets();
    muc_detail::Emitter em{prog, opts.debug, opts.bounds, opts.target};
    em.singleThread = opts.single_thread;
    em.exportMode = opts.export_mode;
    for (auto& e : opts.extra_exports) em.exports.insert(e);
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
