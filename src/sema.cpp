// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Semantic analysis public interface (analyze / type_str); implementations are split by feature across sema_*.cpp.
#include "sema.hpp"

#include "sema_impl.hpp"


// ---------------------------------------------------------------- exports

std::string mangle(const std::string& unit, const std::string& name) {
    return "mu_" + unit + "_" + name;
}

Program analyze(std::vector<std::unique_ptr<Unit>> units, const std::string& entry) {
    muc_detail::Sema sema(std::move(units), entry);
    return sema.run();
}

std::string type_str(const TypePtr& t) { return t ? t->str() : std::string(); }


