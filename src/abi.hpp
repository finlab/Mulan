// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// C ABI classification for aggregates (B2): how a struct/union travels across the boundary —
// packed into an integer register, by a pointer to a caller-made copy (`byval`), or returned
// through a hidden pointer (`sret`). Only `extern` declarations and `export`ed functions follow
// the C ABI; purely internal calls keep the backend's own (faster) convention.
#pragma once

#include <string>

#include "types.hpp"

namespace muc_detail {

enum class AbiPass {
    // The value is not an aggregate (scalar, pointer, class handle): its own LLVM type is right.
    Direct,
    // C passes a 1/2/4/8-byte aggregate "as if it were an integer of the same size". LLVM would
    // hand each *field* to its own register instead (`{i32, i32}` came back in RAX and RDX, while
    // C packs both halves into RAX), so the frontend moves the bits through memory.
    CoerceInt,
    // Passed as a pointer to a caller-made copy (`byval`).
    ByVal,
    // Returned through a hidden pointer (`sret`).
    Sret,
};

// `t` is the *Mu* type of the parameter or return value; `is_return` selects the return rules.
// `target` is the LLVM triple (only the OS part matters).
AbiPass abi_class(const TypePtr& t, bool is_return, const std::string& target);

}  // namespace muc_detail

