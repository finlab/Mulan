// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// C ABI aggregation rules (B2). Two families matter here:
//
//   Windows x64 (MS ABI)   a 1/2/4/8-byte aggregate is passed "as if it were an integer of the same
//                          size" (integer registers, never XMM — verified against gcc/mingw for
//                          `{float,float}`, `{double}` and `{int,int}`); every other size is passed
//                          as a pointer to a caller-made copy, and returned through a hidden pointer
//                          in RCX (`sret`). Over-aligned aggregates are also passed by reference.
//   SysV x64 (Linux/macOS) aggregates up to 16 bytes are classified into INTEGER/SSE eightbytes and
//                          passed in registers; larger ones are passed in memory (a pointer to a
//                          caller-made copy) and returned through a hidden pointer (RDI).
//
// The backend's own lowering of a first-class aggregate is *not* the C rule — it assigns one
// register per field — hence `CoerceInt`: the frontend reinterprets the bits through memory so the
// value really travels as the integer(s) the ABI asks for (one for MS x64, one per eightbyte on SysV).
//
// Not implemented: the SysV SSE class. C classifies each eightbyte as INTEGER or SSE, while the
// backend hands a float-bearing aggregate one register per *field* (two XMM registers for
// `{float, float}`, where C packs both into XMM0), so such a function is refused at codegen instead
// of being lowered wrongly. A packed SysV aggregate whose field straddles an eightbyte is class
// MEMORY and is also not distinguished. Recorded in agents/design-backlog.md.
//
// owns:      the C ABI aggregate classification (MS x64 vs SysV): which aggregates travel in registers, which are passed by reference
// not:       LLVM attribute application or signature building (codegen_abi.cpp)
// pass:      queried by codegen while building a signature
// concept:   c-abi
#include "abi.hpp"
#include "target.hpp"

namespace muc_detail {

AbiPass abi_class(const TypePtr& t, bool is_return, const std::string& target) {
    if (!t) return AbiPass::Direct;
    // Only aggregates have a C classification; scalars, pointers and handles stay in registers.
    if (t->kind != TypeKind::Struct) return AbiPass::Direct;

    const bool is_windows = is_windows_target(target);
    const size_t sz = type_size(t);
    const size_t al = type_align(t);

    if (is_windows) {
        const bool register_sized = sz == 1 || sz == 2 || sz == 4 || sz == 8;
        // MS x64: anything else (or over-aligned) travels by reference.
        if (register_sized && al <= 8) return AbiPass::CoerceInt;
        return is_return ? AbiPass::Sret : AbiPass::ByVal;
    }
    // SysV: an aggregate of at most two eightbytes travels in registers, classified per eightbyte
    // (INTEGER or SSE). Only the INTEGER case is implemented — a float-bearing aggregate is refused
    // in codegen before it gets here — so both eightbytes are packed through memory as integers.
    // A packed aggregate whose field straddles an eightbyte would be class MEMORY; that case is not
    // distinguished here (same as before) because it needs the per-field walk.
    if (sz <= 16 && al <= 16) return AbiPass::CoerceInt;
    return is_return ? AbiPass::Sret : AbiPass::ByVal;
}

}  // namespace muc_detail
