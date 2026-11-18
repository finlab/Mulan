// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Semantic analysis public interface (analyze / type_str); implementations are split by feature across sema_*.cpp.
//
// owns:      the sema public interface (analyze / type_str); the passes themselves live in sema_*.cpp
// not:       any checking pass
// pass:      entry wrapper
// concept:   sema-api
#include "sema.hpp"

#include "sema_impl.hpp"


// ---------------------------------------------------------------- exports

std::string mangle(const std::string& unit, const std::string& name) {
    return "mu$" + unit + "$" + name;
}

Program analyze(std::vector<std::unique_ptr<Unit>> units, const std::string& entry,
                bool single_thread, bool strict) {
    muc_detail::Sema sema(std::move(units), entry, single_thread, strict);
    return sema.run();
}

std::string type_str(const TypePtr& t) { return t ? t->str() : std::string(); }


