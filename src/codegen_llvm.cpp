// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// LLVM 后端对外接口：生成文本 IR（--emit-ir）与 COFF 目标文件。
//
// 真正的代码生成在 codegen_impl.hpp 声明的 Emitter 类里，成员函数按功能分散在
// codegen_support / codegen_runtime / codegen_expr / codegen_expr2 /
// codegen_stmt / codegen_class / codegen_func 七个编译单元中。
#include "codegen_llvm.hpp"

#include "codegen_impl.hpp"

#include <string>

using namespace llvm;

std::string llvm_emit_text(Program& prog) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    muc_detail::Emitter em{prog};
    em.run();
    std::string out;
    raw_string_ostream os(out);
    em.mod->print(os, nullptr);
    os.flush();
    return out;
}

void llvm_emit_object(Program& prog, const std::string& obj_path, bool debug_info) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    muc_detail::Emitter em{prog, debug_info};
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
