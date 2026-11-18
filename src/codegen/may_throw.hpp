// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Conservative module-local may-throw analysis.
//
// It only decides whether a call site needs the B2 pending-flag check.  The
// analysis never changes language semantics: anything whose body is not visible
// (extern functions, precompiled/static/dynamic libraries, unknown indirect
// targets) is treated as may-throw, so the result stays valid when the library
// story arrives.
#pragma once

#include <string>
#include <unordered_map>

#include "sema.hpp"

namespace muc_detail {

struct MayThrowInfo {
    // cname -> whether the function may return with `muc_eh_pending` set.
    std::unordered_map<std::string, bool> funcs;

    // Unknown / external symbols are conservatively may-throw.
    bool mayThrowCname(const std::string& cname) const;
    bool mayThrowFunc(const FuncDecl* f) const;
    // Only statically dispatched struct methods can be proven nothrow; class and
    // interface methods may dispatch to an unknown override.
    bool mayThrowMethod(const MethodSig* sig, const TypePtr& recvTy) const;
};

// The nearest parameterless constructor in `cls` or its base chain, or "" when
// there is none.  Used for implicit `new T` / omitted `super()` calls.
std::string default_ctor_key(const TypePtr& cls);

MayThrowInfo computeMayThrow(Program& prog);

}  // namespace muc_detail
