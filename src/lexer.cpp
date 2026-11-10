// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 词法分析
//
// 与 Python 版的行为保持一致：关键字表、数字字面量（十进制 / 0x / 0b / 浮点 + f 后缀）、
// 字符与字符串字面量（含转义）、`//` 行注释与可嵌套的 `/* */` 块注释。
// 源码按 UTF-8 字节流处理（行 / 列按字节计），注释与字符串里的多字节字符原样透传。
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

// ---------------------------------------------------------------- 字符类

bool in_set(int ch, const char* set) {
    if (ch <= 0) return false;  // -1 = 文件结尾
    return std::strchr(set, ch) != nullptr;
}

bool is_digit(int ch) { return ch >= '0' && ch <= '9'; }

// SPEC 2.2：标识符为 [A-Za-z_][A-Za-z0-9_]*，第一阶段不允许非 ASCII 标识符。
bool ident_start(int ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

bool ident_cont(int ch) { return ident_start(ch) || is_digit(ch); }

// 关键字表（lexer.py 的 KEYWORDS）
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
        "override", "operator",  "packed",
    };
    return kws;
}

// 长的放前面，保证最长匹配（lexer.py 的 OPS3 / OPS2 / OPS1）
const char* const OPS3[] = {"<<=", ">>="};
const char* const OPS2[] = {"==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=", "&=",
                            "|=", "^=", "<<", ">>", "..", "++", "--"};
const char* const OPS1 = "+-*/%&|^!~<>=(){}[];,.?:";

// 近似 Python 的 repr(ch)，用于「非法字符」提示
std::string quote_char(int ch) {
    if (ch == '\'') return "\"'\"";
    if (ch >= 0x20 && ch < 0x7F) return std::string("'") + static_cast<char>(ch) + "'";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "'\\x%02x'", static_cast<unsigned>(ch) & 0xFFu);
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

// 与 Python 的 int(digits, base) 等价（溢出按 64 位回绕，正常源码用不到）
long long parse_digits(const std::string& digits, int base) {
    unsigned long long v = 0;
    const unsigned long long maxv = 9223372036854775807ULL;  // 与 Python 版一致：上限 2^63-1
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

// ---------------------------------------------------------------- 词法器

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
    // ------------------------------------------------------ 基础

    MuError err(const std::string& msg, int line = 0, int col = 0) const {
        return MuError(msg, line != 0 ? line : line_, col != 0 ? col : col_, filename_);
    }

    // 返回第 k 个字节（0..255），文件结尾返回 -1
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
                // UTF-8 续字节不计数，列号按「字符」而非字节推进（与 Python 版一致）
                ++col_;
            }
        }
        i_ = end;
        return out;
    }

    // ------------------------------------------------------ 主循环

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
                if (depth != 0) throw err("块注释没有闭合", line, col);
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

        // 不支持的运算符：给出友好提示（Python 版里这两条判断位于运算符匹配之后，
        // 实际不可达；这里按规范意图提前判断，使提示真正生效）。
        if (ch == '&' && peek(1) == '&') throw err("不支持 `&&`：逻辑与请写 `and`");
        if (ch == '|' && peek(1) == '|') throw err("不支持 `||`：逻辑或请写 `or`");

        // 运算符（最长匹配）
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

        if (ch == '\\') throw err("非法字符 `\\`");
        throw err("非法字符 " + quote_char(ch));
    }

    // ------------------------------------------------------ 数字

    long long int_digits(const std::string& raw, int base, int line, int col) {
        try {
            return parse_digits(raw, base);
        } catch (const std::overflow_error&) {
            throw err("整数字面量超出 64 位范围（最大 2^63-1）", line, col);
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
            if (raw.empty()) throw err("十六进制字面量缺少数字", line, col);
            tok.kind = TokKind::Int;
            tok.ival = int_digits(raw, 16, line, col);
            return tok;
        }

        if (peek() == '0' && in_set(peek(1), "bB")) {
            adv(2);
            std::size_t ds = i_;
            while (i_ < n_ && in_set(peek(), "01_")) adv();
            std::string raw = src_.substr(ds, i_ - ds);
            if (raw.empty()) throw err("二进制字面量缺少数字", line, col);
            tok.kind = TokKind::Int;
            tok.ival = int_digits(raw, 2, line, col);
            return tok;
        }

        std::size_t digits = i_;
        while (i_ < n_ && (is_digit(peek()) || peek() == '_')) adv();

        bool is_float = false;
        // 小数点（注意避开 `..` 区间）
        if (peek() == '.' && peek(1) != '.' && is_digit(peek(1))) {
            is_float = true;
            adv();
            while (i_ < n_ && (is_digit(peek()) || peek() == '_')) adv();
        }
        // 指数
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
            // 1f 视为 float
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

    // ------------------------------------------------------ 字符 / 字符串

    int escape() {
        int line = line_;
        int col = col_;
        int ch = peek();
        adv();
        switch (ch) {
            case 'n': return 0x0A;
            case 't': return 0x09;
            case 'r': return 0x0D;
            case '0': return 0x00;
            case '\\': return 0x5C;
            case '\'': return 0x27;
            case '"': return 0x22;
            case 'a': return 0x07;
            case 'b': return 0x08;
            case 'f': return 0x0C;
            case 'v': return 0x0B;
            default: break;
        }
        if (ch == 'x') {
            std::string h = adv(2);
            if (h.size() != 2 || h.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                throw err("`\\x` 后必须跟两位十六进制数字", line, col);
            }
            return static_cast<int>(std::strtol(h.c_str(), nullptr, 16));
        }
        if (ch == 'u') throw err("暂不支持 `\\u{...}` 转义");
        if (ch < 0) throw err("未知转义 `\\`", line, col);
        throw err("未知转义 `\\" + std::string(1, static_cast<char>(ch)) + "`", line, col);
    }

    Token char_lit(int line, int col) {
        Token tok;
        tok.line = line;
        tok.col = col;
        adv();  // 开头的 '
        if (peek() < 0) throw err("字符字面量没有闭合", line, col);
        long long v = 0;
        if (peek() == '\\') {
            adv();
            v = escape();
        } else {
            int ch = peek();
            adv();
            if (ch > 0x7F) throw err("字符字面量暂只支持 ASCII", line, col);
            v = ch;
        }
        if (peek() != '\'') throw err("字符字面量只能包含一个字符", line, col);
        adv();
        tok.kind = TokKind::Char;
        tok.ival = v;
        return tok;
    }

    Token string_lit(int line, int col) {
        Token tok;
        tok.line = line;
        tok.col = col;
        adv();  // 开头的 "
        std::string buf;
        for (;;) {
            if (i_ >= n_) throw err("字符串没有闭合", line, col);
            int ch = peek();
            if (ch == '"') {
                adv();
                break;
            }
            if (ch == '\n') throw err("字符串不能跨行", line, col);
            if (ch == '\\') {
                adv();
                buf.push_back(static_cast<char>(escape() & 0xFF));
                continue;
            }
            adv();
            buf.push_back(static_cast<char>(ch));  // UTF-8 字节原样保留
        }
        if (buf.find('\0') != std::string::npos) {
            throw err("字符串字面量不能包含 NUL 字节", line, col);
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

// ---------------------------------------------------------------- 对外接口

std::string Token::describe() const {
    switch (kind) {
        case TokKind::Eof:
            return "文件结尾";
        case TokKind::Str:
            return "字符串字面量";
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
