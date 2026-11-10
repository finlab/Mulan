// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// muc（Mulan 语言编译器）—— 公共基础设施（错误、字符串工具）
#pragma once

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mu_detail {

// 读取源文件第 line 行（1-based）；读不到返回空串
inline std::string read_source_line(const std::string& file, int line) {
    if (file.empty() || line <= 0) return {};
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::string s;
    for (int i = 1; i <= line; ++i) {
        if (!std::getline(in, s)) return {};
    }
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

// 渲染「源码行 + 插入符」片段。col 是 1-based 字节列（与词法位置一致）；
// tab 展开为 4 列，多字节 UTF-8 字符按 2 列（CJK 全角）估算显示宽度。
inline std::string render_snippet(const std::string& file, int line, int col) {
    const std::string src = read_source_line(file, line);
    if (src.empty()) return {};

    std::string shown;
    int visual = 0;
    int caret = -1;
    for (size_t i = 0; i < src.size();) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        int bytes = 1;
        int w = 1;
        if (c == '\t') {
            w = 4 - visual % 4;
        } else if (c < 0x80) {
            w = 1;
        } else if ((c & 0xE0) == 0xC0) {
            bytes = 2;
            w = 1;
        } else if ((c & 0xF0) == 0xE0) {
            bytes = 3;
            w = 2;
        } else if ((c & 0xF8) == 0xF0) {
            bytes = 4;
            w = 2;
        }
        if (static_cast<int>(i) + 1 == col) caret = visual;
        if (c == '\t') {
            shown.append(static_cast<size_t>(w), ' ');
        } else {
            shown.append(src, i, static_cast<size_t>(bytes));
        }
        visual += w;
        i += static_cast<size_t>(bytes);
    }
    if (caret < 0) caret = visual;  // col 超出行尾：插在末尾

    const std::string num = std::to_string(line);
    const std::string pad(num.size(), ' ');
    std::string out;
    out += pad + " |\n";
    out += num + " | " + shown + "\n";
    out += pad + " | " + std::string(static_cast<size_t>(caret), ' ') + "^";
    return out;
}

}  // namespace mu_detail

struct MuError : std::runtime_error {
    std::string msg;
    int line = 0;
    int col = 0;
    std::string filename;

    MuError(std::string m, int l = 0, int c = 0, std::string f = "")
        : std::runtime_error(format(m, l, c, f)),
          msg(std::move(m)), line(l), col(c), filename(std::move(f)) {}

    static std::string format(const std::string& m, int l, int c, const std::string& f) {
        std::string loc;
        if (!f.empty()) loc += f;
        if (l > 0) {
            loc += ":";
            loc += std::to_string(l);
            loc += ":";
            loc += std::to_string(c);
        }
        if (!loc.empty()) loc += ": ";
        std::string out = loc + "error: " + m;
        // 有位置信息时附带源码行与插入符（读不到源文件则省略）
        const std::string snippet = mu_detail::render_snippet(f, l, c);
        if (!snippet.empty()) out += "\n" + snippet;
        return out;
    }
};

struct ParserError : MuError {
    using MuError::MuError;
};

struct SemaError : MuError {
    using MuError::MuError;
};

// 一次编译收集到的多个错误（统一输出，而不是遇到第一个就停）
struct MuErrorList : std::runtime_error {
    std::vector<std::string> messages;
    explicit MuErrorList(std::vector<std::string> msgs)
        : std::runtime_error(msgs.empty() ? std::string("compile failed") : msgs.front()),
          messages(std::move(msgs)) {}
};

[[noreturn]] inline void fail(const std::string& m, int line = 0, int col = 0,
                              const std::string& file = "") {
    throw MuError(m, line, col, file);
}
