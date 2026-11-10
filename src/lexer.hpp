// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 词法分析
#pragma once

#include <string>
#include <vector>

enum class TokKind { Ident, Int, Float, Char, Str, Kw, Op, Eof };

struct Token {
    TokKind kind = TokKind::Eof;
    std::string text;      // ident / keyword / op 文本
    long long ival = 0;    // Int / Char
    double fval = 0;       // Float
    bool f32 = false;      // Float 后缀 f
    std::string bytes;     // Str 的原始字节
    int line = 0;
    int col = 0;

    bool is_op(const char* s) const { return kind == TokKind::Op && text == s; }
    bool is_kw(const char* s) const { return kind == TokKind::Kw && text == s; }
    std::string describe() const;
};

// 失败时抛 MuError（带 filename/line/col）
std::vector<Token> tokenize(const std::string& src, const std::string& filename);
