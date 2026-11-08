// LLVM 后端（阶段 B）：LLVM C++ API 生成 COFF 目标文件
#pragma once

#include <string>

#include "sema.hpp"

// 生成文本 IR（调试 / --emit-ir）
std::string llvm_emit_text(Program& prog);

// 生成 COFF 目标文件；失败抛 MuError
void llvm_emit_object(Program& prog, const std::string& obj_path);
