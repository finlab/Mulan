// C99 代码生成后端：把 sema.Program 变成一份自包含的 C 源码。
//
// 实现依据见 docs/codegen-spec.md。
// 本文件只读 AST/sema 注解，不做类型推导；`sizeof`/`alignof` 的目标类型 sema 未落
// 注解，这里用一个只读的 TypeSpec 解析辅助函数补上（见 Emitter::resolve_spec）。
//
// 生成的 C 大致结构：
//     /* 预置头 + mu_string + mu_fill_cbuf */
//     /* struct 前置声明（打破指针循环） */
//     /* 枚举 / struct / 数组包装 / 切片类型定义（依赖在前） */
//     /* 全局变量 */
//     /* 函数原型（含 extern） */
//     /* 函数定义 */

#include "codegen_c.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "common.hpp"
#include "types.hpp"

#ifndef MULANC_VERSION
#define MULANC_VERSION "0.0.0"
#endif

namespace {

// ---------------------------------------------------------------- 常量表

// mu-lan 基本类型 -> C 类型（docs/codegen-spec.md 第 3 节）
std::string prim_c(const std::string& name) {
    if (name == "bool") return "bool";
    if (name == "byte") return "int8_t";
    if (name == "ubyte") return "uint8_t";
    if (name == "char") return "uint8_t";
    if (name == "short") return "int16_t";
    if (name == "ushort") return "uint16_t";
    if (name == "int") return "int32_t";
    if (name == "uint") return "uint32_t";
    if (name == "long") return "int64_t";
    if (name == "ulong") return "uint64_t";
    if (name == "usize") return "size_t";
    if (name == "isize") return "ptrdiff_t";
    if (name == "float") return "float";
    if (name == "double") return "double";
    fail("内部错误：未知基本类型 `" + name + "`");
}

// 字节类元素：与 C 的 char 族互通（string 视图 / C ABI）
bool is_char_like_prim(const std::string& name) {
    return name == "char" || name == "byte" || name == "ubyte";
}

bool char_like(const TypePtr& t) {
    return t && t->kind == TypeKind::Prim && is_char_like_prim(t->name);
}

bool is_c_keyword(const std::string& s) {
    static const std::unordered_set<std::string> kw = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "restrict", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while", "_Bool", "bool", "true", "false",
        "NULL", "main",
    };
    return kw.count(s) != 0;
}

const char* HEADER = R"(/* 本文件由 mulanc（mu-lan 编译器）自动生成，请勿手工编辑。 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* mu-lan 内建 string：不可变 UTF-8 视图（NUL 结尾以便直接传给 C） */
typedef struct { const char* ptr; size_t len; } mu_string;

/* 把 string 拷进 char[N]：补 NUL，其余字节清零；放不下就运行期报错 */
static void mu_fill_cbuf(void* dst, size_t n, mu_string s)
{
    if (n == 0 || s.len >= n) {
        fprintf(stderr, "mu-lan: string (%zu bytes) does not fit in char[%zu]\n",
                s.len, n);
        abort();
    }
    memset(dst, 0, n);
    if (s.len != 0)
        memcpy(dst, s.ptr, s.len);
}

/* string 值比较：按字节；零值 string 与 "" 相等 */
static bool mu_str_eq(mu_string a, mu_string b)
{
    if (a.len != b.len) return false;
    if (a.len == 0) return true;
    return memcmp(a.ptr, b.ptr, a.len) == 0;
})";

// ---------------------------------------------------------------- 小工具

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

bool is_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// 把类型名里的非字母数字替换成 `_`，得到稳定的类型名片段。
std::string mangle_type(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += is_alnum(c) ? c : '_';
    return out;
}

std::string type_text(const TypePtr& t) {
    if (!t) fail("内部错误：缺少类型注解");
    return t->str();
}

std::string struct_name(const TypePtr& t) {
    if (!t) fail("内部错误：缺少类型注解");
    return t->unit.empty() ? "mu_" + t->name : "mu_" + t->unit + "_" + t->name;
}

std::string enum_name(const TypePtr& t) {
    return t->unit.empty() ? "mu_" + t->name : "mu_" + t->unit + "_" + t->name;
}

std::string array_name(const TypePtr& t) {
    return "mu_arr_" + mangle_type(type_text(t->elem)) + "_" +
           std::to_string(t->array_size);
}

std::string slice_name(const TypePtr& t) {
    return "mu_slice_" + mangle_type(type_text(t->elem));
}

// mtypes.Type -> C 类型文本（纯函数，不登记类型定义）。
std::string ctype_of(const TypePtr& t) {
    if (!t) fail("内部错误：缺少类型注解");
    switch (t->kind) {
        case TypeKind::Prim:
            return prim_c(t->name);
        case TypeKind::Void:
            return "void";
        case TypeKind::String:
            return "mu_string";
        case TypeKind::Null:
            return "void*";
        case TypeKind::Ptr:
            // readonly char* 直接映射成 const char*：与 libc 原型（如 puts）兼容。
            // 其余 readonly 指针映射成 const T*（ABI 不变，只是去掉写权限）。
            if (t->readonly && char_like(t->elem)) return "const char*";
            if (t->readonly) return "const " + ctype_of(t->elem) + "*";
            return ctype_of(t->elem) + "*";
        case TypeKind::Array:
            return array_name(t);
        case TypeKind::Slice:
            return slice_name(t);
        case TypeKind::Struct:
            return struct_name(t);
        case TypeKind::Enum:
            return enum_name(t);
        case TypeKind::Range:
            fail("内部错误：range 类型不能出现在 C 代码里");
        case TypeKind::Module:
            fail("内部错误：unit `" + t->unit + "` 不是值类型");
    }
    fail("内部错误：未知类型 `" + type_text(t) + "`");
}

// 浮点字面量文本：最短可回读表示；保证带小数点或指数，C 侧才是浮点常量。
std::string float_text(double v) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    std::string s(buf, res.ptr);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
}

// 把字节串渲染成 C 字符串字面量（必要时拼接多个字面量）。
//
// `\xNN` 转义在 C 里会贪婪吞掉后续十六进制字符，因此遇到 `\xNN` 后面紧跟
// 十六进制数字时先把当前字面量闭合，再用相邻字面量拼接表示。
std::string c_bytes(const std::string& b) {
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            parts.push_back("\"" + cur + "\"");
            cur.clear();
        }
    };

    const size_t n = b.size();
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = static_cast<unsigned char>(b[i]);
        if (ch == 0x22) {
            cur += "\\\"";
        } else if (ch == 0x5C) {
            cur += "\\\\";
        } else if (ch == 0x0A) {
            cur += "\\n";
        } else if (ch == 0x09) {
            cur += "\\t";
        } else if (ch == 0x0D) {
            cur += "\\r";
        } else if (ch >= 0x20 && ch <= 0x7E) {
            cur += static_cast<char>(ch);
        } else {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\x%02x", static_cast<unsigned>(ch));
            cur += esc;
            if (i + 1 < n) {
                char nxt = b[i + 1];
                bool hex = (nxt >= '0' && nxt <= '9') || (nxt >= 'a' && nxt <= 'f') ||
                           (nxt >= 'A' && nxt <= 'F');
                if (hex) flush();
            }
        }
    }
    flush();
    if (parts.empty()) parts.push_back("\"\"");
    return join(parts, " ");
}

// ---------------------------------------------------------------- 生成器

class Emitter {
  public:
    explicit Emitter(Program& prog) : prog_(prog) {}

    std::string run() {
        // 1) struct 前置声明：打破指针循环
        for (auto& u : prog_.units) {
            for (auto& d : u->decls) {
                if (auto sd = dynamic_cast<StructDecl*>(d.get())) {
                    std::string nm = struct_name(sd->ty);
                    forward_.push_back("typedef struct " + nm + " " + nm + ";");
                }
            }
        }

        // 2) 枚举（无依赖，先行）→ struct / 数组 / 切片（依赖在前）
        for (auto& u : prog_.units) {
            for (auto& d : u->decls) {
                if (auto ed = dynamic_cast<EnumDecl*>(d.get())) emit_enum(u->name, *ed);
            }
        }
        for (auto& u : prog_.units) {
            for (auto& d : u->decls) {
                if (auto sd = dynamic_cast<StructDecl*>(d.get())) ensure_type(sd->ty);
            }
        }

        // 3) 全局变量
        for (auto& u : prog_.units) {
            unit_ = u->name;
            for (auto& d : u->decls) {
                if (auto g = dynamic_cast<GlobalVar*>(d.get())) emit_global(*g);
            }
        }

        // 4) 函数原型（含 extern）：允许先调用后定义
        for (auto& u : prog_.units) {
            for (auto& d : u->decls) {
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    protos_.push_back(signature(*f) + ";");
                }
            }
        }

        // 5) 函数定义
        for (auto& u : prog_.units) {
            for (auto& d : u->decls) {
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    if (!f->is_extern) emit_func(u->name, *f);
                }
            }
        }

        // 组装
        std::vector<std::string> sections;
        sections.push_back(std::string("/* generated by mulanc ") + MULANC_VERSION + " */\n" +
                           HEADER);
        if (!forward_.empty()) {
            sections.push_back("/* ---- struct 前置声明 ---- */\n" + join(forward_, "\n"));
        }
        if (!type_defs_.empty()) {
            sections.push_back("/* ---- 类型定义 ---- */\n" + join(type_defs_, "\n\n"));
        }
        if (!globals_.empty()) {
            sections.push_back("/* ---- 全局变量 ---- */\n" + join(globals_, "\n"));
        }
        if (!protos_.empty()) {
            sections.push_back("/* ---- 函数原型 ---- */\n" + join(protos_, "\n"));
        }
        for (auto& f : funcs_) sections.push_back(f);
        return join(sections, "\n\n") + "\n";
    }

  private:
    // ---------------------------------------------------------- 类型登记
    std::string ct(const TypePtr& t) {
        if (!t) fail("内部错误：缺少类型注解");
        ensure_type(t);
        return ctype_of(t);
    }

    void ensure_type(const TypePtr& t) {
        if (!t) return;
        switch (t->kind) {
            case TypeKind::Prim:
            case TypeKind::Void:
            case TypeKind::String:
            case TypeKind::Enum:
            case TypeKind::Null:
            case TypeKind::Range:
            case TypeKind::Module:
                return;
            case TypeKind::Ptr:
                ensure_type(t->elem);
                return;
            case TypeKind::Array: {
                std::string nm = array_name(t);
                if (!types_done_.insert(nm).second) return;
                std::string elem = ct(t->elem);
                type_defs_.push_back("/* " + t->str() + " */\ntypedef struct { " + elem +
                                     " e[" + std::to_string(t->array_size) + "]; } " + nm +
                                     ";");
                return;
            }
            case TypeKind::Slice: {
                std::string nm = slice_name(t);
                if (!types_done_.insert(nm).second) return;
                std::string elem = ct(t->elem);
                type_defs_.push_back("/* " + t->str() +
                                     " */\ntypedef struct { " + elem + "* ptr; size_t len; } " +
                                     nm + ";");
                return;
            }
            case TypeKind::Struct: {
                std::string nm = struct_name(t);
                if (!types_done_.insert(nm).second) return;
                // 先占位：struct 只能通过指针自引用，前置声明已经覆盖这种情况
                std::vector<std::string> lines;
                for (auto& f : t->fields) lines.push_back("    " + ct(f.type) + " " + f.name + ";");
                std::string body = lines.empty() ? "    char _empty;" : join(lines, "\n");
                type_defs_.push_back("/* struct " + t->str() + " */\nstruct " + nm + " {\n" +
                                     body + "\n};");
                return;
            }
        }
        fail("内部错误：无法登记类型 `" + type_text(t) + "`");
    }

    void emit_enum(const std::string& unit, const EnumDecl& d) {
        std::string nm = "mu_" + unit + "_" + d.name;
        type_defs_.push_back("/* enum " + d.name + " */\ntypedef int32_t " + nm + ";");
        std::vector<std::string> items;
        long long value = 0;
        for (auto& m : d.members) {
            const std::string& member = m.first;
            const std::optional<long long>& explicit_value = m.second;
            if (explicit_value.has_value()) value = *explicit_value;
            items.push_back("    " + nm + "_" + member + " = " + std::to_string(value) + ",");
            value += 1;
        }
        if (!items.empty()) {
            type_defs_.push_back("enum {\n" + join(items, "\n") + "\n};");
        }
    }

    // ---------------------------------------------------------- 名字
    std::string cname_of(LocalVar* lv) {
        if (!lv) fail("内部错误：缺少局部变量符号");
        if (!lv->cname.empty()) return lv->cname;
        ++n_;
        std::string base;
        const std::string& raw = lv->name.empty() ? std::string("v") : lv->name;
        for (char c : raw) base += is_alnum(c) || c == '_' ? c : '_';
        if (base.empty() || (base[0] >= '0' && base[0] <= '9')) base = "v" + base;
        if (is_c_keyword(base)) base = "mu_" + base;
        lv->cname = base + "_" + std::to_string(n_);
        return lv->cname;
    }

    std::string new_temp(const std::string& prefix = "mu_t") {
        ++n_;
        return prefix + std::to_string(n_);
    }

    // ---------------------------------------------------------- 输出缓冲
    void w(const std::string& line = "") {
        if (line.empty()) {
            buf_.push_back("");
            return;
        }
        std::string pad;
        for (int i = 0; i < indent_; ++i) pad += "    ";
        buf_.push_back(pad + line);
    }

    // ---------------------------------------------------------- 全局变量
    void emit_global(GlobalVar& g) {
        std::string ct_g = ct(g.rty);
        if (g.is_extern) {
            protos_.push_back("extern " + ct_g + " " + g.cname + ";");
            return;
        }
        std::string quals;
        if (g.is_static) quals += "static ";
        if (g.is_const) quals += "const ";
        std::string tail;
        if (!g.init) {
            tail = g.zero ? " = {0}" : "";
        } else if (auto ie = dynamic_cast<const InitExpr*>(g.init.get())) {
            Expr* e = ie->expr.get();
            if (g.rty->kind == TypeKind::String && dynamic_cast<const StringLit*>(e)) {
                const auto* sl = static_cast<const StringLit*>(e);
                tail = " = { " + c_bytes(sl->value) + ", " + std::to_string(sl->value.size()) +
                       " }";
            } else if (g.rty->kind == TypeKind::Array && char_like(g.rty->elem) &&
                       dynamic_cast<const StringLit*>(e)) {
                const auto* sl = static_cast<const StringLit*>(e);
                tail = " = { " + c_bytes(sl->value) + " }";
            } else {
                tail = " = " + emit_expr(e, g.rty);
            }
        } else if (auto ia = dynamic_cast<const InitAggregate*>(g.init.get())) {
            tail = " = " + aggregate_init(*ia, g.rty);
        } else {
            fail("内部错误：未知初始化器");
        }
        globals_.push_back(quals + ct_g + " " + g.cname + tail + ";");
    }

    // ---------------------------------------------------------- 函数
    std::string signature(FuncDecl& f) {
        std::vector<std::string> params;
        for (auto& pt : f.pty) params.push_back(ct(pt));
        std::string plist = params.empty() ? "void" : join(params, ", ");
        return ct(f.rty) + " " + f.cname + "(" + plist + ")";
    }

    void emit_func(const std::string& unit, FuncDecl& f) {
        unit_ = unit;
        func_ = &f;
        n_ = 0;
        buf_.clear();
        indent_ = 1;

        std::vector<std::string> params;
        size_t np = std::min(f.pty.size(), f.param_locals.size());
        for (size_t i = 0; i < np; ++i) {
            params.push_back(ct(f.pty[i]) + " " + cname_of(f.param_locals[i].get()));
        }
        std::string plist = params.empty() ? "void" : join(params, ", ");
        std::string head = ct(f.rty) + " " + f.cname + "(" + plist + ")";

        buf_.push_back(head + "\n{");
        if (f.body) {
            for (auto& st : f.body->stmts) stmt(st.get());
        }
        buf_.push_back("}");
        funcs_.push_back(join(buf_, "\n"));
        func_ = nullptr;
    }

    // ---------------------------------------------------------- 语句
    void stmt(Stmt* s) {
        if (!s) return;
        if (auto b = dynamic_cast<Block*>(s)) {
            w("{");
            ++indent_;
            for (auto& st : b->stmts) stmt(st.get());
            --indent_;
            w("}");
        } else if (auto lv = dynamic_cast<LocalVar*>(s)) {
            emit_local(*lv);
        } else if (auto es = dynamic_cast<const ExprStmt*>(s)) {
            w(emit_expr(es->expr.get()) + ";");
        } else if (auto as = dynamic_cast<const AssignStmt*>(s)) {
            TypePtr tt = as->target ? as->target->ty : nullptr;
            std::string val = emit_expr(as->value.get(), tt);
            w(emit_expr(as->target.get()) + " " + as->op + " " + val + ";");
        } else if (auto is = dynamic_cast<IfStmt*>(s)) {
            emit_if(*is);
        } else if (auto ws = dynamic_cast<const WhileStmt*>(s)) {
            std::string cond = emit_expr(ws->cond.get(), t_bool());
            w("while (" + cond + ") {");
            ++indent_;
            if (ws->body) {
                for (auto& st : ws->body->stmts) stmt(st.get());
            }
            --indent_;
            w("}");
        } else if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            emit_for(*fs);
        } else if (dynamic_cast<const BreakStmt*>(s)) {
            w("break;");
        } else if (dynamic_cast<const ContinueStmt*>(s)) {
            w("continue;");
        } else if (auto rs = dynamic_cast<const ReturnStmt*>(s)) {
            if (!rs->value) {
                w("return;");
            } else {
                TypePtr rt = func_ ? func_->rty : nullptr;
                w("return " + emit_expr(rs->value.get(), rt) + ";");
            }
        } else {
            fail("内部错误：未知语句");
        }
    }

    void emit_if(IfStmt& s) {
        // 收集 if / else if 链，最后可能跟一个 else 块
        std::vector<IfStmt*> chain;
        Stmt* cur = &s;
        Block* final_block = nullptr;
        while (auto is = dynamic_cast<IfStmt*>(cur)) {
            chain.push_back(is);
            Stmt* nxt = is->els.get();
            if (auto b = dynamic_cast<Block*>(nxt)) {
                final_block = b;
                cur = nullptr;
            } else {
                cur = nxt;
            }
        }

        for (size_t i = 0; i < chain.size(); ++i) {
            IfStmt* node = chain[i];
            std::string cond = emit_expr(node->cond.get(), t_bool());
            if (i == 0) {
                w("if (" + cond + ") {");
            } else {
                w("} else if (" + cond + ") {");
            }
            ++indent_;
            if (node->then) {
                for (auto& st : node->then->stmts) stmt(st.get());
            }
            --indent_;
        }
        if (final_block) {
            w("} else {");
            ++indent_;
            for (auto& st : final_block->stmts) stmt(st.get());
            --indent_;
        }
        w("}");
    }

    void emit_local(LocalVar& lv) {
        TypePtr t = lv.rty;
        std::string cn = cname_of(&lv);
        std::string decl = (lv.is_const ? std::string("const ") : std::string()) + ct(t) + " " + cn;
        const Init* init = lv.init.get();

        if (!init) {
            w(lv.zero ? decl + " = {0};" : decl + ";");
            return;
        }
        if (lv.zero) {
            w(decl + " = {0};");
            return;
        }

        if (auto ie = dynamic_cast<const InitExpr*>(init)) {
            Expr* e = ie->expr.get();
            if (t->kind == TypeKind::Array && char_like(t->elem) &&
                dynamic_cast<const StringLit*>(e)) {
                const auto* sl = static_cast<const StringLit*>(e);
                if (lv.is_const) {
                    std::vector<std::string> vals;
                    size_t n = std::min(sl->value.size(), t->array_size);
                    for (size_t i = 0; i < n; ++i) {
                        vals.push_back(std::to_string(
                            static_cast<unsigned char>(sl->value[i])));
                    }
                    w(decl + " = {{ " + join(vals, ", ") + " }};");
                } else {
                    // 字节拷贝 + NUL：先声明再填，容量检查由 mu_fill_cbuf 负责
                    w(decl + ";");
                    w("mu_fill_cbuf(" + cn + ".e, " + std::to_string(t->array_size) + "U, " +
                      emit_expr(e, nullptr) + ");");
                }
                return;
            }
            w(decl + " = " + emit_expr(e, t) + ";");
            return;
        }

        if (auto ia = dynamic_cast<const InitAggregate*>(init)) {
            w(decl + " = " + aggregate_init(*ia, t) + ";");
            return;
        }

        fail("内部错误：未知初始化器");
    }

    void emit_for(ForInStmt& s) {
        if (s.loop_kind == "range") {
            auto re = dynamic_cast<const RangeExpr*>(s.seq.get());
            if (!re) fail("内部错误：区间循环缺少区间表达式", s.line, s.col);
            TypePtr vt = s.var_ty;
            std::string cvt = ct(vt);
            std::string cn = cname_of(s.lvar.get());
            std::string lo = loop_bound(re->lo.get(), vt);
            std::string hi = loop_bound(re->hi.get(), vt);
            // 区间长度在循环开始时确定：端点不是简单值先求值到临时量（避免每次迭代重算）
            Expr* hi_expr = re->hi.get();
            bool simple = dynamic_cast<const IntLit*>(hi_expr) ||
                          dynamic_cast<const FloatLit*>(hi_expr) ||
                          dynamic_cast<const Name*>(hi_expr);
            if (!simple) {
                std::string tmp = new_temp("mu_t");
                w(cvt + " " + tmp + " = " + hi + ";");
                hi = tmp;
            }
            w("for (" + cvt + " " + cn + " = " + lo + "; " + cn + " < " + hi + "; " + cn +
              " += 1) {");
            ++indent_;
            if (s.body) {
                for (auto& st : s.body->stmts) stmt(st.get());
            }
            --indent_;
            w("}");
            return;
        }

        TypePtr sty = s.seq_ty;
        if (!sty) fail("内部错误：for-in 缺少序列类型", s.line, s.col);
        std::string seq = materialize(s.seq.get(), sty);
        TypePtr et = s.elem_ty;
        std::string ect = ct(et);
        std::string cn = cname_of(s.lvar.get());
        std::string idx = new_temp("mu_i");

        std::string length;
        std::string elem;
        if (sty->kind == TypeKind::Array) {
            length = std::to_string(sty->array_size);
            elem = "(" + seq + ").e[" + idx + "]";
        } else if (sty->kind == TypeKind::Slice) {
            length = "(" + seq + ").len";
            elem = "(" + seq + ").ptr[" + idx + "]";
        } else if (sty->kind == TypeKind::String) {
            length = "(" + seq + ").len";
            elem = "((uint8_t)(" + seq + ").ptr[" + idx + "])";
        } else {
            fail("内部错误：for-in 不能遍历 `" + sty->str() + "`", s.line, s.col);
        }

        w("for (size_t " + idx + " = 0; " + idx + " < " + length + "; " + idx + " += 1) {");
        ++indent_;
        if (s.by_ref) {
            w(ect + "* " + cn + " = &" + elem + ";");
        } else {
            w(ect + " " + cn + " = " + elem + ";");
        }
        if (s.body) {
            for (auto& st : s.body->stmts) stmt(st.get());
        }
        --indent_;
        w("}");
    }

    // 区间的端点：字面量直接写，其余显式转换到循环变量类型。
    std::string loop_bound(Expr* e, const TypePtr& t) {
        std::string s = emit_expr(e, nullptr);
        if (e && e->ty && same_type(e->ty, t)) return s;
        if (dynamic_cast<const IntLit*>(e) || dynamic_cast<const FloatLit*>(e)) return s;
        return "((" + ct(t) + ")(" + s + "))";
    }

    // 序列表达式只求值一次：简单左值（变量或成员链）直接内联，其余先物化。
    static bool simple_seq(Expr* e) {
        if (dynamic_cast<const Name*>(e)) return true;
        if (auto m = dynamic_cast<const Member*>(e)) return simple_seq(m->obj.get());
        return false;
    }

    std::string materialize(Expr* e, const TypePtr& t) {
        if (simple_seq(e)) return emit_expr(e, nullptr);
        std::string tmp = new_temp("mu_t");
        w(ct(t) + " " + tmp + " = " + emit_expr(e, nullptr) + ";");
        return tmp;
    }

    // ---------------------------------------------------------- 初始化器
    static bool zeroable(const TypePtr& t) {
        return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Array ||
                     t->kind == TypeKind::Slice || t->kind == TypeKind::String);
    }

    // 单个初始化位置（声明 / 聚合成员 / 嵌套花括号）
    std::string init_value(const TypePtr& t, Init* init) {
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            Expr* e = ie->expr.get();
            if (!e) fail("内部错误：初始化缺少表达式", init->line, init->col);
            if (auto il = dynamic_cast<IntLit*>(e)) {
                if (il->value == 0 && zeroable(t)) return "{0}";
            }
            if (t->kind == TypeKind::Array && char_like(t->elem) &&
                dynamic_cast<StringLit*>(e)) {
                return "{" + c_bytes(static_cast<StringLit*>(e)->value) + "}";
            }
            return emit_expr(e, t);
        }
        if (auto ia = dynamic_cast<InitAggregate*>(init)) return aggregate_init(*ia, t);
        fail("内部错误：未知初始化器", init->line, init->col);
    }

    std::string aggregate_init(const InitAggregate& init, const TypePtr& t) {
        if (t->kind == TypeKind::Struct) {
            std::vector<std::pair<int, Init*>> slots;
            bool any_named = false;
            for (auto& it : init.items) {
                if (!it.first.empty()) any_named = true;
            }
            if (any_named) {
                for (auto& it : init.items) {
                    int idx = t->field_index(it.first);
                    if (idx < 0) {
                        fail("结构体 `" + t->str() + "` 没有字段 `" + it.first + "`", init.line,
                             init.col);
                    }
                    slots.emplace_back(idx, it.second.get());
                }
            } else {
                for (size_t i = 0; i < init.items.size(); ++i) {
                    slots.emplace_back(static_cast<int>(i), init.items[i].second.get());
                }
            }
            std::vector<std::string> parts;
            for (size_t i = 0; i < t->fields.size(); ++i) {
                Init* found = nullptr;
                for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
                    if (it->first == static_cast<int>(i)) {
                        found = it->second;
                        break;
                    }
                }
                parts.push_back(found ? init_value(t->fields[i].type, found)
                                      : std::string("{0}"));
            }
            return "{" + join(parts, ", ") + "}";
        }

        if (t->kind == TypeKind::Array) {
            std::vector<std::string> parts;
            for (auto& it : init.items) {
                if (!it.first.empty()) {
                    fail("内部错误：数组初始化出现字段名", init.line, init.col);
                }
                parts.push_back(init_value(t->elem, it.second.get()));
            }
            return "{{" + (parts.empty() ? std::string("0") : join(parts, ", ")) + "}}";
        }

        if (t->kind == TypeKind::Slice) {
            std::vector<std::string> parts;
            for (auto& it : init.items) parts.push_back(init_value(t->elem, it.second.get()));
            std::string inner = parts.empty() ? std::string("0") : join(parts, ", ");
            std::string arr = "(" + ct(t->elem) + "[]){" + inner + "}";
            return "{ " + arr + ", " + std::to_string(parts.size()) + " }";
        }

        fail("类型 `" + t->str() + "` 不能用花括号初始化", init.line, init.col);
    }

    // ---------------------------------------------------------- 表达式
    std::string emit_expr(Expr* e, const TypePtr& expected = nullptr) {
        if (!e) fail("内部错误：缺少表达式");
        return convert(expr(e), e->ty, expected);
    }

    std::string convert(const std::string& s, const TypePtr& src, const TypePtr& expected) {
        if (!expected || !src || same_type(src, expected)) return s;
        // string -> readonly char*
        if (expected->kind == TypeKind::Ptr && src->kind == TypeKind::String) {
            return "(" + s + ").ptr";
        }
        if (expected->kind == TypeKind::Ptr && src->kind == TypeKind::Ptr) {
            if (ct(expected) != ct(src)) return "((" + ct(expected) + ")" + s + ")";
            return s;
        }
        // T[N] -> T[]
        if (expected->kind == TypeKind::Slice) {
            if (src->kind == TypeKind::Array && same_type(src->elem, expected->elem)) {
                return "((" + ct(expected) + "){" + "(" + s + ").e, " +
                       std::to_string(src->array_size) + "})";
            }
            if (src->kind == TypeKind::String) {
                std::string elem = ct(expected->elem);
                if (elem == "uint8_t") {
                    return "((" + ct(expected) + "){((uint8_t*)(" + s + ").ptr), (" + s +
                           ").len})";
                }
                return "((" + ct(expected) + "){" + "(" + s + ").ptr, (" + s + ").len})";
            }
        }
        return s;
    }

    // 成员 / 下标访问的基址：后置表达式不用加括号。
    std::string at(Expr* e) {
        std::string s = emit_expr(e, nullptr);
        if (dynamic_cast<const Name*>(e) || dynamic_cast<const Member*>(e) ||
            dynamic_cast<const Index*>(e) || dynamic_cast<const Call*>(e)) {
            return s;
        }
        return "(" + s + ")";
    }

    std::string expr(Expr* e) {
        if (!e) fail("内部错误：缺少表达式");

        if (auto v = dynamic_cast<const IntLit*>(e)) {
            TypePtr t = v->ty;
            std::string body = std::to_string(v->value);
            if (t && t->kind == TypeKind::Prim) {
                if (t->is_float()) return float_text(static_cast<double>(v->value));
                if (t->is_integer()) {
                    if (t->bits() == 64 && !t->is_signed()) {
                        body += "ULL";
                    } else if (t->bits() == 64) {
                        body += "LL";
                    }
                    if (t->name != "int") {
                        return "((" + prim_c(t->name) + ")" + body + ")";
                    }
                }
            }
            return body;
        }

        if (auto v = dynamic_cast<const FloatLit*>(e)) {
            std::string text = float_text(v->value);
            if (v->ty && v->ty->kind == TypeKind::Prim && v->ty->name == "float") {
                return "((float)" + text + ")";
            }
            return text;
        }

        if (auto v = dynamic_cast<const CharLit*>(e)) {
            return "((uint8_t)" + std::to_string(v->value) + ")";
        }

        if (auto v = dynamic_cast<const StringLit*>(e)) {
            return "(mu_string){" + c_bytes(v->value) + ", " + std::to_string(v->value.size()) +
                   "}";
        }

        if (auto v = dynamic_cast<const BoolLit*>(e)) {
            return v->value ? "true" : "false";
        }

        if (dynamic_cast<const NullLit*>(e)) return "NULL";

        if (auto v = dynamic_cast<Name*>(e)) return name(v);

        if (auto v = dynamic_cast<const Member*>(e)) {
            if (v->kind == "global") {
                auto g = std::get_if<GlobalVar*>(&v->sym);
                if (!g || !*g) fail("内部错误：成员 `" + v->name + "` 不是全局变量", e->line, e->col);
                return (*g)->cname;
            }
            return at(v->obj.get()) + "." + v->name;
        }

        if (auto v = dynamic_cast<const Index*>(e)) {
            TypePtr bt = v->base ? v->base->ty : nullptr;
            if (!bt) fail("内部错误：下标访问缺少基址类型", e->line, e->col);
            std::string base = at(v->base.get());
            std::string idx = emit_expr(v->index.get(), nullptr);
            if (bt->kind == TypeKind::Array) return base + ".e[" + idx + "]";
            if (bt->kind == TypeKind::Slice) return base + ".ptr[" + idx + "]";
            if (bt->kind == TypeKind::Ptr) return base + "[" + idx + "]";
            if (bt->kind == TypeKind::String) return "((uint8_t)" + base + ".ptr[" + idx + "])";
            fail("类型 `" + bt->str() + "` 不能下标访问", e->line, e->col);
        }

        if (auto v = dynamic_cast<const Unary*>(e)) {
            std::string inner = emit_expr(v->operand.get(), nullptr);
            if (v->op == "!") return "(~" + inner + ")";
            return "(" + v->op + inner + ")";
        }

        if (auto v = dynamic_cast<const NotExpr*>(e)) {
            // not -> C 逻辑非
            return "(!" + emit_expr(v->operand.get(), nullptr) + ")";
        }

        if (auto v = dynamic_cast<const Binary*>(e)) {
            std::string op = v->op;
            if (op == "and") op = "&&";
            else if (op == "or") op = "||";
            TypePtr lt = v->left ? v->left->ty : nullptr;
            TypePtr rt = v->right ? v->right->ty : nullptr;
            if ((op == "==" || op == "!=") && lt && rt &&
                lt->kind == TypeKind::String && rt->kind == TypeKind::String) {
                std::string call = "mu_str_eq(" + emit_expr(v->left.get(), nullptr) + ", " +
                                   emit_expr(v->right.get(), nullptr) + ")";
                return op == "==" ? call : "(!" + call + ")";
            }
            std::string left = emit_expr(v->left.get(), nullptr);
            std::string right = emit_expr(v->right.get(), nullptr);
            return "(" + left + " " + op + " " + right + ")";
        }

        if (auto v = dynamic_cast<Cast*>(e)) return emit_cast(v);

        if (auto v = dynamic_cast<Call*>(e)) return emit_call(v);

        if (auto v = dynamic_cast<const SizeofExpr*>(e)) {
            TypePtr t = resolve_spec(v->spec.get());
            if (v->which == "alignof") {
                return "((size_t)offsetof(struct { char c; " + ct(t) + " x; }, x))";
            }
            return "((size_t)sizeof(" + ct(t) + "))";
        }

        if (auto v = dynamic_cast<const Len*>(e)) {
            TypePtr st = v->seq ? v->seq->ty : nullptr;
            if (st && st->kind == TypeKind::Array) {
                return "((size_t)" + std::to_string(st->array_size) + ")";
            }
            return "((size_t)(" + emit_expr(v->seq.get(), nullptr) + ").len)";
        }

        fail("内部错误：未知表达式", e->line, e->col);
    }

    std::string name(Name* e) {
        if (e->kind == "local") {
            auto lv = std::get_if<LocalVar*>(&e->sym);
            if (!lv || !*lv) fail("内部错误：局部名字 `" + e->ident + "` 缺少符号", e->line, e->col);
            std::string cn = cname_of(*lv);
            return (*lv)->by_ref ? "(*" + cn + ")" : cn;
        }
        if (e->kind == "global") {
            auto g = std::get_if<GlobalVar*>(&e->sym);
            if (!g || !*g) fail("内部错误：全局名字 `" + e->ident + "` 缺少符号", e->line, e->col);
            return (*g)->cname;
        }
        if (e->kind == "enum_const") {
            auto ec = std::get_if<EnumConst*>(&e->sym);
            if (!ec || !*ec) fail("内部错误：枚举常量 `" + e->ident + "` 缺少符号", e->line, e->col);
            return "mu_" + (*ec)->unit + "_" + (*ec)->enum_name + "_" + (*ec)->member;
        }
        fail("内部错误：名字 `" + e->ident + "` 不能作为值使用（kind=`" + e->kind + "`）", e->line,
             e->col);
    }

    std::string emit_cast(Cast* e) {
        TypePtr src = e->expr ? e->expr->ty : nullptr;
        TypePtr dst = e->ty;
        if (!dst) fail("内部错误：强转缺少目标类型", e->line, e->col);
        if (dst->kind == TypeKind::Ptr && src && src->kind == TypeKind::String) {
            return "(" + emit_expr(e->expr.get(), nullptr) + ").ptr";
        }
        std::string inner = emit_expr(e->expr.get(), nullptr);
        return "((" + ct(dst) + ")(" + inner + "))";
    }

    std::string emit_call(Call* e) {
        if (e->callee_kind == "builtin") {
            std::string cname;
            if (e->builtin == "alloc") cname = "malloc";
            else if (e->builtin == "realloc") cname = "realloc";
            else if (e->builtin == "free") cname = "free";
            else fail("内部错误：未知内建函数 `" + e->builtin + "`", e->line, e->col);
            std::vector<std::string> args;
            for (auto& a : e->args) args.push_back(emit_expr(a.get(), nullptr));
            return cname + "(" + join(args, ", ") + ")";
        }

        if (e->callee_kind == "ctor") {
            TypePtr ty = e->ctor;
            if (!ty) fail("内部错误：结构体构造缺少类型", e->line, e->col);
            if (e->args.empty() && e->named.empty()) {
                return "((" + ct(ty) + "){0})";
            }
            std::vector<std::pair<int, Expr*>> slots;
            if (!e->named.empty()) {
                for (auto& it : e->named) {
                    int idx = ty->field_index(it.first);
                    if (idx < 0) {
                        fail("结构体 `" + ty->str() + "` 没有字段 `" + it.first + "`", e->line,
                             e->col);
                    }
                    slots.emplace_back(idx, it.second.get());
                }
            } else {
                for (size_t i = 0; i < e->args.size(); ++i) {
                    int idx = i < e->field_order.size() ? e->field_order[i]
                                                        : static_cast<int>(i);
                    slots.emplace_back(idx, e->args[i].get());
                }
            }
            std::vector<std::string> parts;
            for (size_t i = 0; i < ty->fields.size(); ++i) {
                Expr* found = nullptr;
                for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
                    if (it->first == static_cast<int>(i)) {
                        found = it->second;
                        break;
                    }
                }
                parts.push_back(found ? emit_expr(found, ty->fields[i].type)
                                      : std::string("0"));
            }
            return "((" + ct(ty) + "){" + join(parts, ", ") + "})";
        }

        if (e->callee_kind == "func") {
            FuncDecl* f = e->func;
            if (!f) fail("内部错误：函数调用缺少符号", e->line, e->col);
            std::vector<std::string> args;
            size_t n = std::min(e->args.size(), f->pty.size());
            for (size_t i = 0; i < n; ++i) {
                args.push_back(emit_expr(e->args[i].get(), f->pty[i]));
            }
            for (size_t i = n; i < e->args.size(); ++i) {
                args.push_back(emit_expr(e->args[i].get(), nullptr));
            }
            return f->cname + "(" + join(args, ", ") + ")";
        }

        fail("内部错误：无法识别的调用", e->line, e->col);
    }

    // ---------------------------------------------------------- TypeSpec
    // 只读地把 TypeSpec 解析成 Type。
    //
    // sema 已经解析过同一个 spec，但没有把结果留在 Sizeof 节点上；
    // 这里按 sema 的规则复现一遍（不改变任何语言语义）。
    TypePtr resolve_spec(const TypeSpec* spec) {
        if (!spec) fail("内部错误：sizeof/alignof 缺少类型");
        switch (spec->kind) {
            case TypeSpec::K::Named: {
                const std::string& nm = spec->name;
                if (nm == "void") return t_void();
                if (nm == "string") return t_string();
                if (is_prim_name(nm)) return Type::make_prim(nm);
                std::string unit, tname;
                auto dot = nm.find('.');
                if (dot != std::string::npos) {
                    unit = nm.substr(0, dot);
                    tname = nm.substr(dot + 1);
                } else {
                    unit = unit_;
                    tname = nm;
                }
                auto it = prog_.infos.find(unit);
                if (it == prog_.infos.end()) {
                    fail("未导入的 unit `" + unit + "`", spec->line, spec->col);
                }
                UnitInfo& info = it->second;
                auto si = info.structs.find(tname);
                if (si != info.structs.end() && si->second) return si->second->ty;
                auto ei = info.enums.find(tname);
                if (ei != info.enums.end() && ei->second) return ei->second->ty;
                fail("unit `" + unit + "` 中没有类型 `" + tname + "`", spec->line, spec->col);
            }
            case TypeSpec::K::Ptr:
                return Type::make_ptr(resolve_spec(spec->elem.get()), spec->readonly);
            case TypeSpec::K::Slice:
                return Type::make_slice(resolve_spec(spec->elem.get()), spec->readonly);
            case TypeSpec::K::Array:
                return Type::make_array(resolve_spec(spec->elem.get()), spec->size);
        }
        fail("未知类型规格", spec->line, spec->col);
    }

    // ---------------------------------------------------------- 状态
    Program& prog_;

    std::vector<std::string> forward_;    // struct 前置声明
    std::vector<std::string> type_defs_;  // 枚举 / struct / 数组 / 切片
    std::vector<std::string> globals_;    // 全局变量定义
    std::vector<std::string> protos_;     // 函数原型（含 extern）
    std::vector<std::string> funcs_;      // 函数定义

    std::unordered_set<std::string> types_done_;

    // 每个函数重置的局部命名状态
    int n_ = 0;
    std::vector<std::string> buf_;
    int indent_ = 0;
    std::string unit_;
    FuncDecl* func_ = nullptr;
};

}  // namespace

std::string generate(Program& prog) { return Emitter(prog).run(); }
