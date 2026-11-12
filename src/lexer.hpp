// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Lexing
#pragma once

#include <string>
#include <vector>

enum class TokKind { Ident, Int, Float, Char, Str, Kw, Op, Eof };

struct Token {
    TokKind kind = TokKind::Eof;
    std::string text;      // ident / keyword / op text
    long long ival = 0;    // Int / Char
    double fval = 0;       // Float
    bool f32 = false;      // Float suffix f
    std::string bytes;     // raw bytes of a Str
    int line = 0;
    int col = 0;

    bool is_op(const char* s) const { return kind == TokKind::Op && text == s; }
    bool is_kw(const char* s) const { return kind == TokKind::Kw && text == s; }
    std::string describe() const;
};

// Throws MuError on failure (with filename/line/col)
std::vector<Token> tokenize(const std::string& src, const std::string& filename);
