// mu-lan 编译器 —— 公共基础设施（错误、字符串工具）
#pragma once

#include <stdexcept>
#include <string>

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
        return loc + "error: " + m;
    }
};

struct ParserError : MuError {
    using MuError::MuError;
};

struct SemaError : MuError {
    using MuError::MuError;
};

[[noreturn]] inline void fail(const std::string& m, int line = 0, int col = 0,
                              const std::string& file = "") {
    throw MuError(m, line, col, file);
}
