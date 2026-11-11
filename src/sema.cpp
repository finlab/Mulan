// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 语义分析对外接口（analyze / type_str）；实现按功能分散在 sema_*.cpp。
#include "sema.hpp"

#include "sema_impl.hpp"


// ---------------------------------------------------------------- 出口

std::string mangle(const std::string& unit, const std::string& name) {
    return "mu_" + unit + "_" + name;
}

Program analyze(std::vector<std::unique_ptr<Unit>> units, const std::string& entry) {
    muc_detail::Sema sema(std::move(units), entry);
    return sema.run();
}

std::string type_str(const TypePtr& t) { return t ? t->str() : std::string(); }


