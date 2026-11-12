// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Lexing
//
// Matches the Python version's behavior: keyword table, numeric literals (decimal / 0x / 0b / float + f suffix),
// character and string literals (with escapes), `//` line comments, and nestable `/* */` block comments.
// Source is processed as a UTF-8 byte stream (lines / columns counted in bytes); multibyte characters in comments and strings pass through unchanged.
#include "lexer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common.hpp"

namespace {

// ---------------------------------------------------------------- character classes

bool in_set(int ch, const char* set) {
    if (ch <= 0) return false;  // -1 = end of file
    return std::strchr(set, ch) != nullptr;
}

bool is_digit(int ch) { return ch >= '0' && ch <= '9'; }

// SPEC 2.2: identifiers are [A-Za-z_][A-Za-z0-9_]*; non-ASCII identifiers are not allowed in stage one.
bool ident_start(int ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

bool ident_cont(int ch) { return ident_start(ch) || is_digit(ch); }

// keyword table (KEYWORDS in lexer.py)
const std::unordered_set<std::string>& keywords() {
    static const std::unordered_set<std::string> kws = {
        "unit",     "use",      "public",  "private", "const",   "auto",    "struct",   "enum",
        "extern",   "if",       "else",    "elif",    "while",    "for",      "in",
        "switch",
        "ref",      "break",    "continue", "return", "true",     "false",    "null",
        "as",       "sizeof",   "alignof", "len",     "alloc",    "realloc",  "free",
        "void",     "bool",     "byte",    "char",     "short",
        "ushort",   "int",      "uint",    "long",    "ulong",    "float",    "double",
        "usize",    "isize",    "and",     "or",       "not",
        "class",    "interface", "new",    "del",     "weak",     "this",     "super",
        "override", "operator",  "packed",   "union",
        "try",      "catch",     "throw",    "finally",
    };
    return kws;
}

// longer operators first to guarantee longest match (OPS3 / OPS2 / OPS1 in lexer.py)
const char* const OPS3[] = {"<<=", ">>=", "...", "..="};
const char* const OPS2[] = {"==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=", "&=",
                            "|=", "^=", "<<", ">>", "..", "++", "--"};
const char* const OPS1 = "+-*/%&|^!~<>=(){}[];,.?:";

// used for the "invalid character" message; as in other diagnostics, source snippets are wrapped in backticks
std::string quote_char(int ch) {
    if (ch == '`') return "`\\``";
    if (ch == '\\') return "`\\\\`";
    if (ch >= 0x20 && ch < 0x7F) return std::string("`") + static_cast<char>(ch) + "`";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "`\\x%02x`", static_cast<unsigned>(ch) & 0xFFu);
    return buf;
}

std::string strip_underscores(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

// equivalent to Python's int(digits, base) (overflow wraps at 64 bits; normal source never reaches it)
long long parse_digits(const std::string& digits, int base) {
    unsigned long long v = 0;
    const unsigned long long maxv = 9223372036854775807ULL;  // same as the Python version: limit 2^63-1
    for (char c : digits) {
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0 || d >= base) continue;
        if (v > (maxv - static_cast<unsigned long long>(d)) / static_cast<unsigned long long>(base)) {
            throw std::overflow_error("integer literal out of range");
        }
        v = v * static_cast<unsigned long long>(base) + static_cast<unsigned long long>(d);
    }
    return static_cast<long long>(v);
}

double parse_double(const std::string& text) {
    return std::strtod(text.c_str(), nullptr);
}

// append a code point to the byte string as UTF-8 (used when `\u{...}` is written into a string literal)
void append_utf8(std::string& out, int cp) {
    unsigned v = static_cast<unsigned>(cp);
    if (v < 0x80) {
        out.push_back(static_cast<char>(v));
    } else if (v < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (v >> 6)));
        out.push_back(static_cast<char>(0x80u | (v & 0x3Fu)));
    } else if (v < 0x10000) {
        out.push_back(static_cast<char>(0xE0u | (v >> 12)));
        out.push_back(static_cast<char>(0x80u | ((v >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (v & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (v >> 18)));
        out.push_back(static_cast<char>(0x80u | ((v >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((v >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (v & 0x3Fu)));
    }
}

// ---------------------------------------------------------------- lexer

class Lexer {
public:
    Lexer(std::string src, std::string filename)
        : src_(std::move(src)), filename_(std::move(filename)), n_(src_.size()) {}

    std::vector<Token> tokens() {
        std::vector<Token> out;
        for (;;) {
            out.push_back(next());
            if (out.back().kind == TokKind::Eof) return out;
        }
    }

private:
    // ------------------------------------------------------ basics

    MuError err(const std::string& msg, int line = 0, int col = 0) const {
        return MuError(msg, line != 0 ? line : line_, col != 0 ? col : col_, filename_);
    }

    // return the k-th byte (0..255), or -1 at end of file
    int peek(std::size_t k = 0) const {
        std::size_t j = i_ + k;
        return j < n_ ? static_cast<int>(static_cast<unsigned char>(src_[j])) : -1;
    }

    std::string adv(std::size_t k = 1) {
        std::size_t end = i_ + k;
        if (end > n_) end = n_;
        std::string out = src_.substr(i_, end - i_);
        for (char ch : out) {
            unsigned char b = static_cast<unsigned char>(ch);
            if (ch == '\n') {
                ++line_;
                col_ = 1;
            } else if ((b & 0xC0u) != 0x80u) {
                // UTF-8 continuation bytes are not counted; columns advance by character rather than byte (same as the Python version)
                ++col_;
            }
        }
        i_ = end;
        return out;
    }

    // ------------------------------------------------------ main loop

    void skip_space_and_comments() {
        while (i_ < n_) {
            int ch = peek();
            if (in_set(ch, " \t\r\n")) {
                adv();
                continue;
            }
            if (ch == '/' && peek(1) == '/') {
                while (i_ < n_ && peek() != '\n') adv();
                continue;
            }
            if (ch == '/' && peek(1) == '*') {
                int line = line_;
                int col = col_;
                adv(2);
                int depth = 1;
                while (i_ < n_ && depth > 0) {
                    if (peek() == '/' && peek(1) == '*') {
                        ++depth;
                        adv(2);
                    } else if (peek() == '*' && peek(1) == '/') {
                        --depth;
                        adv(2);
                    } else {
                        adv();
                    }
                }
                if (depth != 0) throw err("unterminated block comment", line, col);
                continue;
            }
            break;
        }
    }

    Token next() {
        skip_space_and_comments();
        Token tok;
        tok.line = line_;
        tok.col = col_;
        if (i_ >= n_) {
            tok.kind = TokKind::Eof;
            return tok;
        }
        int ch = peek();

        if (ident_start(ch)) {
            std::size_t start = i_;
            while (i_ < n_ && ident_cont(peek())) adv();
            tok.text = src_.substr(start, i_ - start);
            tok.kind = keywords().count(tok.text) != 0 ? TokKind::Kw : TokKind::Ident;
            return tok;
        }

        if (is_digit(ch)) return number(tok.line, tok.col);

        if (ch == '\'') return char_lit(tok.line, tok.col);

        if (ch == '"') return string_lit(tok.line, tok.col);

        // unsupported operators: give a friendly hint (in the Python version these two checks sit after operator matching
        // and are effectively unreachable; here they run earlier as the spec intends, so the hint actually fires).
        if (ch == '&' && peek(1) == '&') throw err("`&&` is not supported: use `and` for logical and");
        if (ch == '|' && peek(1) == '|') throw err("`||` is not supported: use `or` for logical or");

        // operators (longest match)
        for (const char* op : OPS3) {
            if (src_.compare(i_, std::strlen(op), op) == 0) {
                adv(3);
                tok.kind = TokKind::Op;
                tok.text = op;
                return tok;
            }
        }
        for (const char* op : OPS2) {
            if (src_.compare(i_, std::strlen(op), op) == 0) {
                adv(2);
                tok.kind = TokKind::Op;
                tok.text = op;
                return tok;
            }
        }
        if (in_set(ch, OPS1)) {
            adv();
            tok.kind = TokKind::Op;
            tok.text = std::string(1, static_cast<char>(ch));
            return tok;
        }

        if (ch == '\\') throw err("invalid character `\\`");
        throw err("invalid character " + quote_char(ch));
    }

    // ------------------------------------------------------ numbers

    long long int_digits(const std::string& raw, int base, int line, int col) {
        try {
            return parse_digits(raw, base);
        } catch (const std::overflow_error&) {
            throw err("integer literal is too large (maximum 2^63-1)", line, col);
        }
    }

    Token number(int line, int col) {
        Token tok;
        tok.line = line;
        tok.col = col;

        if (peek() == '0' && in_set(peek(1), "xX")) {
            adv(2);
            std::size_t ds = i_;
            while (i_ < n_ && in_set(peek(), "0123456789abcdefABCDEF_")) adv();
            std::string raw = src_.substr(ds, i_ - ds);
            if (raw.empty()) throw err("hexadecimal literal has no digits", line, col);
            tok.kind = TokKind::Int;
            tok.ival = int_digits(raw, 16, line, col);
            return tok;
        }

        if (peek() == '0' && in_set(peek(1), "bB")) {
            adv(2);
            std::size_t ds = i_;
            while (i_ < n_ && in_set(peek(), "01_")) adv();
            std::string raw = src_.substr(ds, i_ - ds);
            if (raw.empty()) throw err("binary literal has no digits", line, col);
            tok.kind = TokKind::Int;
            tok.ival = int_digits(raw, 2, line, col);
            return tok;
        }

        std::size_t digits = i_;
        while (i_ < n_ && (is_digit(peek()) || peek() == '_')) adv();

        bool is_float = false;
        // decimal point (careful to avoid the `..` range)
        if (peek() == '.' && peek(1) != '.' && is_digit(peek(1))) {
            is_float = true;
            adv();
            while (i_ < n_ && (is_digit(peek()) || peek() == '_')) adv();
        }
        // exponent
        if (in_set(peek(), "eE")) {
            std::size_t save_i = i_;
            int save_line = line_;
            int save_col = col_;
            adv();
            if (in_set(peek(), "+-")) adv();
            if (is_digit(peek())) {
                is_float = true;
                while (i_ < n_ && (is_digit(peek()) || peek() == '_')) adv();
            } else {
                i_ = save_i;
                line_ = save_line;
                col_ = save_col;
            }
        }

        std::string text = strip_underscores(src_.substr(digits, i_ - digits));
        if (is_float) {
            if (in_set(peek(), "fF")) {
                adv();
                tok.f32 = true;
            }
            tok.kind = TokKind::Float;
            tok.fval = parse_double(text);
            return tok;
        }
        if (in_set(peek(), "fF")) {
            // treat 1f as a float
            adv();
            tok.kind = TokKind::Float;
            tok.f32 = true;
            tok.fval = parse_double(text);
            return tok;
        }
        tok.kind = TokKind::Int;
        tok.ival = int_digits(text, 10, line, col);
        return tok;
    }

    // ------------------------------------------------------ characters / strings

    // escape result: a plain escape is a single byte value; `\u{...}` is a Unicode code point (UTF-8 encoded when written into a string)
    struct Escape {
        int value;
        bool is_codepoint;
    };

    // the `{...}` part of `\u{...}` (`u` already consumed): a 1–6 digit hexadecimal code point
    int unicode_codepoint(int line, int col) {
        if (peek() != '{') throw err("`\\u` must be followed by `{...}`, e.g. `\\u{4E2D}`", line, col);
        adv();
        std::string hex;
        while (i_ < n_ && peek() != '}') {
            int c = peek();
            if (!(is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                throw err("`\\u{...}` expects hexadecimal digits", line, col);
            }
            hex.push_back(static_cast<char>(c));
            adv();
        }
        if (i_ >= n_) throw err("unterminated `\\u{...}` escape", line, col);
        adv();  // '}'
        if (hex.empty()) throw err("`\\u{...}` expects at least one hexadecimal digit", line, col);
        if (hex.size() > 6) throw err("`\\u{...}` accepts at most 6 hexadecimal digits", line, col);
        long long v = std::strtoll(hex.c_str(), nullptr, 16);
        char buf[80];
        if (v > 0x10FFFF) {
            std::snprintf(buf, sizeof(buf), "code point `U+%llX` is out of range (max U+10FFFF)", v);
            throw err(buf, line, col);
        }
        if (v >= 0xD800 && v <= 0xDFFF) {
            std::snprintf(buf, sizeof(buf), "surrogate code point `U+%llX` is not allowed", v);
            throw err(buf, line, col);
        }
        return static_cast<int>(v);
    }

    Escape escape() {
        int line = line_;
        int col = col_;
        int ch = peek();
        adv();
        switch (ch) {
            case 'n': return {0x0A, false};
            case 't': return {0x09, false};
            case 'r': return {0x0D, false};
            case '0': return {0x00, false};
            case '\\': return {0x5C, false};
            case '\'': return {0x27, false};
            case '"': return {0x22, false};
            case 'a': return {0x07, false};
            case 'b': return {0x08, false};
            case 'f': return {0x0C, false};
            case 'v': return {0x0B, false};
            default: break;
        }
        if (ch == 'x') {
            std::string h = adv(2);
            if (h.size() != 2 || h.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                throw err("`\\x` must be followed by two hexadecimal digits", line, col);
            }
            return {static_cast<int>(std::strtol(h.c_str(), nullptr, 16)), false};
        }
        if (ch == 'u') return {unicode_codepoint(line, col), true};
        if (ch < 0) throw err("invalid escape sequence `\\`", line, col);
        throw err("invalid escape sequence `\\" + std::string(1, static_cast<char>(ch)) + "`", line, col);
    }

    Token char_lit(int line, int col) {
        Token tok;
        tok.line = line;
        tok.col = col;
        adv();  // opening '
        if (peek() < 0) throw err("unterminated character literal", line, col);
        long long v = 0;
        bool is_codepoint = false;
        if (peek() == '\\') {
            adv();
            Escape e = escape();
            v = e.value;
            is_codepoint = e.is_codepoint;
        } else {
            int ch = peek();
            adv();
            if (ch > 0x7F) throw err("character literal only supports ASCII for now", line, col);
            v = ch;
        }
        if (is_codepoint && v > 0xFF) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "code point `U+%04llX` does not fit in a character literal (single byte); "
                          "use a string literal instead",
                          v);
            throw err(buf, line, col);
        }
        if (peek() != '\'') throw err("character literal can only contain one character", line, col);
        adv();
        tok.kind = TokKind::Char;
        tok.ival = v;
        return tok;
    }

    Token string_lit(int line, int col) {
        Token tok;
        tok.line = line;
        tok.col = col;
        adv();  // opening "
        std::string buf;
        for (;;) {
            if (i_ >= n_) throw err("unterminated string literal", line, col);
            int ch = peek();
            if (ch == '"') {
                adv();
                break;
            }
            if (ch == '\n') throw err("string literal cannot span multiple lines", line, col);
            if (ch == '\\') {
                adv();
                Escape e = escape();
                if (e.is_codepoint) append_utf8(buf, e.value);
                else buf.push_back(static_cast<char>(e.value & 0xFF));
                continue;
            }
            adv();
            buf.push_back(static_cast<char>(ch));  // keep UTF-8 bytes as-is
        }
        if (buf.find('\0') != std::string::npos) {
            throw err("string literal cannot contain NUL bytes", line, col);
        }
        tok.kind = TokKind::Str;
        tok.bytes = std::move(buf);
        return tok;
    }

    std::string src_;
    std::string filename_;
    std::size_t n_ = 0;
    std::size_t i_ = 0;
    int line_ = 1;
    int col_ = 1;
};

}  // namespace

// ---------------------------------------------------------------- public interface

std::string Token::describe() const {
    switch (kind) {
        case TokKind::Eof:
            return "end of file";
        case TokKind::Str:
            return "string literal";
        case TokKind::Float: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", fval);
            return std::string("`") + buf + "`";
        }
        case TokKind::Int:
        case TokKind::Char:
            return "`" + std::to_string(ival) + "`";
        case TokKind::Ident:
        case TokKind::Kw:
        case TokKind::Op:
        default:
            return "`" + text + "`";
    }
}

std::vector<Token> tokenize(const std::string& src, const std::string& filename) {
    return Lexer(src, filename).tokens();
}
