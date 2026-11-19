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
    int isuffix = 0;       // Int suffix: 0 none, 1 `u` (uint), 2 `l` (long), 3 `ul` (ulong)
    // The magnitude is exactly 2^63 with a signed suffix: `ival` is 0 and the literal is only valid
    // directly after unary `-` (it spells the minimum of a signed 64-bit type).
    bool int_min_mag = false;
    std::string bytes;     // raw bytes of a Str
    int line = 0;
    int col = 0;
    bool bol = false;      // first token on its line; `#` directives require it

    bool is_op(const char* s) const { return kind == TokKind::Op && text == s; }
    bool is_kw(const char* s) const { return kind == TokKind::Kw && text == s; }
    std::string describe() const;
};

// Throws MuError on failure (with filename/line/col)
std::vector<Token> tokenize(const std::string& src, const std::string& filename);
