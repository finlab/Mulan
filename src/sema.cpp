// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 语义分析：名字解析、类型检查、类型注解
//
// 流程与 Python 版一致：
//   collect -> place_structs -> resolve_unit_types -> check_unit -> check_entry
// 所有解析结果直接写回 AST 节点（契约见 ast.hpp 顶部）。
#include "sema.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common.hpp"
#include "mangle.hpp"

namespace {

// ---------------------------------------------------------------- 小工具

bool is_integer(const TypePtr& t) { return t && t->is_integer(); }
bool is_numeric(const TypePtr& t) { return t && t->is_numeric(); }
bool is_void(const TypePtr& t) { return t && t->kind == TypeKind::Void; }
bool is_prim(const TypePtr& t) { return t && t->kind == TypeKind::Prim; }
bool is_enum_t(const TypePtr& t) { return t && t->kind == TypeKind::Enum; }
// 枚举只允许与「真正的整数类型」显式互转（bool 与枚举都不算）
bool is_plain_int(const TypePtr& t) { return is_prim(t) && t->is_integer(); }

// 类型语法文本化。ast.hpp 里声明了 TypeSpec::str()，但它的定义属于 parser.cpp；
// 这里自带一份，避免 sema 对 parser 的链接期依赖（只在错误信息里用）。
std::string spec_text(const TypeSpec* s) {
    if (!s) return "?";
    switch (s->kind) {
        case TypeSpec::K::Named:
            return s->name;
        case TypeSpec::K::Ptr:
            return spec_text(s->elem.get()) + "*";
        case TypeSpec::K::Slice:
            return spec_text(s->elem.get()) + "[" + std::string(s->rank - 1, ',') + "]" +
                   (s->is_const ? " (const)" : "");
        case TypeSpec::K::Array:
            return spec_text(s->elem.get()) + "[" + std::to_string(s->size) + "]";
    }
    return "?";
}

// 枚举常量要被 AST 长期持有（Name::sym 是裸指针），而 Program 里没有存放它们的位置。
// 用进程级 deque 存储：push_back 不会让已有元素失效，Program 销毁后指针仍然有效。
EnumConst* intern_enum_const(const std::string& unit, const std::string& enum_name,
                             const std::string& member, long long value, const TypePtr& ty) {
    static std::deque<EnumConst> pool;
    pool.push_back(EnumConst{unit, enum_name, member, value, ty});
    return &pool.back();
}

// ---------------------------------------------------------------- Sema

class Sema {
public:
    Sema(std::vector<std::unique_ptr<Unit>> units, std::string entry)
        : units_(std::move(units)), entry_(std::move(entry)) {}

    Program run() {
        collect();
        place_structs();
        for (auto& u : units_) resolve_unit_types(u.get());
        check_layouts();
        check_hierarchy_cycles();
        compute_abstract_all();
        check_public_signatures();
        for (auto& u : units_) check_unit(u.get());
        check_entry();

        Program prog;
        prog.units = std::move(units_);
        prog.infos = std::move(infos_);
        prog.entry = entry_;
        return prog;
    }

private:
    using Scope = std::unordered_map<std::string, LocalVar*>;

    // for 循环体异常时也要退出作用域（对应 Python 的 try/finally）
    struct ScopeGuard {
        Sema& sema;
        explicit ScopeGuard(Sema& s) : sema(s) { sema.push(); }
        ~ScopeGuard() { sema.pop(); }
    };

    // ------------------------------------------------------------ 字段

    std::vector<std::unique_ptr<Unit>> units_;
    std::string entry_;
    std::unordered_map<std::string, UnitInfo> infos_;
    Unit* cur_ = nullptr;
    FuncDecl* cur_func_ = nullptr;
    TypePtr cur_ret_;
    std::string cur_name_;
    TypePtr cur_class_;
    bool in_ctor_ = false;
    bool in_dtor_ = false;
    bool super_ok_ = false;
    std::vector<std::unique_ptr<LocalVar>> this_local_;
    std::vector<Scope> scopes_;
    int loop_depth_ = 0;
    std::unordered_map<int, TypePtr> auto_groups_;  // auto 同一声明的推断结果
    Expr* direct_init_expr_ = nullptr;  // 当前正在检查的直接初始化表达式（构造器就地构造）

    // ------------------------------------------------------------ 工具

    [[noreturn]] void err(const std::string& msg, const Node* node = nullptr) const {
        const int line = node ? node->line : 0;
        const int col = node ? node->col : 0;
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string());
    }

    // 警告不中止编译，只打到 stderr（格式与错误一致，前缀 warning）。
    void warn(const std::string& msg, const Node* node) const {
        const int line = node ? node->line : 0;
        const int col = node ? node->col : 0;
        std::cerr << (cur_ ? cur_->filename : std::string()) << ":" << line << ":" << col
                  << ": warning: " << msg << "\n";
    }

    // TypeSpec 不是 Node（自带 line/col），单独一个报错入口。
    [[noreturn]] void err_ts(const std::string& msg, const TypeSpec* spec) const {
        const int line = spec ? spec->line : 0;
        const int col = spec ? spec->col : 0;
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string());
    }

    UnitInfo& info() { return infos_.at(cur_->name); }

    UnitInfo* std_info() {
        auto it = infos_.find("std.string");
        return it == infos_.end() ? nullptr : &it->second;
    }

    // std.string 中的规范字符串结构体（名字就是 `string`）
    TypePtr str_type() {
        if (UnitInfo* std = std_info()) {
            auto it = std->structs.find("string");
            if (it != std->structs.end()) return it->second->ty;
        }
        return nullptr;
    }

    bool is_str(const TypePtr& t) {
        TypePtr s = str_type();
        return t && s && same_type(t, s);
    }

    bool implicit_ok(const TypePtr& src, const TypePtr& dst) {
        if (is_str(src) && dst && dst->kind == TypeKind::Ptr && dst->elem &&
            dst->elem->is_const &&
            dst->elem->kind == TypeKind::Prim && dst->elem->bits() == 8) {
            return true;  // Str -> readonly char*/byte*（NUL 结尾）
        }
        return implicitly_convertible(src, dst);
    }

    bool uses_unit(const std::string& uname) const {
        if (!cur_) return false;
        for (const auto& use : cur_->uses) {
            if (use->unit_name == uname) return true;
        }
        return false;
    }

    // std.string 始终作为隐式依赖加载，等价于已 `use`。
    bool unit_imported(const std::string& uname) const {
        return uname == "std.string" || uses_unit(uname);
    }

    // ------------------------------------------------------------ 可见性
    //
    // 库边界 = unit 名的第一段（`std.string` → `std`；无点号的 unit 属于根库）。
    // 两档：默认仅本 unit 可见；`public` 跨 unit 可见。跨库还需要目标 unit 本身 `public unit`。

    static std::string lib_of(const std::string& unit_name) {
        const auto dot = unit_name.find('.');
        return dot == std::string::npos ? std::string() : unit_name.substr(0, dot);
    }

    // 目标 unit 是否可被当前 unit 引用：同库总是可以；跨库要求 `public unit`。
    bool unit_visible(const std::string& target) const {
        auto it = infos_.find(target);
        if (it == infos_.end()) return false;
        if (cur_ && lib_of(target) == lib_of(cur_->name)) return true;
        return it->second.unit && it->second.unit->is_public;
    }

    // 声明级可见性：本 unit 内全可见；其他 unit 要求声明为 public。
    bool decl_visible(const std::string& owner_unit, bool is_public) const {
        if (!cur_ || owner_unit == cur_->name) return true;
        if (!unit_visible(owner_unit)) return false;
        return is_public;
    }

    bool type_is_public(const TypePtr& t) const {
        if (!t) return true;
        auto it = infos_.find(t->unit);
        if (it == infos_.end()) return true;  // 内建类型
        const UnitInfo& ui = it->second;
        switch (t->kind) {
            case TypeKind::Struct: {
                auto p = ui.structs.find(t->name);
                return p != ui.structs.end() && p->second->is_public;
            }
            case TypeKind::Enum: {
                auto p = ui.enums.find(t->name);
                return p != ui.enums.end() && p->second->is_public;
            }
            case TypeKind::Class: {
                auto p = ui.classes.find(t->name);
                return p != ui.classes.end() && p->second->is_public;
            }
            case TypeKind::Interface: {
                auto p = ui.interfaces.find(t->name);
                return p != ui.interfaces.end() && p->second->is_public;
            }
            default: return true;
        }
    }

    // 类型声明可见性（带类型名，用于报错）。
    void require_type_visible(const TypePtr& t, const TypeSpec* spec) const {
        if (!t) return;
        if (cur_ && t->unit == cur_->name) return;
        if (!unit_visible(t->unit)) {
            err_ts("unit `" + t->unit + "` is only visible within its library; cross-library use requires `public unit`", spec);
        }
        if (!type_is_public(t)) {
            err_ts("type `" + t->unit + "." + t->name + "` is not declared public", spec);
        }
    }

    // ------------------------------------------------------------ 主流程

    // ------------------------------------------------------------ 收集
    void collect() {
        std::unordered_set<std::string> seen_units;
        for (auto& u : units_) {
            if (!seen_units.insert(u->name).second) {
                cur_ = u.get();
                err("duplicate unit `" + u->name + "`");
            }
            UnitInfo ui;
            ui.unit = u.get();
            infos_.emplace(u->name, std::move(ui));
        }

        for (auto& u : units_) {
            cur_ = u.get();
            UnitInfo& ui = infos_.at(u->name);
            for (auto& d : u->decls) {
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    ui.funcs[f->name].push_back(f);
                    f->sym = f;
                } else if (auto s = dynamic_cast<StructDecl*>(d.get())) {
                    ui.structs[s->name] = s;
                } else if (auto e = dynamic_cast<EnumDecl*>(d.get())) {
                    ui.enums[e->name] = e;
                } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                    if (ui.classes.count(c->name)) err("duplicate definition of class `" + c->name + "`", c);
                    ui.classes[c->name] = c;
                } else if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) {
                    if (ui.interfaces.count(i->name)) err("duplicate definition of interface `" + i->name + "`", i);
                    ui.interfaces[i->name] = i;
                } else if (auto g = dynamic_cast<GlobalVar*>(d.get())) {
                    if (ui.globals.count(g->name)) err("duplicate definition of variable `" + g->name + "`", g);
                    ui.globals[g->name] = g;
                    g->cname = g->is_extern ? g->name : mangle(u->name, g->name);
                }
            }

            // 同域名字不能跨类别冲突
            std::unordered_map<std::string, Node*> names;
            for (auto& d : u->decls) {
                Node* node = d.get();
                std::string nm;
                if (auto f = dynamic_cast<FuncDecl*>(node)) {
                    nm = f->name;
                } else if (auto s = dynamic_cast<StructDecl*>(node)) {
                    nm = s->name;
                } else if (auto e = dynamic_cast<EnumDecl*>(node)) {
                    nm = e->name;
                } else if (auto c = dynamic_cast<ClassDecl*>(node)) {
                    nm = c->name;
                } else if (auto i = dynamic_cast<InterfaceDecl*>(node)) {
                    nm = i->name;
                } else if (auto g = dynamic_cast<GlobalVar*>(node)) {
                    nm = g->name;
                } else {
                    continue;
                }
                if (dynamic_cast<FuncDecl*>(node)) {
                    auto it = names.find(nm);
                    if (it != names.end() && !dynamic_cast<FuncDecl*>(it->second)) {
                        err("`" + nm + "` conflicts with another declaration", node);
                    }
                    if (it == names.end()) names.emplace(nm, node);
                    continue;
                }
                if (names.count(nm)) err("`" + nm + "` is declared more than once in the same unit", node);
                names.emplace(nm, node);
            }
        }

        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& use : u->uses) {
                if (!infos_.count(use->unit_name)) {
                    err("cannot find unit `" + use->unit_name + "`", use.get());
                }
                if (!unit_visible(use->unit_name)) {
                    err("unit `" + use->unit_name +
                            "` is only visible within its library; cross-library use requires `public unit`",
                        use.get());
                }
            }
        }
    }

    // ------------------------------------------------------------ public 签名连锁检查
    //
    // public 声明对外暴露的类型也必须 public，且其 unit 必须可见；否则外部拿到签名却无法命名类型。

    void check_exposed_type(const TypePtr& t, const std::string& what, const Node* at) {
        if (!t) return;
        TypePtr b = t;
        while (b && (b->kind == TypeKind::Ptr || b->kind == TypeKind::Array ||
                     b->kind == TypeKind::Slice)) {
            b = b->elem;
        }
        if (!b) return;
        if (b->kind != TypeKind::Struct && b->kind != TypeKind::Class &&
            b->kind != TypeKind::Interface && b->kind != TypeKind::Enum) {
            return;
        }
        if (cur_ && b->unit == cur_->name) {
            if (!type_is_public(b)) {
                err(what + " cannot use non-public type `" + b->str() + "`", at);
            }
            return;
        }
        if (!unit_visible(b->unit)) {
            err(what + " cannot use type `" + b->str() + "` from non-visible unit `" + b->unit + "`", at);
        }
        if (!type_is_public(b)) {
            err(what + " cannot use non-public type `" + b->str() + "`", at);
        }
    }

    void check_public_signatures() {
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                if (!d->is_public) continue;
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    if (f->is_extern) continue;
                    const std::string what = "public function `" + f->name + "`";
                    check_exposed_type(f->rty, what, f);
                    for (auto& p : f->pty) check_exposed_type(p, what, f);
                } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                    const std::string what = "public class `" + c->name + "`";
                    if (c->ty->base) check_exposed_type(c->ty->base, "base class of " + what, c);
                    for (auto& i : c->ty->interfaces) {
                        check_exposed_type(i, "interface of " + what, c);
                    }
                    for (auto& fd : c->ty->fields) {
                        if (fd.is_public) {
                            check_exposed_type(fd.type, "public field `" + fd.name + "` of " + what, c);
                        }
                    }
                    for (auto& m : c->ty->methods) {
                        if (!m.is_public) continue;
                        const std::string mw = "public method `" + m.name + "` of " + what;
                        check_exposed_type(m.ret, mw, c);
                        for (auto& p : m.params) check_exposed_type(p, mw, c);
                    }
                } else if (auto sd = dynamic_cast<StructDecl*>(d.get())) {
                    const std::string what = "public struct `" + sd->name + "`";
                    for (auto& fd : sd->ty->fields) {
                        check_exposed_type(fd.type, "field `" + fd.name + "` of " + what, sd);
                    }
                    for (auto& m : sd->ty->methods) {
                        if (!m.is_public) continue;
                        const std::string mw = "public method `" + m.name + "` of " + what;
                        check_exposed_type(m.ret, mw, sd);
                        for (auto& p : m.params) check_exposed_type(p, mw, sd);
                    }
                } else if (auto itf = dynamic_cast<InterfaceDecl*>(d.get())) {
                    const std::string what = "public interface `" + itf->name + "`";
                    for (auto& m : itf->ty->methods) {
                        const std::string mw = "method `" + m.name + "` of " + what;
                        check_exposed_type(m.ret, mw, itf);
                        for (auto& p : m.params) check_exposed_type(p, mw, itf);
                    }
                }
            }
        }
    }

    void place_structs() {
        for (auto& u : units_) {
            for (auto& d : u->decls) {
                if (auto s = dynamic_cast<StructDecl*>(d.get())) {
                    s->ty = Type::make_struct(u->name, s->name);
                } else if (auto e = dynamic_cast<EnumDecl*>(d.get())) {
                    // 与 Python 版不同（见回报）：枚举类型在这里就建好，保证跨 unit
                    // 引用到“尚未 resolve 的 unit 的枚举”时不会拿到空类型。
                    e->ty = Type::make_enum(u->name, e->name);
                } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                    c->ty = Type::make_class(u->name, c->name);
                } else if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) {
                    i->ty = Type::make_interface(u->name, i->name);
                }
            }
        }
    }

    void resolve_class_fields(ClassDecl* c, Unit* u) {
        std::vector<Field> fields;
        if (c->ty->base) fields = c->ty->base->fields;
        std::unordered_set<std::string> fnames;
        for (auto& f : fields) fnames.insert(f.name);
        for (auto& [fname, spec] : c->fields) {
            if (!fnames.insert(fname).second) {
                err("duplicate field `" + fname + "` in class `" + c->name + "`", c);
            }
            TypePtr ft = resolve_type(spec.get(), u);
            if (is_void(ft)) err("class field cannot be void", c);
            if (spec->weak && !is_class_type(ft)) {
                err("`weak` field must be a class reference (interfaces do not support `weak` yet)", c);
            }
            if (noncopyable_struct(ft)) {
                err("struct `" + ft->str() +
                        "` has a destructor and cannot be used as a field (field destruction is not implemented)",
                    c);
            }
            bool field_pub = false;  // class 默认 private
            if (auto vit = c->field_visibility.find(fname); vit != c->field_visibility.end()) {
                field_pub = vit->second;
            }
            fields.push_back(Field{fname, ft, spec->weak, field_pub});
        }
        c->ty->fields = std::move(fields);
    }

    // ------------------------------------------------------------ 布局修饰符
    //
    // packed / align(N) 的最终布局由 types.cpp 的 type_align/type_size 计算；
    // 这里只负责求值 N、校验 N 与自然对齐的关系，以及 packed 的字段类型限制。

    bool eval_const_int(Expr* e, long long& out) {
        if (!e) return false;
        if (auto il = dynamic_cast<IntLit*>(e)) {
            out = il->value;
            return true;
        }
        if (auto cl = dynamic_cast<CharLit*>(e)) {
            out = cl->value;
            return true;
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            long long v = 0;
            if (!eval_const_int(u->operand.get(), v)) return false;
            if (u->op == "-") {
                out = -v;
                return true;
            }
            if (u->op == "+") {
                out = v;
                return true;
            }
            return false;
        }
        if (auto c = dynamic_cast<Cast*>(e)) return eval_const_int(c->expr.get(), out);
        if (auto b = dynamic_cast<Binary*>(e)) {
            long long l = 0, r = 0;
            if (!eval_const_int(b->left.get(), l) || !eval_const_int(b->right.get(), r)) {
                return false;
            }
            const std::string& op = b->op;
            if (op == "+") out = l + r;
            else if (op == "-") out = l - r;
            else if (op == "*") out = l * r;
            else if (op == "/") {
                if (r == 0) return false;
                out = l / r;
            } else if (op == "%") {
                if (r == 0) return false;
                out = l % r;
            } else if (op == "<<") {
                if (r < 0 || r > 62) return false;
                out = l << r;
            } else if (op == ">>") {
                if (r < 0 || r > 63) return false;
                out = l >> r;
            } else if (op == "&") out = l & r;
            else if (op == "|") out = l | r;
            else if (op == "^") out = l ^ r;
            else return false;
            return true;
        }
        return false;
    }

    static bool is_pow2(long long n) { return n > 0 && (n & (n - 1)) == 0; }

    void check_layouts() {
        // 第一遍：求值 align(N)，校验正整数 + 2 的幂
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto sd = dynamic_cast<StructDecl*>(d.get());
                if (!sd || !sd->ty) continue;
                sd->ty->is_packed = sd->is_packed;
                if (!sd->align_expr) continue;
                long long n = 0;
                if (!eval_const_int(sd->align_expr.get(), n)) {
                    err("N in `align(N)` must be a compile-time integer constant", sd);
                }
                if (!is_pow2(n) || n > 65536) {
                    err("N in `align(N)` must be a power of two (got " + std::to_string(n) + ")", sd);
                }
                sd->ty->align_req = static_cast<int>(n);
            }
        }
        // 第二遍：N 与自然对齐比较 + packed 字段类型限制（此时所有 unit 的字段都已解析）
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto sd = dynamic_cast<StructDecl*>(d.get());
                if (!sd || !sd->ty) continue;
                const TypePtr& ty = sd->ty;
                const std::string kw = sd->is_union ? "union" : "struct";
                if (ty->align_req > 0 && !ty->is_packed) {
                    size_t natural = 1;
                    for (auto& f : ty->fields) natural = std::max(natural, type_align(f.type));
                    if (static_cast<size_t>(ty->align_req) < natural) {
                        err("align(" + std::to_string(ty->align_req) + ") is less than the natural alignment of " + kw + " `" +
                                sd->name + "` (" + std::to_string(natural) + ")" +
                                "; use `packed` to lower the alignment",
                            sd);
                    }
                }
                if (!ty->is_packed) continue;
                for (auto& f : ty->fields) {
                    // 数组按元素类型检查
                    TypePtr base = f.type;
                    while (base && base->kind == TypeKind::Array) base = base->elem;
                    if (!base) continue;
                    if (is_str(base)) {
                        err("field `" + f.name + "` of packed " + kw + " `" + sd->name +
                                "` cannot be `string` (unaligned pointer access)",
                            sd);
                    }
                    if (base->kind == TypeKind::Slice) {
                        err("field `" + f.name + "` of packed " + kw + " `" + sd->name +
                                "` cannot be a dynamic array `" + base->str() + "` (unaligned pointer access)",
                            sd);
                    }
                    if (is_reference_type(base)) {
                        err("field `" + f.name + "` of packed " + kw + " `" + sd->name +
                                "` cannot be a `class` or `interface` reference (unaligned pointer access)",
                            sd);
                    }
                }
            }
        }
    }

    void resolve_unit_types(Unit* u) {
        cur_ = u;

        // 结构体：占位类型已就绪，这里填字段（字段可引用别的结构体/枚举）
        for (auto& d : u->decls) {
            auto sd = dynamic_cast<StructDecl*>(d.get());
            if (!sd) continue;
            if (!sd->ty) sd->ty = Type::make_struct(u->name, sd->name);
            sd->ty->is_packed = sd->is_packed;  // align_req 在 check_layouts 里求值
            sd->ty->is_union = sd->is_union;
            const std::string kw = sd->is_union ? "union" : "struct";
            std::vector<Field> fields;
            std::unordered_set<std::string> fnames;
            for (auto& [fname, spec] : sd->fields) {
                if (!fnames.insert(fname).second) {
                    err("duplicate field `" + fname + "` in " + kw + " `" + sd->name + "`", sd);
                }
                TypePtr ft = resolve_type(spec.get(), u);
                if (is_void(ft)) err("field of " + kw + " `" + sd->name + "` cannot be void", sd);
                if (same_type(ft, sd->ty)) {
                    err(kw + " `" + sd->name + "` cannot contain itself by value", sd);
                }
                if (spec->weak) err("`weak` can only be used on class fields", sd);
                if (sd->is_union && (is_str(ft) || ft->needs_cleanup())) {
                    err("field `" + fname + "` of union `" + sd->name + "` cannot be `" + ft->str() +
                            "`: union members cannot hold values that need reference counting",
                        sd);
                }
                if (noncopyable_struct(ft)) {
                    err("struct `" + ft->str() +
                            "` has a destructor and cannot be used as a field (field destruction is not implemented)",
                        sd);
                }
                bool field_pub = true;  // struct 默认 public
                if (auto vit = sd->field_visibility.find(fname);
                    vit != sd->field_visibility.end()) {
                    field_pub = vit->second;
                }
                fields.push_back(Field{fname, ft, false, field_pub});
            }
            sd->ty->fields = std::move(fields);
            if (sd->is_union && sd->ty->fields.empty()) {
                err("union `" + sd->name + "` must have at least one member", sd);
            }

            sd->ty->methods.clear();
            for (auto& m : sd->methods) {
                m->rty = resolve_type(m->ret.get(), u);
                if (noncopyable_struct(m->rty)) {
                    err("struct `" + m->rty->str() +
                            "` has a destructor and cannot be returned by value (use a pointer)",
                        m.get());
                }
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = false;
                sig.is_override = false;
                sig.is_public = m->is_public;
                sig.owner = sd->ty;
                sd->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, sd->name, m->name, m->pty);
            }
            sd->ty->ctors.clear();
            for (auto& ctor : sd->ctors) {
                ctor->pty.clear();
                for (auto& p : ctor->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    ctor->pty.push_back(pt);
                }
                ctor->cname = ctor_cname(u->name, sd->name, ctor->pty);
                sd->ty->ctors.push_back(ctor->pty);
                sd->ty->ctor_is_public.push_back(ctor->is_public);
            }
            sd->ty->has_dtor = sd->dtor != nullptr;
            if (sd->dtor) sd->dtor->cname = dtor_cname(u->name, sd->name);
        }

        // class：基类、接口、扁平字段、方法/构造器签名
        for (auto& d : u->decls) {
            auto c = dynamic_cast<ClassDecl*>(d.get());
            if (!c) continue;
            if (!c->ty) c->ty = Type::make_class(u->name, c->name);

            bool has_base = false;
            TypeSpec where;
            where.line = c->line;
            where.col = c->col;
            for (size_t i = 0; i < c->bases.size(); ++i) {
                TypePtr bt = resolve_named_type(c->bases[i], u, &where);
                if (bt->kind == TypeKind::Class) {
                    if (i != 0 || has_base) err("a class may have at most one base class, and it must come first", c);
                    has_base = true;
                    c->ty->base = bt;
                } else if (bt->kind == TypeKind::Interface) {
                    for (auto& prev : c->ty->interfaces) {
                        if (same_type(prev, bt)) {
                            err("class `" + c->name + "` implements interface `" + bt->str() + "` more than once", c);
                        }
                    }
                    c->ty->interfaces.push_back(bt);
                } else {
                    err("`" + c->bases[i] + "` is not a class or interface", c);
                }
            }

            // 字段在后面的拓扑 pass 处理（基类字段需先就绪）

            c->ty->methods.clear();
            c->ty->is_abstract = false;
            for (auto& m : c->methods) {
                m->rty = resolve_type(m->ret.get(), u);
                if (noncopyable_struct(m->rty)) {
                    err("struct `" + m->rty->str() +
                            "` has a destructor and cannot be returned by value (use a pointer)",
                        m.get());
                }
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = !m->body;
                sig.is_override = m->is_override;
                sig.is_public = m->is_public;
                sig.owner = c->ty;
                c->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, c->name, m->name, m->pty);
                if (!m->body) c->ty->is_abstract = true;
            }
            for (auto& ctor : c->ctors) {
                ctor->pty.clear();
                for (auto& p : ctor->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    ctor->pty.push_back(pt);
                }
                ctor->cname = ctor_cname(u->name, c->name, ctor->pty);
                c->ty->ctors.push_back(ctor->pty);
                c->ty->ctor_is_public.push_back(ctor->is_public);
            }
            if (c->dtor) c->dtor->cname = dtor_cname(u->name, c->name);
        }

        // class 字段：按基类依赖顺序解析（同一 unit 内基类可能后声明）
        {
            std::unordered_map<std::string, ClassDecl*> by_name;
            for (auto& d : u->decls) {
                if (auto c = dynamic_cast<ClassDecl*>(d.get())) by_name[c->name] = c;
            }
            std::unordered_map<ClassDecl*, int> state;
            std::function<void(ClassDecl*)> resolve_fields = [&](ClassDecl* c) {
                if (!c || state[c] != 0) return;
                state[c] = 1;
                for (auto& bn : c->bases) {
                    if (bn.find('.') != std::string::npos) continue;
                    auto it = by_name.find(bn);
                    if (it != by_name.end() && it->second != c) resolve_fields(it->second);
                }
                resolve_class_fields(c, u);
                state[c] = 2;
            };
            for (auto& d : u->decls) {
                if (auto c = dynamic_cast<ClassDecl*>(d.get())) resolve_fields(c);
            }
        }

        // interface：基接口与方法签名
        for (auto& d : u->decls) {
            auto itf = dynamic_cast<InterfaceDecl*>(d.get());
            if (!itf) continue;
            if (!itf->ty) itf->ty = Type::make_interface(u->name, itf->name);
            TypeSpec where;
            where.line = itf->line;
            where.col = itf->col;
            for (auto& b : itf->bases) {
                TypePtr bt = resolve_named_type(b, u, &where);
                if (bt->kind != TypeKind::Interface) {
                    err("an interface can only inherit from an interface", itf);
                }
                for (auto& prev : itf->ty->interfaces) {
                    if (same_type(prev, bt)) {
                        err("interface `" + itf->name + "` inherits `" + bt->str() + "` more than once", itf);
                    }
                }
                itf->ty->interfaces.push_back(bt);
            }
            itf->ty->methods.clear();
            for (auto& m : itf->methods) {
                m->rty = resolve_type(m->ret.get(), u);
                if (noncopyable_struct(m->rty)) {
                    err("struct `" + m->rty->str() +
                            "` has a destructor and cannot be returned by value (use a pointer)",
                        m.get());
                }
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("parameter cannot be void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("struct `" + pt->str() +
                                "` has a destructor and can only be passed by pointer (passing by value would copy)",
                            p.get());
                    }
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = !m->body;
                sig.is_override = false;
                sig.owner = itf->ty;
                itf->ty->methods.push_back(std::move(sig));
                m->cname = method_cname(u->name, itf->name, m->name, m->pty);
            }
        }

        // 枚举：检查成员重复（类型已在 place_structs 建好）
        for (auto& d : u->decls) {
            auto ed = dynamic_cast<EnumDecl*>(d.get());
            if (!ed) continue;
            std::unordered_set<std::string> names;
            for (auto& [m, v] : ed->members) {
                if (!names.insert(m).second) {
                    err("duplicate member `" + m + "` in enum `" + ed->name + "`", ed);
                }
            }
        }

        for (auto& d : u->decls) {
            auto g = dynamic_cast<GlobalVar*>(d.get());
            if (!g) continue;
            g->rty = resolve_type(g->spec.get(), u);
            if (is_void(g->rty)) err("variable `" + g->name + "` cannot be void", g);
        }

        for (auto& d : u->decls) {
            auto f = dynamic_cast<FuncDecl*>(d.get());
            if (!f) continue;
            f->rty = resolve_type(f->ret.get(), u);
            if (noncopyable_struct(f->rty)) {
                err("struct `" + f->rty->str() + "` has a destructor and cannot be returned by value (use a pointer)", f);
            }
            f->pty.clear();
            for (auto& p : f->params) {
                TypePtr pt = resolve_type(p->spec.get(), u);
                if (is_void(pt)) err("parameter `" + p->name + "` cannot be void", p.get());
                if (noncopyable_struct(pt)) {
                    err("struct `" + pt->str() +
                            "` has a destructor and can only be passed by pointer (passing by value would copy)",
                        p.get());
                }
                f->pty.push_back(pt);
            }
            if (f->is_extern) {
                if (contains_reference_type(f->rty)) {
                    err("`extern` function `" + f->name + "` cannot return a class or interface (C code does not participate in reference counting)",
                        f);
                }
                for (auto& pt : f->pty) {
                    if (contains_reference_type(pt)) {
                        err("parameter of `extern` function `" + f->name + "` cannot be a class or interface", f);
                    }
                }
                if (contains_slice_type(f->rty) || [&] {
                        for (auto& pt : f->pty)
                            if (contains_slice_type(pt)) return true;
                        return false;
                    }()) {
                    err("`extern` function `" + f->name +
                            "` cannot use dynamic arrays `T[]` (they carry an RC header and are not C layout)",
                        f);
                }
                f->cname = f->name;
            } else if (f->name == "main" && u->name == entry_) {
                f->cname = "main";
            } else {
                f->cname = func_cname(u->name, f);
            }
        }
    }

    // ------------------------------------------------------------ 类型解析
    TypePtr resolve_type(const TypeSpec* spec, Unit* u) {
        if (!spec) err("missing type");
        switch (spec->kind) {
            case TypeSpec::K::Named: {
                TypePtr t = resolve_named_type(spec->name, u, spec);
                // C 模型：prim 的 const 需要独立类型节点（prim 工厂有单例缓存，不能就地改）
                if (spec->is_const && t && t->kind == TypeKind::Prim) {
                    auto c = std::make_shared<Type>(*t);
                    c->is_const = true;
                    return c;
                }
                return t;
            }
            case TypeSpec::K::Ptr: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_reference_type(elem)) {
                    err_ts("pointers to class or interface types are not supported (reference types are already handles)", spec);
                }
                return Type::make_ptr(elem, spec->is_const);
            }
            case TypeSpec::K::Slice: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_void(elem)) err_ts("dynamic array element cannot be void", spec);
                if (noncopyable_struct(elem)) {
                    err_ts("struct `" + elem->str() +
                               "` has a destructor and cannot be a dynamic array element",
                           spec);
                }
                return Type::make_slice(elem, spec->is_const, spec->rank);
            }
            case TypeSpec::K::Array: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_void(elem)) err_ts("array element cannot be void", spec);
                TypePtr base = elem;
                while (base && base->kind == TypeKind::Array) base = base->elem;
                if (noncopyable_struct(base)) {
                    err_ts("struct `" + base->str() +
                               "` has a destructor and cannot be an array element (array destruction is not implemented)",
                           spec);
                }
                return Type::make_array(elem, spec->size);
            }
        }
        err_ts("unknown type " + spec_text(spec), spec);
    }

    // 在指定 unit 中按名字找类型；require_pub 时只返回 public 声明。
    TypePtr find_type_named(const std::string& unit, const std::string& name,
                            bool require_pub) const {
        auto it = infos_.find(unit);
        if (it == infos_.end()) return nullptr;
        const UnitInfo& ui = it->second;
        auto ok = [&](bool is_pub) { return !require_pub || is_pub; };
        if (auto s = ui.structs.find(name); s != ui.structs.end()) {
            return ok(s->second->is_public) ? s->second->ty : nullptr;
        }
        if (auto e = ui.enums.find(name); e != ui.enums.end()) {
            return ok(e->second->is_public) ? e->second->ty : nullptr;
        }
        if (auto c = ui.classes.find(name); c != ui.classes.end()) {
            return ok(c->second->is_public) ? c->second->ty : nullptr;
        }
        if (auto i = ui.interfaces.find(name); i != ui.interfaces.end()) {
            return ok(i->second->is_public) ? i->second->ty : nullptr;
        }
        return nullptr;
    }

    TypePtr resolve_named_type(const std::string& name, Unit* u, const TypeSpec* spec) {
        if (name == "void") return t_void();
        if (is_prim_name(name)) return Type::make_prim(name);

        // 多段 unit 名：从最长前缀开始匹配（`lib.a.b.T` → unit `lib.a.b` + 类型 `T`）
        for (std::size_t pos = name.rfind('.'); pos != std::string::npos;) {
            const std::string un = name.substr(0, pos);
            const std::string tn = name.substr(pos + 1);
            auto it = infos_.find(un);
            if (it != infos_.end()) {
                if (un != u->name && !unit_imported(un)) {
                    err_ts("unit `" + un + "` is not imported with `use`", spec);
                }
                if (!unit_visible(un)) {
                    err_ts("unit `" + un + "` is only visible within its library; cross-library use requires `public unit`", spec);
                }
                UnitInfo& ui = it->second;
                auto need_pub = [&](bool is_pub) {
                    if (un != u->name && !is_pub) {
                        err_ts("type `" + un + "." + tn + "` is not declared public", spec);
                    }
                };
                if (auto s = ui.structs.find(tn); s != ui.structs.end()) {
                    need_pub(s->second->is_public);
                    return s->second->ty;
                }
                if (auto e = ui.enums.find(tn); e != ui.enums.end()) {
                    need_pub(e->second->is_public);
                    return e->second->ty;
                }
                if (auto c = ui.classes.find(tn); c != ui.classes.end()) {
                    need_pub(c->second->is_public);
                    return c->second->ty;
                }
                if (auto i = ui.interfaces.find(tn); i != ui.interfaces.end()) {
                    need_pub(i->second->is_public);
                    return i->second->ty;
                }
                err_ts("unit `" + un + "` has no type `" + tn + "`", spec);
            }
            if (pos == 0) break;
            pos = name.rfind('.', pos - 1);
        }

        // 当前 unit 优先（含私有类型，本地遮蔽导入）
        if (TypePtr t = find_type_named(u->name, name, false)) return t;

        // 直接 use 的 unit：只收 public 类型；多个来源同名 → 使用点报歧义
        TypePtr found;
        std::string found_unit;
        for (auto& use : u->uses) {
            if (!unit_visible(use->unit_name)) continue;
            TypePtr t = find_type_named(use->unit_name, name, true);
            if (!t) continue;
            if (found) {
                err_ts("type `" + name + "` is ambiguous: `" + found_unit + "." + name + "` and `" +
                           use->unit_name + "." + name + "`; use a qualified name",
                       spec);
            }
            found = t;
            found_unit = use->unit_name;
        }
        if (found) return found;
        if (UnitInfo* stdn = std_info()) {
            if (auto s = stdn->structs.find(name); s != stdn->structs.end()) {
                if (!s->second->is_public)
                    err_ts("type `std.string." + name + "` is not declared public", spec);
                return s->second->ty;
            }
            if (auto e = stdn->enums.find(name); e != stdn->enums.end()) {
                if (!e->second->is_public)
                    err_ts("type `std.string." + name + "` is not declared public", spec);
                return e->second->ty;
            }
            if (auto c = stdn->classes.find(name); c != stdn->classes.end()) {
                if (!c->second->is_public)
                    err_ts("type `std.string." + name + "` is not declared public", spec);
                return c->second->ty;
            }
            if (auto i = stdn->interfaces.find(name); i != stdn->interfaces.end()) {
                if (!i->second->is_public)
                    err_ts("type `std.string." + name + "` is not declared public", spec);
                return i->second->ty;
            }
        }
        err_ts("unknown type `" + name + "`", spec);
    }

    // 表达式里的类型名解析（当前 unit → use 的 public 类型 → std.string），
    // 用于 `Color.Red` 这类枚举限定引用。找不到返回 nullptr。
    TypePtr find_type_in_expr(const std::string& name, const Node* at) {
        if (TypePtr t = find_type_named(cur_->name, name, false)) return t;
        TypePtr found;
        std::string found_unit;
        for (auto& use : cur_->uses) {
            if (!unit_visible(use->unit_name)) continue;
            TypePtr t = find_type_named(use->unit_name, name, true);
            if (!t) continue;
            if (found && !same_type(found, t)) {
                err("type `" + name + "` is ambiguous: `" + found_unit + "." + name + "` and `" +
                        use->unit_name + "." + name + "`; use a qualified name",
                    at);
            }
            if (!found) {
                found = t;
                found_unit = use->unit_name;
            }
        }
        if (found) return found;
        if (UnitInfo* stdn = std_info()) {
            if (auto e = stdn->enums.find(name); e != stdn->enums.end() && e->second->is_public) {
                return e->second->ty;
            }
        }
        return nullptr;
    }

    // 在指定枚举里按名字找成员（顺序与声明一致，显式赋值后继续递增）。
    EnumConst* find_enum_member(const TypePtr& et, const std::string& member) {
        if (!et || et->kind != TypeKind::Enum) return nullptr;
        auto it = infos_.find(et->unit);
        if (it == infos_.end() || !it->second.unit) return nullptr;
        for (auto& d : it->second.unit->decls) {
            auto ed = dynamic_cast<EnumDecl*>(d.get());
            if (!ed || ed->name != et->name) continue;
            long long value = 0;
            bool has = false;
            for (auto& [m, v] : ed->members) {
                if (v.has_value()) {
                    value = *v;
                    has = true;
                } else if (!has) {
                    value = 0;
                    has = true;
                } else {
                    value += 1;
                }
                if (m == member) {
                    return intern_enum_const(it->second.unit->name, ed->name, m, value,
                                             ed->ty ? ed->ty : et);
                }
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------ 作用域
    void push() { scopes_.emplace_back(); }
    void pop() {
        if (!scopes_.empty()) scopes_.pop_back();
    }

    LocalVar* lookup_local(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }

    void declare_local(LocalVar* lv) {
        if (scopes_.empty()) scopes_.emplace_back();
        Scope& sc = scopes_.back();
        if (sc.count(lv->name)) err("duplicate declaration of `" + lv->name + "`", lv);
        sc[lv->name] = lv;
    }

    EnumConst* lookup_enum_const_in(const std::string& unit_name, const std::string& name,
                                    bool require_pub) {
        auto it = infos_.find(unit_name);
        if (it == infos_.end()) return nullptr;
        // 按声明顺序遍历（Python 的 dict 保留插入顺序）
        for (auto& d : it->second.unit->decls) {
            auto ed = dynamic_cast<EnumDecl*>(d.get());
            if (!ed) continue;
            if (require_pub && !ed->is_public) continue;
            long long value = 0;
            bool has = false;
            for (auto& [m, v] : ed->members) {
                if (v.has_value()) {
                    value = *v;
                    has = true;
                } else if (!has) {
                    value = 0;
                    has = true;
                } else {
                    value += 1;
                }
                if (m == name) return intern_enum_const(unit_name, ed->name, m, value, ed->ty);
            }
        }
        return nullptr;
    }

    EnumConst* lookup_enum_const(const std::string& name) {
        return lookup_enum_const_in(cur_->name, name, false);
    }

    // ------------------------------------------------------------ unit 检查
    void check_unit(Unit* u) {
        cur_ = u;
        for (auto& d : u->decls) {
            if (auto g = dynamic_cast<GlobalVar*>(d.get())) check_global(g);
        }
        // 同名函数的签名不能完全重复（重载必须参数不同）
        for (auto& entry : infos_.at(u->name).funcs) {
            auto& vec = entry.second;
            if (entry.first == "main" && vec.size() > 1) {
                err("`main` cannot be overloaded (at most one `main` per unit)", vec[1]);
            }
            if (vec.size() > 1) {
                bool hasExtern = false, hasBody = false;
                for (auto* f : vec) {
                    if (f->is_extern) hasExtern = true;
                    else hasBody = true;
                }
                if (hasExtern && hasBody) {
                    err("function `" + entry.first + "` cannot be declared both `extern` and normal", vec[1]);
                }
                if (hasExtern) err("`extern` function `" + entry.first + "` cannot be overloaded", vec[1]);
            }
            for (size_t i = 0; i < vec.size(); ++i) {
                for (size_t j = i + 1; j < vec.size(); ++j) {
                    bool same = vec[i]->pty.size() == vec[j]->pty.size();
                    if (same) {
                        for (size_t k = 0; k < vec[i]->pty.size(); ++k) {
                            if (!same_type(vec[i]->pty[k], vec[j]->pty[k])) {
                                same = false;
                                break;
                            }
                        }
                    }
                    if (same) err("duplicate signature of function `" + entry.first + "`", vec[j]);
                }
            }
        }
        for (auto& d : u->decls) {
            auto f = dynamic_cast<FuncDecl*>(d.get());
            if (!f || f->is_extern) continue;
            check_func(f);
        }
        for (auto& d : u->decls) {
            if (auto c = dynamic_cast<ClassDecl*>(d.get())) check_class(c);
        }
        for (auto& d : u->decls) {
            if (auto sd = dynamic_cast<StructDecl*>(d.get())) check_struct(sd);
        }
        for (auto& d : u->decls) {
            if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) check_interface(i);
        }
    }

    void check_global(GlobalVar* g) {
        if (!g->rty) err("global variable `" + g->name + "` is missing its type", g);
        if (noncopyable_struct(g->rty)) {
            err("struct `" + g->rty->str() +
                    "` has a destructor and cannot be a global variable (no exit-time destruction)",
                g);
        }
        if (g->rty->kind == TypeKind::Struct && g->rty->needs_cleanup() &&
            !is_str(g->rty)) {
            err("struct `" + g->rty->str() +
                    "` has reference fields and cannot be a global variable (no exit-time destruction)",
                g);
        }
        if (g->is_extern) {
            if (g->init) err("`extern` variable `" + g->name + "` cannot have an initializer", g);
            return;
        }
        if (!g->init) {
            if (g->is_const) err("`const` variable `" + g->name + "` must be initialized", g);
            g->zero = true;
            return;
        }
        if (!is_const_expr(g->init.get())) {
            err("initializer of unit-level variable `" + g->name + "` must be a compile-time constant", g);
        }
        check_decl_init(g->zero, g->rty, g->init.get(), "global initialization", g);
    }

    static bool guarantees_return(Stmt* s) {
        if (!s) return false;
        if (dynamic_cast<ReturnStmt*>(s)) return true;
        if (auto b = dynamic_cast<Block*>(s)) {
            if (b->stmts.empty()) return false;
            return guarantees_return(b->stmts.back().get());
        }
        if (auto i = dynamic_cast<IfStmt*>(s)) {
            if (!i->els) return false;
            return guarantees_return(i->then.get()) && guarantees_return(i->els.get());
        }
        // 值域覆盖全部时，switch 的所有分支都返回才算保证返回
        if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            if (!sw->has_else && !sw->enum_exhaustive) return false;
            if (sw->arms.empty()) return false;
            for (auto& a : sw->arms) {
                if (!guarantees_return(a->body.get())) return false;
            }
            return true;
        }
        return false;
    }

    void check_operator_decl(FuncDecl* f) {
        const std::string sym = f->name.substr(8);
        const size_t n = f->pty.size();
        const bool unary_or_binary = (sym == "-" || sym == "+");
        if (sym == "~") {
            if (n != 1) err("`operator~` takes 1 parameter", f);
        } else if (sym == "[]") {
            if (n != 2) err("`operator[]` takes 2 parameters (container, index)", f);
        } else if (sym == "[]=") {
            if (n != 3) err("`operator[]=` takes 3 parameters (container, index, value)", f);
            if (!is_void(f->rty)) err("`operator[]=` must return void", f);
        } else if (unary_or_binary) {
            if (n != 1 && n != 2) err("`operator" + sym + "` takes 1 or 2 parameters", f);
        } else {
            static const char* allowed[] = {"*", "/", "%", "<", "<=", ">", ">=", "==",
                                            "!=", "&", "|", "^", "<<", ">>"};
            bool ok = false;
            for (auto* a : allowed) {
                if (sym == a) { ok = true; break; }
            }
            if (!ok) err("operator `" + sym + "` cannot be overloaded", f);
            if (n != 2) err("binary operators take 2 parameters", f);
        }

        bool has_user = false;
        for (auto& p : f->pty) {
            if (p->kind == TypeKind::Struct || p->kind == TypeKind::Class) has_user = true;
            if (p->kind == TypeKind::Void) err("operator parameter cannot be void", f);
        }
        if (!has_user) err("operators require at least one struct or class operand", f);

        if (sym == "==" || sym == "!=") {
            for (auto& p : f->pty) {
                if (p->kind == TypeKind::Class || p->kind == TypeKind::Interface) {
                    err("`" + sym + "` on class or interface types is always identity comparison and cannot be overloaded", f);
                }
            }
        }

        if (sym == "<" || sym == "<=" || sym == ">" || sym == ">=" || sym == "==" || sym == "!=") {
            if (!(f->rty && f->rty->kind == TypeKind::Prim && f->rty->name == "bool")) {
                err("comparison operators must return `bool`", f);
            }
        }

        if (sym == "[]" || sym == "[]=") {
            if (!(f->pty[0]->kind == TypeKind::Struct || f->pty[0]->kind == TypeKind::Class)) {
                err("first parameter of `operator" + sym + "` must be a struct or class", f);
            }
            if (!is_integer(f->pty[1])) err("index of `operator[]` must be an integer", f);
        }
    }

    void check_func(FuncDecl* f) {
        if (!f->rty) err("function `" + f->name + "` is missing its return type", f);
        if (f->is_operator) check_operator_decl(f);
        cur_func_ = f;
        cur_ret_ = f->rty;
        cur_name_ = f->name;
        cur_class_ = nullptr;
        in_ctor_ = false;
        in_dtor_ = false;
        scopes_.clear();
        auto_groups_.clear();
        scopes_.emplace_back();
        loop_depth_ = 0;
        f->param_locals.clear();

        for (size_t i = 0; i < f->params.size() && i < f->pty.size(); ++i) {
            Param* p = f->params[i].get();
            auto lv = std::make_unique<LocalVar>();
            lv->name = p->name;
            lv->rty = f->pty[i];
            lv->line = p->line;
            lv->col = p->col;
            declare_local(lv.get());
            f->param_locals.push_back(std::move(lv));
        }

        if (f->body) check_block(f->body.get());

        // 非 void 函数必须保证有返回值；main 例外（掉出等价 return 0）
        if (!is_void(f->rty) && f->cname != "main" && !guarantees_return(f->body.get())) {
            err("function `" + f->name + "` may reach the end of its body without returning a value", f);
        }

        scopes_.clear();
        auto_groups_.clear();
        cur_func_ = nullptr;
        cur_ret_ = nullptr;
        cur_name_.clear();
        this_local_.clear();
    }

    // ------------------------------------------------------------ class / interface

    void begin_member(const TypePtr& cls, const TypePtr& ret, const std::string& name, int line,
                      int col) {
        scopes_.clear();
        auto_groups_.clear();
        scopes_.emplace_back();
        loop_depth_ = 0;
        cur_class_ = cls;
        cur_ret_ = ret;
        cur_name_ = name;
        in_ctor_ = false;
        in_dtor_ = false;
        auto self = std::make_unique<LocalVar>();
        self->name = "this";
        self->rty = cls;
        self->line = line;
        self->col = col;
        declare_local(self.get());
        this_local_.clear();
        this_local_.push_back(std::move(self));
    }

    void end_member() {
        scopes_.clear();
        auto_groups_.clear();
        cur_class_ = nullptr;
        cur_ret_ = nullptr;
        cur_name_.clear();
        in_ctor_ = false;
        this_local_.clear();
    }

    void declare_params(std::vector<std::unique_ptr<LocalVar>>& out,
                        std::vector<std::unique_ptr<Param>>& params,
                        const std::vector<TypePtr>& pty) {
        out.clear();
        for (size_t i = 0; i < params.size() && i < pty.size(); ++i) {
            auto lv = std::make_unique<LocalVar>();
            lv->name = params[i]->name;
            lv->rty = pty[i];
            lv->line = params[i]->line;
            lv->col = params[i]->col;
            declare_local(lv.get());
            out.push_back(std::move(lv));
        }
    }

    static bool same_sig(const MethodSig& a, const MethodSig& b) {
        if (a.name != b.name || a.params.size() != b.params.size()) return false;
        for (size_t i = 0; i < a.params.size(); ++i) {
            if (!same_type(a.params[i], b.params[i])) return false;
        }
        return true;
    }

    static const MethodSig* find_sig(const TypePtr& t, const MethodSig& sig, int depth = 0) {
        if (!t || depth > 256) return nullptr;
        for (auto& m : t->methods) {
            if (same_sig(m, sig)) return &m;
        }
        if (t->kind == TypeKind::Class && t->base) {
            if (auto r = find_sig(t->base, sig, depth + 1)) return r;
        }
        for (auto& i : t->interfaces) {
            if (auto r = find_sig(i, sig, depth + 1)) return r;
        }
        return nullptr;
    }

    static void collect_sigs(const TypePtr& t, const MethodSig& sig,
                             std::vector<const MethodSig*>& out, int depth = 0) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (same_sig(m, sig)) out.push_back(&m);
        }
        if (t->kind == TypeKind::Class && t->base) collect_sigs(t->base, sig, out, depth + 1);
        for (auto& i : t->interfaces) collect_sigs(i, sig, out, depth + 1);
    }

    static const MethodSig* find_method(const TypePtr& t, const std::string& name, size_t argc,
                                        int depth = 0) {
        if (!t || depth > 256) return nullptr;
        for (auto& m : t->methods) {
            if (m.name == name && m.params.size() == argc) return &m;
        }
        if (t->kind == TypeKind::Class && t->base) {
            if (auto r = find_method(t->base, name, argc, depth + 1)) return r;
        }
        for (auto& i : t->interfaces) {
            if (auto r = find_method(i, name, argc, depth + 1)) return r;
        }
        return nullptr;
    }

    static bool has_method_named(const TypePtr& t, const std::string& name, int depth = 0) {
        if (!t || depth > 256) return false;
        for (auto& m : t->methods) {
            if (m.name == name) return true;
        }
        if (t->kind == TypeKind::Class && t->base && has_method_named(t->base, name, depth + 1)) {
            return true;
        }
        for (auto& i : t->interfaces) {
            if (has_method_named(i, name, depth + 1)) return true;
        }
        return false;
    }

    void check_method(ClassDecl* c, MethodDecl* m) {
        if (!m->body) return;  // 抽象方法
        begin_member(c->ty, m->rty, c->name + "." + m->name, m->line, m->col);
        declare_params(m->param_locals, m->params, m->pty);
        check_block(m->body.get());
        if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
            err("method `" + m->name + "` may reach the end of its body without returning a value", m);
        }
        end_member();
    }

    void check_ctor(ClassDecl* c, CtorDecl* ctor) {
        begin_member(c->ty, t_void(), c->name + " constructor", ctor->line, ctor->col);
        in_ctor_ = true;
        declare_params(ctor->param_locals, ctor->params, ctor->pty);

        bool needs_super = false;
        if (c->ty->base) {
            if (c->ty->base->ctors.empty()) {
                needs_super = false;
            } else {
                needs_super = true;
                for (auto& cs : c->ty->base->ctors) {
                    if (cs.empty()) { needs_super = false; break; }
                }
            }
        }

        Block* body = ctor->body.get();
        bool first_super = false;
        if (body && !body->stmts.empty()) {
            auto es = dynamic_cast<ExprStmt*>(body->stmts[0].get());
            auto call = es ? dynamic_cast<Call*>(es->expr.get()) : nullptr;
            auto nm = call ? dynamic_cast<Name*>(call->callee.get()) : nullptr;
            first_super = nm && nm->ident == "super";
        }
        if (needs_super && !first_super) {
            err("constructor must call `super(...)` as its first statement", ctor);
        }
        ctor->has_explicit_super = first_super;

        push();
        size_t start = 0;
        if (body) {
            if (first_super) {
                super_ok_ = true;
                check_stmt(body->stmts[0].get());
                super_ok_ = false;
                start = 1;
            }
            for (size_t i = start; i < body->stmts.size(); ++i) {
                check_stmt(body->stmts[i].get());
            }
        }
        pop();
        super_ok_ = false;
        end_member();
    }

    void check_dtor(ClassDecl* c) {
        DtorDecl* d = c->dtor.get();
        if (!d) return;
        begin_member(c->ty, t_void(), c->name + " destructor", d->line, d->col);
        in_dtor_ = true;
        check_block(d->body.get());
        in_dtor_ = false;
        end_member();
    }

    void check_struct_method(StructDecl* sd, MethodDecl* m) {
        if (!m->body) {
            err("struct method `" + m->name + "` must have a body", m);
        }
        begin_member(sd->ty, m->rty, sd->name + "." + m->name, m->line, m->col);
        declare_params(m->param_locals, m->params, m->pty);
        check_block(m->body.get());
        if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
            err("method `" + m->name + "` may reach the end of its body without returning a value", m);
        }
        end_member();
    }

    void check_struct_ctor(StructDecl* sd, CtorDecl* ctor) {
        begin_member(sd->ty, t_void(), sd->name + " constructor", ctor->line, ctor->col);
        in_ctor_ = true;
        declare_params(ctor->param_locals, ctor->params, ctor->pty);
        check_block(ctor->body.get());
        end_member();
    }

    void check_struct_dtor(StructDecl* sd) {
        DtorDecl* d = sd->dtor.get();
        if (!d) return;
        begin_member(sd->ty, t_void(), sd->name + " destructor", d->line, d->col);
        in_dtor_ = true;
        check_block(d->body.get());
        in_dtor_ = false;
        end_member();
    }

    void check_struct(StructDecl* sd) {
        std::unordered_set<std::string> seen;
        for (auto& m : sd->methods) {
            std::string key = m->name + "(";
            for (auto& p : m->pty) key += p->str() + ",";
            key += ")";
            if (!seen.insert(key).second) {
                err("duplicate method `" + key + "` in struct `" + sd->name + "`", m.get());
            }
            if (sd->ty->field_type(m->name)) {
                err("method `" + m->name + "` in struct `" + sd->name + "` has the same name as a field", m.get());
            }
            check_struct_method(sd, m.get());
        }
        std::unordered_set<std::string> ctor_keys;
        for (auto& ctor : sd->ctors) {
            std::string key;
            for (auto& p : ctor->pty) key += p->str() + ",";
            if (!ctor_keys.insert(key).second) {
                err("duplicate constructor signature in struct `" + sd->name + "`", ctor.get());
            }
            check_struct_ctor(sd, ctor.get());
        }
        check_struct_dtor(sd);
    }

    void check_class(ClassDecl* c) {
        std::unordered_set<std::string> seen;
        for (auto& m : c->methods) {
            MethodSig sig;
            sig.name = m->name;
            sig.params = m->pty;
            std::string key = m->name + "(";
            for (auto& p : m->pty) key += p->str() + ",";
            key += ")";
            if (!seen.insert(key).second) {
                err("duplicate method `" + key + "` in class `" + c->name + "`", m.get());
            }
            if (c->ty->field_type(m->name)) {
                err("method `" + m->name + "` in class `" + c->name + "` has the same name as a field", m.get());
            }

            const MethodSig* base_sig = nullptr;
            if (c->ty->base) base_sig = find_sig(c->ty->base, sig);
            if (!base_sig) {
                for (auto& i : c->ty->interfaces) {
                    if (auto s = find_sig(i, sig)) {
                        base_sig = s;
                        break;
                    }
                }
            }
            if (base_sig) {
                if (!m->is_override) {
                    err("overriding or implementing a base class method requires `override`", m.get());
                }
                std::vector<const MethodSig*> matches;
                collect_sigs(c->ty->base ? c->ty->base : c->ty, sig, matches);
                for (auto& i : c->ty->interfaces) collect_sigs(i, sig, matches);
                for (auto* ms : matches) {
                    if (!same_type(ms->ret, m->rty)) {
                        err("override return type does not match the declaration in `" + ms->owner->str() + "`",
                            m.get());
                    }
                }
                if (!same_type(base_sig->ret, m->rty)) {
                    err("override has a mismatched return type", m.get());
                }
            } else if (m->is_override) {
                err("`override` does not match any base class or interface method", m.get());
            }
            check_method(c, m.get());
        }
        std::unordered_set<std::string> ctor_keys;
        for (auto& ctor : c->ctors) {
            std::string key;
            for (auto& p : ctor->pty) key += p->str() + ",";
            if (!ctor_keys.insert(key).second) {
                err("duplicate constructor signature in class `" + c->name + "`", ctor.get());
            }
            check_ctor(c, ctor.get());
        }
        // 派生类隐式默认构造器要求基类有无参构造器
        if (c->ctors.empty() && c->ty->base && !c->ty->base->ctors.empty()) {
            bool base_default = false;
            for (auto& cs : c->ty->base->ctors) {
                if (cs.empty()) { base_default = true; break; }
            }
            if (!base_default) {
                err("base class of class `" + c->name + "` has no parameterless constructor; declare a constructor explicitly", c);
            }
        }
        // 派生类字段不能遮蔽基类方法名（用字段名访问会与方法调用混淆）
        if (c->ty->base) {
            for (auto& [fname, spec] : c->fields) {
                if (has_method_named(c->ty->base, fname)) {
                    err("field `" + fname + "` in class `" + c->name + "` has the same name as a base class method", c);
                }
            }
        }
        check_dtor(c);
    }

    void check_interface(InterfaceDecl* itf) {
        for (auto& m : itf->methods) {
            if (!m->body) continue;  // 默认方法之外的抽象声明
            begin_member(itf->ty, m->rty, itf->name + "." + m->name, m->line, m->col);
            declare_params(m->param_locals, m->params, m->pty);
            check_block(m->body.get());
            if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
                err("interface method `" + m->name + "` may reach the end of its body without returning a value", m.get());
            }
            end_member();
        }
    }

    static bool class_has_concrete(const TypePtr& cls, const MethodSig& req) {
        TypePtr cur = cls;
        int guard = 0;
        while (cur && cur->kind == TypeKind::Class && guard++ < 1024) {
            for (auto& m : cur->methods) {
                if (same_sig(m, req) && !m.is_abstract) return true;
            }
            cur = cur->base;
        }
        return false;
    }

    static bool iface_has_default(const TypePtr& t, const MethodSig& req, int depth = 0) {
        if (!t || depth > 256) return false;
        for (auto& m : t->methods) {
            if (same_sig(m, req) && !m.is_abstract) return true;
        }
        for (auto& i : t->interfaces) {
            if (iface_has_default(i, req, depth + 1)) return true;
        }
        return false;
    }

    static void collect_abstract_reqs(const TypePtr& t, std::vector<MethodSig>& out,
                                      int depth = 0) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (m.is_abstract) out.push_back(m);
        }
        if (t->kind == TypeKind::Class && t->base) {
            collect_abstract_reqs(t->base, out, depth + 1);
        }
        for (auto& i : t->interfaces) {
            collect_abstract_reqs(i, out, depth + 1);
        }
    }

    static void collect_defaults(const TypePtr& t, std::vector<MethodSig>& out, int depth = 0) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (!m.is_abstract) out.push_back(m);
        }
        for (auto& i : t->interfaces) {
            collect_defaults(i, out, depth + 1);
        }
    }

    void check_hierarchy_cycles() {
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (!c || !c->ty) continue;
                std::unordered_set<Type*> seen;
                TypePtr cur = c->ty;
                while (cur && cur->kind == TypeKind::Class) {
                    if (!seen.insert(cur.get()).second) err("class `" + c->name + "` has a circular inheritance chain", c);
                    cur = cur->base;
                }
            }
        }

        std::unordered_map<Type*, int> color;
        std::function<void(TypePtr)> visit = [&](TypePtr t) {
            if (!t || t->kind != TypeKind::Interface) return;
            int& col = color[t.get()];
            if (col == 1) err("circular interface inheritance: " + t->str());
            if (col == 2) return;
            col = 1;
            for (auto& b : t->interfaces) visit(b);
            col = 2;
        };
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                if (auto i = dynamic_cast<InterfaceDecl*>(d.get()); i && i->ty) visit(i->ty);
            }
        }
    }

    void compute_abstract_all() {
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (!c || !c->ty) continue;

                std::vector<MethodSig> reqs;
                collect_abstract_reqs(c->ty, reqs);
                bool abs = false;
                for (auto& req : reqs) {
                    if (!class_has_concrete(c->ty, req) && !iface_has_default(c->ty, req)) {
                        abs = true;
                        break;
                    }
                }
                c->ty->is_abstract = abs;

                std::vector<MethodSig> defaults;
                for (TypePtr cur = c->ty; cur && cur->kind == TypeKind::Class; cur = cur->base) {
                    for (auto& i : cur->interfaces) collect_defaults(i, defaults);
                }
                for (size_t i = 0; i < defaults.size(); ++i) {
                    for (size_t j = i + 1; j < defaults.size(); ++j) {
                        if (same_sig(defaults[i], defaults[j]) &&
                            !class_has_concrete(c->ty, defaults[i])) {
                            err("default interface method `" + defaults[i].name +
                                    "` is conflicting and must be overridden in the class",
                                c);
                        }
                    }
                }
            }
        }
    }

    void check_block(Block* b) {
        if (!b) return;
        push();
        for (auto& s : b->stmts) check_stmt(s.get());
        pop();
    }

    void check_stmt(Stmt* s) {
        if (!s) return;
        if (auto lv = dynamic_cast<LocalVar*>(s)) {
            check_local(lv);
        } else if (auto es = dynamic_cast<ExprStmt*>(s)) {
            check_expr(es->expr.get());
        } else if (auto as = dynamic_cast<AssignStmt*>(s)) {
            check_assign_stmt(as);
        } else if (auto is = dynamic_cast<IfStmt*>(s)) {
            check_cond(is->cond.get(), "if condition");
            check_block(is->then.get());
            if (is->els) {
                if (auto b = dynamic_cast<Block*>(is->els.get())) {
                    check_block(b);
                } else {
                    check_stmt(is->els.get());
                }
            }
        } else if (auto ws = dynamic_cast<WhileStmt*>(s)) {
            check_cond(ws->cond.get(), "while condition");
            ++loop_depth_;
            check_block(ws->body.get());
            --loop_depth_;
        } else if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            check_for(fs);
        } else if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            check_switch(sw);
        } else if (dynamic_cast<BreakStmt*>(s)) {
            if (loop_depth_ == 0) err("`break` can only appear in a loop", s);
        } else if (dynamic_cast<ContinueStmt*>(s)) {
            if (loop_depth_ == 0) err("`continue` can only appear in a loop", s);
        } else if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            check_return(rs);
        } else if (auto b = dynamic_cast<Block*>(s)) {
            check_block(b);
        } else {
            err("unknown statement", s);
        }
    }

    void check_local(LocalVar* lv) {
        if (lv->is_auto) {
            auto ie = dynamic_cast<InitExpr*>(lv->init.get());
            if (!ie || !ie->expr) err("`auto` must be initialized with an expression", lv);
            Expr* e = ie->expr.get();
            if (dynamic_cast<NullLit*>(e)) err("cannot infer a type from `null`", lv);
            TypePtr t = check_expr(e);
            if (is_void(t)) err("cannot infer a type from a `void` expression", lv);
            if (t->kind == TypeKind::Range) err("cannot infer a type from a range", lv);
            lv->rty = t;
            // 同一声明语句的多个 auto 声明符必须推断出同一类型
            auto it = auto_groups_.find(lv->group);
            if (it == auto_groups_.end()) {
                auto_groups_[lv->group] = t;
            } else if (!same_type(it->second, t)) {
                err("`auto` in the same declaration is inferred as different types: " + it->second->str() + " and " +
                        t->str(),
                    lv);
            }
        } else {
            lv->rty = resolve_type(lv->spec.get(), cur_);
            if (is_void(lv->rty)) err("variable cannot have type `void`", lv);
        }

        if (lv->is_const && !lv->init) {
            err("`const` variable `" + lv->name + "` must be initialized", lv);
        }

        if (!lv->init && lv->rty && lv->rty->kind == TypeKind::Struct &&
            !lv->rty->ctors.empty()) {
            bool has_default = false;
            for (auto& cs : lv->rty->ctors) {
                if (cs.empty()) {
                    has_default = true;
                    break;
                }
            }
            if (!has_default) {
                err("struct `" + lv->rty->str() +
                        "` has no parameterless constructor and must be initialized with a constructor",
                    lv);
            }
        }

        declare_local(lv);
        if (lv->init) check_decl_init(lv->zero, lv->rty, lv->init.get(), "initialization", lv);
    }

    void check_decl_init(bool& zero_flag, const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node) {
        if (!init) err("missing initializer", node);
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            if (ia->items.empty()) {
                if (!is_zeroable(ty)) {
                    err("type `" + ty->str() +
                            "` cannot be initialized with `{}`; only aggregate types (struct, array, slice, string) can use `{}`"
                            "use a plain constant for scalars (0, 0.0, false, null)",
                        node);
                }
                zero_flag = true;
                return;
            }
        }
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            if (auto il = dynamic_cast<IntLit*>(ie->expr.get())) {
                if (il->value == 0 && is_zeroable(ty)) {
                    err("zero-initialize aggregate type `" + ty->str() + "` with `{}`; `= 0` is no longer supported", node);
                }
            }
        }
        check_init_into(ty, init, what, node);
    }

    // 单个初始化位置：声明初始化、聚合成员、嵌套花括号都走这里
    void check_init_into(const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node) {
        if (!init) err("missing initializer", node);
        if (auto ia0 = dynamic_cast<InitAggregate*>(init)) {
            if (ia0->items.empty()) {
                if (!is_zeroable(ty)) {
                    err("type `" + ty->str() +
                            "` cannot be initialized with `{}`; only aggregate types (struct, array, slice, string) can use `{}`"
                            "use a plain constant for scalars (0, 0.0, false, null)",
                        node);
                }
                return;
            }
        }
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            Expr* e = ie->expr.get();
            if (!e) err("missing initializer expression", node);
            if (auto il = dynamic_cast<IntLit*>(e)) {
                if (il->value == 0 && is_zeroable(ty)) {
                    err("zero-initialize aggregate type `" + ty->str() + "` with `{}`; `= 0` is no longer supported", node);
                }
            }
            if (auto call = dynamic_cast<Call*>(e);
                call && ty->kind == TypeKind::Struct && !ty->ctors.empty()) {
                direct_init_expr_ = e;
                check_expr(e, ty);
                direct_init_expr_ = nullptr;
                if (call->callee_kind == "struct_ctor") return;  // 就地构造
                if (!same_type(call->ty, ty)) {
                    err("struct `" + ty->str() +
                            "` has a constructor and must be built with `T(...)` or copy-initialized from an expression of the same type",
                        node);
                }
                return;  // 同类型返回值：拷贝初始化
            }
            if (ty->kind == TypeKind::Array && is_char(ty->elem) &&
                dynamic_cast<StringLit*>(e)) {
                check_expr(e, str_type());
                return;
            }
            check_assign(e, ty, what);
            return;
        }
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            check_aggregate(ia, ty, what);
            return;
        }
        err("unknown initializer", node);
    }

    void check_aggregate(InitAggregate* init, const TypePtr& ty, const std::string& what) {
        auto& items = init->items;

        if (ty->kind == TypeKind::Struct && ty->is_union) {
            // union：只允许「恰好一个具名成员」；不提供位置初始化（显式优于隐式）
            if (items.size() != 1 || items[0].first.empty()) {
                err("union `" + ty->str() +
                        "` must be initialized with exactly one named member, e.g. `{ member = value }`",
                    init);
            }
            const Field* target = nullptr;
            for (auto& f : ty->fields) {
                if (f.name == items[0].first) target = &f;
            }
            if (!target) {
                err("union `" + ty->str() + "` has no member `" + items[0].first + "`", init);
            }
            check_init_into(target->type, items[0].second.get(), what, init);
            return;
        }

        if (ty->kind == TypeKind::Struct && !ty->ctors.empty()) {
            err("struct `" + ty->str() + "` has a constructor and cannot use aggregate initialization", init);
        }

        if (ty->kind == TypeKind::Struct) {
            const auto& fields = ty->fields;
            // 跨 unit 的聚合初始化要求字段全部 public
            if (cur_ && cur_->name != ty->unit) {
                for (auto& f : fields) {
                    if (!f.is_public) {
                        err("field `" + f.name + "` of struct `" + ty->str() +
                                "` is not declared public and cannot be aggregate-initialized from another unit",
                            init);
                    }
                }
            }
            bool any_positional = false;
            bool any_named = false;
            for (auto& item : items) {
                if (item.first.empty()) {
                    any_positional = true;
                } else {
                    any_named = true;
                }
            }
            if (any_positional && any_named) err("cannot mix named and positional initializers", init);
            if (!any_named) {
                if (items.size() != fields.size()) {
                    err("struct `" + ty->str() + "` expects " + std::to_string(fields.size()) +
                            " initializers, but " + std::to_string(items.size()) + " were given",
                        init);
                }
                for (size_t i = 0; i < items.size(); ++i) {
                    check_init_into(fields[i].type, items[i].second.get(), what, init);
                }
                return;
            }

            std::unordered_map<std::string, size_t> fmap;
            for (size_t i = 0; i < fields.size(); ++i) fmap.emplace(fields[i].name, i);

            std::unordered_set<std::string> seen;
            for (auto& [n, sub] : items) {
                auto it = fmap.find(n);
                if (it == fmap.end()) {
                    err("struct `" + ty->str() + "` has no field `" + n + "`", init);
                }
                if (!seen.insert(n).second) err("field `" + n + "` is initialized more than once", init);
                check_init_into(fields[it->second].type, sub.get(), what, init);
            }

            std::string missing;
            for (auto& f : fields) {
                if (seen.count(f.name)) continue;
                if (!missing.empty()) missing += ", ";
                missing += f.name;
            }
            if (!missing.empty()) {
                err("struct `" + ty->str() + "` is missing initializers for: " + missing, init);
            }
            return;
        }

        if (ty->kind == TypeKind::Array) {
            for (auto& item : items) {
                if (!item.first.empty()) err("array initializers cannot use named fields", init);
            }
            if (items.size() > ty->array_size) {
                err("array of length " + std::to_string(ty->array_size) + " was given " +
                        std::to_string(items.size()) + " initializers",
                    init);
            }
            for (auto& item : items) check_init_into(ty->elem, item.second.get(), what, init);
            return;
        }

        if (ty->kind == TypeKind::Slice) {
            if (type_align(ty->elem) > 8) err_slice_elem_align(ty->elem, init);
            if (ty->rank != 1) {
                err("dynamic rectangular array `" + ty->str() + "` must be created with `new T[m,n]`", init);
            }
            for (auto& item : items) {
                if (!item.first.empty()) err("slice initializers cannot use named fields", init);
            }
            for (auto& item : items) check_init_into(ty->elem, item.second.get(), what, init);
            return;
        }

        err("type " + ty->str() + " cannot be brace-initialized", init);
    }

    static bool simple_seq(Expr* e) {
        if (dynamic_cast<Name*>(e)) return true;
        if (auto m = dynamic_cast<Member*>(e)) return simple_seq(m->obj.get());
        return false;
    }

    // 沿 Index/Member 找到最内层的绑定，判断它是否 const
    bool root_is_const(Expr* e) {
        if (!e) return false;
        if (auto n = dynamic_cast<Name*>(e)) {
            if (auto lv = std::get_if<LocalVar*>(&n->sym)) return *lv && (*lv)->is_const;
            if (auto gv = std::get_if<GlobalVar*>(&n->sym)) return *gv && (*gv)->is_const;
            return false;
        }
        if (auto m = dynamic_cast<Member*>(e)) return root_is_const(m->obj.get());
        if (auto ix = dynamic_cast<Index*>(e)) return root_is_const(ix->base.get());
        return false;
    }

    void check_writable(Expr* t) {
        if (auto n = dynamic_cast<Name*>(t)) {
            if (n->ident == "this") err("cannot assign to `this`", t);
            if (root_is_const(t)) err("`" + n->ident + "` is a `const` binding and cannot be assigned", t);
            return;
        }
        if (dynamic_cast<Member*>(t)) {
            if (root_is_const(t)) err("cannot modify a member of a `const` binding", t);
            return;
        }
        if (auto ix = dynamic_cast<Index*>(t)) {
            TypePtr bt = ix->base->ty;
            if (is_str(bt)) err("`string` is immutable; cannot write to elements (use `byte[]` or `str_concat` to build one)", t);
            if (bt && bt->kind == TypeKind::Slice && bt->elem && bt->elem->is_const)
                err("cannot write through a `const` slice", t);
            if (bt && bt->kind == TypeKind::Ptr && bt->elem && bt->elem->is_const)
                err("cannot write through a `const` pointer", t);
            if (root_is_const(ix->base.get())) err("cannot modify an element of a `const` binding", t);
            return;
        }
        if (auto u = dynamic_cast<Unary*>(t)) {
            if (u->op == "*") {
                TypePtr ot = u->operand->ty;
                if (ot && ot->kind == TypeKind::Ptr && ot->elem && ot->elem->is_const)
                    err("cannot write through a `const` pointer", t);
            }
        }
    }

    bool is_weak_field_access(Expr* e) {
        if (auto n = dynamic_cast<Name*>(e)) {
            if (n->kind == "field" && n->implicit_this && cur_class_ && n->field_index >= 0 &&
                n->field_index < static_cast<int>(cur_class_->fields.size())) {
                return cur_class_->fields[n->field_index].weak;
            }
            return false;
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            TypePtr ot = m->obj->ty;
            if (ot && ot->kind == TypeKind::Class && m->field_index >= 0 &&
                m->field_index < static_cast<int>(ot->fields.size())) {
                return ot->fields[m->field_index].weak;
            }
        }
        return false;
    }

    void check_assign_stmt(AssignStmt* s) {
        if (!is_lvalue(s->target.get())) err("left side of an assignment must be assignable", s);

        if (auto ix = dynamic_cast<Index*>(s->target.get())) {
            TypePtr bt = check_expr(ix->base.get());
            if (is_user_type(bt)) {
                if (is_str(bt))
                    err("`string` is immutable; cannot write to elements (use `byte[]` or `str_concat` to build one)",
                        s->target.get());
                if (ix->indices.size() != 1)
                    err("user-defined `operator[]=` takes exactly one index", s->target.get());
                TypePtr it = check_expr(ix->indices[0].get());
                if (!is_integer(it)) err("index must be an integer", s->target.get());
                if (s->op != "=") err("user-defined subscript assignment only supports `=`", s);
                auto cands = collect_operators("[]=", {bt, it});
                if (cands.empty()) {
                    err("type `" + bt->str() + "` does not support subscript assignment (no `operator[]=`)", s);
                }
                check_writable(s->target.get());
                s->set_func = choose_overload(cands,
                                              {ix->base.get(), ix->indices[0].get(),
                                               s->value.get()},
                                              "operator[]=", s);
                return;
            }
        }

        TypePtr tt = check_expr(s->target.get());
        if (auto m = dynamic_cast<Member*>(s->target.get()); m && m->kind == "enum_const") {
            err("enum members are constants and cannot be assigned", s);
        }
        check_writable(s->target.get());
        if (s->op == "=") {
            check_assign(s->value.get(), tt, "assignment");
            return;
        }
        if (is_enum_t(tt)) {
            err("enum `" + tt->str() + "` does not support `" + s->op +
                    "` (write `x = (x as int " + s->op.substr(0, s->op.size() - 1) +
                    " y) as " + tt->name + "`）",
                s);
        }
        const std::string op = s->op.empty() ? std::string() : s->op.substr(0, s->op.size() - 1);
        TypePtr vt = check_expr(s->value.get(), tt);
        if (op == "<<" || op == ">>") {
            if (!is_integer(tt) || !is_integer(vt)) err("`both sides of `" + s->op + "` must be integers", s);
            return;
        }
        if (op == "&" || op == "|" || op == "^") {
            if (!is_integer(tt) || !is_integer(vt)) err("`both sides of `" + s->op + "` must be integers", s);
            return;
        }
        if ((op == "+" || op == "-") && tt->kind == TypeKind::Ptr) {
            if (tt->elem && tt->elem->kind == TypeKind::Void) err("pointer arithmetic is not allowed on `void*`", s);
            if (!is_integer(vt)) err("pointer arithmetic requires an integer operand", s);
            return;
        }
        if (!(is_numeric(tt) && is_numeric(vt))) err("`" + s->op + "` requires numeric operands", s);
    }

    void check_return(ReturnStmt* s) {
        if (in_dtor_) err("`return` is not allowed in a destructor (fields and the base class are destroyed at the end of the function)", s);
        if (!cur_ret_) err("`return` can only appear in a function or method", s);
        TypePtr rt = cur_ret_;
        if (!s->value) {
            if (!is_void(rt)) {
                err("`" + cur_name_ + "` must return a value of type " + (rt ? rt->str() : "void"), s);
            }
            return;
        }
        if (is_void(rt)) err("`void` function or method `" + cur_name_ + "` cannot return a value", s);
        check_assign(s->value.get(), rt, "return value");
    }

    void check_cond(Expr* e, const std::string& what) {
        TypePtr t = check_expr(e);
        if (!(is_prim(t) && t->name == "bool")) {
            err(what + " must be `bool`, found " + (t ? t->str() : "void"), e);
        }
    }

    // switch 分支模式求值：只接受编译期常量（字面量 / 枚举成员）
    long long checked_int_pattern(long long v, const TypePtr& st, Expr* e) {
        if (!promote_int_literal(v, st)) {
            err("switch case value " + std::to_string(v) + " is out of range for " + st->str(), e);
        }
        return v;
    }

    long long pattern_value(Expr* e, const TypePtr& st) {
        if (st->kind == TypeKind::Enum) {
            // 成员名直接按被匹配的枚举解析（`Red` 不必写前缀）
            if (auto* n = dynamic_cast<Name*>(e)) {
                if (EnumConst* ec = find_enum_member(st, n->ident)) {
                    n->kind = "enum_const";
                    n->sym = ec;
                    n->ty = st;
                    return ec->value;
                }
                err("enum `" + st->str() + "` has no member `" + n->ident + "`", e);
            }
            if (auto* m = dynamic_cast<Member*>(e)) {
                TypePtr pt = check_expr(e);
                auto* ec = std::get_if<EnumConst*>(&m->sym);
                if (!ec || !*ec || !pt || !same_type(pt, st)) {
                    err("switch case type does not match the value being switched on: expected " + st->str(), e);
                }
                return (*ec)->value;
            }
            err("cases of an enum switch must be members of " + st->str(), e);
        }

        if (auto* il = dynamic_cast<IntLit*>(e)) return checked_int_pattern(il->value, st, e);
        if (auto* cl = dynamic_cast<CharLit*>(e)) return checked_int_pattern(cl->value, st, e);
        if (auto* u = dynamic_cast<Unary*>(e); u && (u->op == "-" || u->op == "+")) {
            if (auto* il = dynamic_cast<IntLit*>(u->operand.get())) {
                const long long v = u->op == "-" ? -il->value : il->value;
                return checked_int_pattern(v, st, e);
            }
        }
        if (auto* n = dynamic_cast<Name*>(e)) {
            check_expr(e);  // 解析成变量 / 枚举成员，好给出更准的错误
            if (std::get_if<EnumConst*>(&n->sym)) {
                err("pattern `" + n->ident + "` is an enum member and cannot match " + st->str(), e);
            }
            err("switch case pattern must be a compile-time constant: `" + n->ident +
                    "` is a variable; use a literal or an enum member",
                e);
        }
        err("switch case pattern must be a compile-time constant (integer literal, character literal, or enum member)", e);
    }

    std::vector<std::pair<std::string, long long>> enum_members_of(const TypePtr& et) {
        std::vector<std::pair<std::string, long long>> out;
        auto it = infos_.find(et->unit);
        if (it == infos_.end() || !it->second.unit) return out;
        for (auto& d : it->second.unit->decls) {
            auto* ed = dynamic_cast<EnumDecl*>(d.get());
            if (!ed || ed->name != et->name) continue;
            long long value = 0;
            bool has = false;
            for (auto& [m, v] : ed->members) {
                if (v.has_value()) {
                    value = *v;
                    has = true;
                } else if (!has) {
                    value = 0;
                    has = true;
                } else {
                    value += 1;
                }
                out.emplace_back(m, value);
            }
        }
        return out;
    }

    void check_switch(SwitchStmt* s) {
        ScopeGuard guard(*this);
        TypePtr st = check_expr(s->scrut.get());
        if (!st) err("switch is missing the value to match", s);
        const bool enum_sw = st->kind == TypeKind::Enum;
        if (!enum_sw && !(is_prim(st) && is_integer(st))) {
            err("switch value must be an integer or enum (found " + st->str() + ")", s->scrut.get());
        }
        s->sty = st;

        // 重叠检测：按 [lo, hi) 区间合并，top = 覆盖到类型的最大值
        struct Iv {
            long long lo = 0;
            long long hi = 0;
            bool top = false;
            const Node* node = nullptr;
        };
        std::vector<Iv> ivs;

        for (auto& up : s->arms) {
            SwitchArm* arm = up.get();
            if (arm->is_else) {
                if (s->has_else) err("switch can have at most one `else` branch", s);
                s->has_else = true;
            }
            if (arm->pats.empty() && !arm->is_else) {
                err("switch case is missing a pattern", s);
            }
            for (auto& p : arm->pats) {
                p.vlo = pattern_value(p.lo.get(), st);
                Iv iv;
                iv.lo = p.vlo;
                iv.node = p.lo.get();
                if (p.is_range) {
                    if (enum_sw) {
                        err("enum switches do not support range cases (enum members have no numeric order); list each member explicitly",
                            p.lo.get());
                    }
                    p.vhi = pattern_value(p.hi.get(), st);
                    if (p.vlo >= p.vhi) {
                        err("switch range `" + std::to_string(p.vlo) + ".." +
                                std::to_string(p.vhi) +
                                "` is empty: the upper bound of a half-open range must be greater than the lower bound",
                            p.lo.get());
                    }
                    iv.hi = p.vhi;
                } else {
                    if (p.vlo == LLONG_MAX) {
                        iv.hi = p.vlo;
                        iv.top = true;
                        p.vhi = p.vlo;  // 仅内部使用；+1 会溢出
                    } else {
                        iv.hi = p.vlo + 1;
                        p.vhi = p.vlo + 1;  // 仅内部使用（p.vlo 是唯一命中值）
                    }
                }
                ivs.push_back(iv);
            }
            check_block(arm->body.get());
        }

        std::sort(ivs.begin(), ivs.end(),
                  [](const Iv& a, const Iv& b) { return a.lo < b.lo; });
        for (size_t i = 1; i < ivs.size(); ++i) {
            const Iv& prev = ivs[i - 1];
            const Iv& cur = ivs[i];
            if (prev.top || cur.lo < prev.hi) {
                err("overlapping switch cases: value " + std::to_string(cur.lo) +
                        " is already covered by an earlier case (ranges are half-open; `a..b` excludes b)",
                    cur.node);
            }
        }

        if (enum_sw) {
            auto members = enum_members_of(st);
            std::string missing;
            for (auto& [name, v] : members) {
                bool covered = false;
                for (auto& up : s->arms) {
                    for (auto& p : up->pats) {
                        if (!p.is_range && p.vlo == v) covered = true;
                    }
                }
                if (!covered) {
                    if (!missing.empty()) missing += ", ";
                    missing += name;
                }
            }
            if (!s->has_else) {
                if (!missing.empty()) {
                    err("switch does not cover all members of enum " + st->str() + " (missing " + missing +
                            "); add the missing cases or write `else:`",
                        s);
                }
                s->enum_exhaustive = true;
            } else if (missing.empty()) {
                warn("`else` branch is unreachable: all members of enum `" + st->str() + "` are already covered", s);
            }
        }
    }

    // 索引形态（SPEC 6.4.1）：length() -> usize  +  operator[](C, usize) -> T
    void resolve_index_shape(ForInStmt* s, const TypePtr& st) {
        TypePtr us = Type::make_prim("usize");
        const MethodSig* len = find_method(st, "length", 0);
        if (!len) {
            err("`for-in` cannot iterate " + st->str() +
                    ": the indexed form requires a method `length() -> usize` (none found)",
                s->seq.get());
        }
        if (!same_type(len->ret, us)) {
            err("`" + st->str() + "::length()` must return `usize` (found `" +
                    (len->ret ? len->ret->str() : "void") + "`）",
                s->seq.get());
        }

        FuncDecl* picked = nullptr;
        std::string saw_index;
        for (auto* c : collect_operators("[]", {st, us})) {
            if (c->pty.size() != 2) continue;
            bool recv_ok = same_type(c->pty[0], st) ||
                           (c->pty[0] && c->pty[0]->kind == TypeKind::Ptr &&
                            same_type(c->pty[0]->elem, st));
            if (!recv_ok) continue;
            if (!same_type(c->pty[1], us)) {
                if (saw_index.empty() && c->pty[1]) saw_index = c->pty[1]->str();
                continue;
            }
            if (picked) {
                err("`" + st->str() + "` has more than one `operator[](`" + st->str() +
                        ", usize)` overload; `for-in` cannot choose one",
                    s->seq.get());
            }
            picked = c;
        }
        if (!picked) {
            std::string msg = "`for-in` cannot iterate " + st->str() + ": the indexed form also requires `operator[](" +
                              st->str() + ", usize) -> T`";
            if (!saw_index.empty()) {
                msg += "(the `operator[]` found takes index type `" + saw_index + "`; `usize` is required)";
            }
            err(msg, s->seq.get());
        }

        s->loop_kind = "index";
        s->seq_ty = st;
        s->elem_ty = picked->rty;
        s->len_sig = len;
        s->idx_fn = picked;
    }

    void check_for(ForInStmt* s) {
        ScopeGuard guard(*this);

        if (auto r = dynamic_cast<RangeExpr*>(s->seq.get())) {
            TypePtr lt = check_expr(r->lo.get());
            TypePtr ht = check_expr(r->hi.get());
            if (is_enum_t(lt) || is_enum_t(ht)) {
                err("range endpoints cannot be enum values (write `x as int`)", r);
            }
            if (!is_integer(lt) || !is_integer(ht)) err("range bounds must be integers", r);
            TypePtr vt = usual_arith(lt, ht);
            if (!vt) vt = t_int();
            if (!is_integer(vt)) err("the loop variable of a range must be an integer", r);
            s->loop_kind = "range";
            s->seq_ty = t_range();
            s->elem_ty = vt;
        } else {
            TypePtr st = check_expr(s->seq.get());
            if (!st) err("`for-in` is missing a sequence", s);
            if (st->kind == TypeKind::Array) {
                if (st->elem && st->elem->kind == TypeKind::Array) {
                    err("`for-in` does not support multidimensional arrays `" + st->str() +
                            "`: use an index loop, or take one row first and iterate over it",
                        s->seq.get());
                }
                s->loop_kind = "array";
                s->elem_ty = st->elem;
            } else if (st->kind == TypeKind::Slice) {
                if (st->rank > 1) {
                    err("`for-in` does not support multidimensional arrays `" + st->str() +
                            "`: use an index loop (get each dimension's length with `len(a, k)`)",
                        s->seq.get());
                }
                s->loop_kind = "slice";
                s->elem_ty = st->elem;
            } else if (is_str(st)) {
                s->loop_kind = "string";
                s->elem_ty = Type::make_prim("byte");
            } else if (st->kind == TypeKind::Struct || st->kind == TypeKind::Class) {
                resolve_index_shape(s, st);
            } else {
                err("`for-in` cannot iterate " + st->str() +
                        " (supported: range, array, slice, string; user types must provide `length() -> usize` and "
                        "`operator[](C, usize)`）",
                    s->seq.get());
            }
            s->seq_ty = st;
        }

        if (s->by_ref && s->loop_kind == "string") {
            err("`string` is immutable and cannot be iterated with `for ref`", s);
        }
        if (s->by_ref && s->loop_kind == "index") {
            err("user types do not support `for ref` (`operator[]` returns a value); use `for i, x` with `c[i] = ...` instead",
                s->seq.get());
        }
        if (s->by_ref && s->loop_kind == "range") {
            err("`for ref` cannot be used on a range loop", s);
        }
        if (s->by_ref && s->elem_ty && is_reference_type(s->elem_ty)) {
            err("`for ref` is not supported for class or interface elements (bind the handle by value)", s);
        }
        if (s->by_ref && s->seq_ty && s->seq_ty->kind == TypeKind::Slice && s->seq_ty->elem &&
            s->seq_ty->elem->is_const) {
            err("`for ref` cannot be used on a `const` slice", s->seq.get());
        }
        if (s->by_ref && root_is_const(s->seq.get())) {
            err("`for ref` cannot be used on a `const` binding", s->seq.get());
        }
        if (s->by_ref && !simple_seq(s->seq.get())) {
            err("the sequence of `for ref` must be a variable or member access (an lvalue)", s->seq.get());
        }

        TypePtr vt = s->elem_ty;
        auto lv = std::make_unique<LocalVar>();
        lv->name = s->var;
        lv->rty = vt;
        lv->by_ref = s->by_ref;
        lv->line = s->line;
        lv->col = s->col;
        declare_local(lv.get());
        s->var_ty = vt;
        s->lvar = std::move(lv);

        if (s->has_idx) {
            TypePtr us = Type::make_prim("usize");
            auto ilv = std::make_unique<LocalVar>();
            ilv->name = s->idx_name;
            ilv->rty = us;
            ilv->is_const = true;
            ilv->line = s->line;
            ilv->col = s->col;
            declare_local(ilv.get());
            s->idx_ty = us;
            s->idx_lvar = std::move(ilv);
        }

        ++loop_depth_;
        check_block(s->body.get());
        --loop_depth_;
    }

    // ------------------------------------------------------------ 赋值兼容
    static bool contains_reference_type(const TypePtr& t) {
        if (!t) return false;
        if (is_reference_type(t)) return true;
        if (t->kind == TypeKind::Array || t->kind == TypeKind::Slice) {
            return contains_reference_type(t->elem);
        }
        if (t->kind == TypeKind::Struct) {
            for (const auto& f : t->fields) {
                if (f.weak) continue;
                if (contains_reference_type(f.type)) return true;
            }
        }
        return false;
    }

    static bool contains_slice_type(const TypePtr& t) {
        if (!t) return false;
        if (t->kind == TypeKind::Slice) return true;
        if (t->kind == TypeKind::Array) return contains_slice_type(t->elem);
        if (t->kind == TypeKind::Struct) {
            for (const auto& f : t->fields) {
                if (f.weak) continue;
                if (contains_slice_type(f.type)) return true;
            }
        }
        return false;
    }

    static bool is_zeroable(const TypePtr& t) {
        if (!t) return false;
        return t->kind == TypeKind::Struct || t->kind == TypeKind::Array ||
               t->kind == TypeKind::Slice;
    }

    static bool is_char(const TypePtr& t) {
        if (!is_prim(t)) return false;
        return t->name == "char" || t->name == "byte";
    }

    static bool noncopyable_struct(const TypePtr& t) {
        return t && t->kind == TypeKind::Struct && t->has_dtor;
    }

    TypePtr check_assign(Expr* e, const TypePtr& dst, const std::string& what) {
        if (noncopyable_struct(dst)) {
            err(what + ": struct `" + dst->str() +
                    "` has a destructor and cannot be copied by value (use a pointer or an explicit clone)",
                e);
        }
        TypePtr src = check_expr(e, dst);
        if (assignable(e, src, dst)) return src;
        if (is_enum_t(src) || is_enum_t(dst)) {
            err(what + ": cannot assign " + (src ? src->str() : "void") + " to " +
                    (dst ? dst->str() : "void") + " (no implicit conversion between enums and integers; write `as`)",
                e);
        }
        err(what + ": cannot assign " + (src ? src->str() : "void") + " to " +
                (dst ? dst->str() : "void"),
            e);
    }

    bool assignable(Expr* e, const TypePtr& src, const TypePtr& dst) {
        // C 模型：const 只能放宽，不能收紧（same_type 忽略 const，必须先拦）
        if (src && dst && src->kind == dst->kind &&
            (src->kind == TypeKind::Ptr || src->kind == TypeKind::Slice) &&
            src->elem && dst->elem && src->elem->is_const && !dst->elem->is_const) {
            return false;
        }
        if (same_type(src, dst)) return true;
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (promote_int_literal(il->value, dst)) {
                il->ty = dst;
                return true;
            }
            if (il->value == 0 && dst &&
                (dst->kind == TypeKind::Class || dst->kind == TypeKind::Interface)) {
                il->ty = dst;  // `Cat c = 0` 等价于 null
                return true;
            }
        }
        if (dynamic_cast<CharLit*>(e) && dst && is_prim(dst) && dst->is_integer() &&
            dst->bits() == 8) {
            e->ty = dst;  // 字符字面量适配到 char / byte
            return true;
        }
        if (dynamic_cast<NullLit*>(e) && dst) {
            if (dst->kind == TypeKind::Ptr || dst->kind == TypeKind::Slice ||
                dst->kind == TypeKind::Class || dst->kind == TypeKind::Interface) {
                e->ty = dst;
                return true;
            }
        }
        if (src && dst && src->kind == TypeKind::Array && dst->kind == TypeKind::Slice) {
            // T[N] -> T[]；T[m,n] -> T[,]（按嵌套层数匹配 rank，元素类型一致）
            TypePtr base = src;
            size_t depth = 0;
            while (base && base->kind == TypeKind::Array) {
                base = base->elem;
                depth++;
            }
            if (static_cast<int>(depth) == dst->rank && same_type(base, dst->elem)) {
                if (type_align(base) > 8) err_slice_elem_align(base, e);
                return true;
            }
            return false;
        }
        if (is_str(src) && dst && dst->kind == TypeKind::Ptr && dst->elem &&
            dst->elem->is_const &&
            dst->elem->kind == TypeKind::Prim && dst->elem->bits() == 8) {
            return true;  // Str -> readonly char*/byte*（要求 NUL 结尾）
        }
        return implicit_ok(src, dst);
    }

    // ------------------------------------------------------------ 常量表达式
    bool is_const_expr(Init* init) {
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            for (auto& item : ia->items) {
                if (!is_const_expr(item.second.get())) return false;
            }
            return true;
        }
        if (auto ie = dynamic_cast<InitExpr*>(init)) return const_ok(ie->expr.get());
        return false;
    }

    bool const_ok(Expr* e) {
        if (!e) return false;
        if (dynamic_cast<IntLit*>(e) || dynamic_cast<FloatLit*>(e) ||
            dynamic_cast<CharLit*>(e) || dynamic_cast<StringLit*>(e) ||
            dynamic_cast<BoolLit*>(e) || dynamic_cast<NullLit*>(e)) {
            return true;
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            return (u->op == "-" || u->op == "+") && const_ok(u->operand.get());
        }
        if (auto c = dynamic_cast<Cast*>(e)) return const_ok(c->expr.get());
        if (auto b = dynamic_cast<Binary*>(e)) {
            if (b->op == "and" || b->op == "or") return false;
            return const_ok(b->left.get()) && const_ok(b->right.get());
        }
        return false;
    }

    // ------------------------------------------------------------ 表达式
    TypePtr check_expr(Expr* e, const TypePtr& expected = nullptr) {
        if (!e) err("missing expression");
        TypePtr t = check_expr_impl(e, expected);
        e->ty = t;
        return t;
    }

    TypePtr check_expr_impl(Expr* e, const TypePtr& expected) {
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (is_prim(expected) && expected->is_numeric() &&
                promote_int_literal(il->value, expected)) {
                return expected;
            }
            if (il->value >= -(1LL << 31) && il->value < (1LL << 31)) return t_int();
            // long long 本身已在 int64 范围内；超出 64 位由 lexer/parser 报错
            return Type::make_prim("long");
        }

        if (auto fl = dynamic_cast<FloatLit*>(e)) {
            if (is_prim(expected) && (expected->name == "float" || expected->name == "double")) {
                return expected;
            }
            return Type::make_prim(fl->is_float32 ? "float" : "double");
        }

        if (auto cl = dynamic_cast<CharLit*>(e)) {
            (void)cl;
            if (expected && is_prim(expected) && expected->is_integer() &&
                expected->bits() == 8) {
                return expected;
            }
            return Type::make_prim("byte");  // 字符字面量默认是字节
        }
        if (dynamic_cast<StringLit*>(e)) return str_type();
        if (dynamic_cast<BoolLit*>(e)) return t_bool();

        if (dynamic_cast<NullLit*>(e)) {
            if (expected) {
                const auto k = expected->kind;
                if (k == TypeKind::Ptr || k == TypeKind::Slice ||
                    k == TypeKind::Class || k == TypeKind::Interface) {
                    return expected;
                }
            }
            return t_null();
        }

        if (auto nm = dynamic_cast<Name*>(e)) return check_name(nm);
        if (auto m = dynamic_cast<Member*>(e)) return check_member(m);

        if (auto ix = dynamic_cast<Index*>(e)) {
            TypePtr bt = check_expr(ix->base.get());
            if (ix->indices.empty()) err("subscript list cannot be empty", e);
            std::vector<TypePtr> its;
            for (auto& idxExpr : ix->indices) {
                TypePtr it = check_expr(idxExpr.get());
                if (is_enum_t(it)) {
                    err("subscript cannot be an enum `" + it->str() + "` (write `x as int`)", idxExpr.get());
                }
                if (!is_integer(it)) {
                    err("subscript must be an integer, found " + (it ? it->str() : "void"),
                        idxExpr.get());
                }
                its.push_back(it);
            }
            if (!bt) err("type cannot be subscripted", e);
            const size_t n = ix->indices.size();
            if (bt->kind == TypeKind::Array) {
                TypePtr cur = bt;
                size_t depth = 0;
                while (cur && cur->kind == TypeKind::Array) {
                    cur = cur->elem;
                    depth++;
                }
                if (n > depth) {
                    err("too many subscripts: `" + bt->str() + "` has only " + std::to_string(depth) +
                            " dimensions",
                        e);
                }
                TypePtr r = bt;
                for (size_t i = 0; i < n; ++i) r = r->elem;
                return r;
            }
            if (bt->kind == TypeKind::Slice) {
                if (static_cast<int>(n) != bt->rank) {
                    err("`" + bt->str() + "` requires " + std::to_string(bt->rank) +
                            " subscripts (got " + std::to_string(n) + ")",
                        e);
                }
                return bt->elem;
            }
            if (bt->kind == TypeKind::Ptr) {
                if (n != 1) err("pointer accepts exactly one subscript", e);
                return bt->elem;
            }
            if (is_str(bt)) {
                if (n != 1) err("`string` accepts exactly one subscript", e);
                return Type::make_prim("byte");
            }
            if (is_user_type(bt)) {
                if (n != 1) err("user-defined `operator[]` takes exactly one subscript", e);
                auto cands = collect_operators("[]", {bt, its[0]});
                if (cands.empty()) {
                    err("type `" + bt->str() + "` does not support subscripting (no `operator[]`)", e);
                }
                auto f = choose_overload(cands, {ix->base.get(), ix->indices[0].get()},
                                         "operator[]", e);
                ix->get_func = f;
                return f->rty;
            }
            err("type " + bt->str() + " cannot be subscripted", e);
        }

        if (auto u = dynamic_cast<Unary*>(e)) return check_unary(u);

        if (auto nt = dynamic_cast<NotExpr*>(e)) {
            TypePtr t = check_expr(nt->operand.get());
            if (!(is_prim(t) && t->name == "bool")) {
                err("`not` requires a `bool` operand, found " + (t ? t->str() : "void"), e);
            }
            return t_bool();
        }

        if (auto b = dynamic_cast<Binary*>(e)) return check_binary(b);

        if (auto te = dynamic_cast<Ternary*>(e)) return check_ternary(te, expected);

        if (auto c = dynamic_cast<Cast*>(e)) {
            TypePtr st = check_expr(c->expr.get());
            TypePtr dt = resolve_type(c->spec.get(), cur_);
            if (!can_cast(st, dt)) {
                err("cannot use `as` to convert " + (st ? st->str() : "void") + " to " + dt->str(), e);
            }
            e->ty = dt;
            return dt;
        }

        if (auto ne = dynamic_cast<NewExpr*>(e)) return check_new(ne);

        if (auto c = dynamic_cast<Call*>(e)) return check_call(c);

        if (auto sz = dynamic_cast<SizeofExpr*>(e)) {
            TypePtr t = resolve_type(sz->spec.get(), cur_);
            if (is_void(t)) err("`" + sz->which + "(void)` is invalid", e);
            e->ty = Type::make_prim("usize");
            return e->ty;
        }

        if (auto ln = dynamic_cast<Len*>(e)) {
            TypePtr t = check_expr(ln->seq.get());
            if (is_str(t)) {
                if (ln->dim) err("`string` length has no dimension argument", ln->dim.get());
                return Type::make_prim("usize");
            }
            if (ln->dim) {
                TypePtr dt = check_expr(ln->dim.get());
                if (!is_integer(dt)) err("dimension of `len(seq, k)` must be an integer", ln->dim.get());
                auto il = dynamic_cast<IntLit*>(ln->dim.get());
                if (t && t->kind == TypeKind::Array) {
                    if (!il) err("dimension argument of a static array must be an integer literal", ln->dim.get());
                    size_t depth = 0;
                    for (TypePtr c = t; c && c->kind == TypeKind::Array; c = c->elem) depth++;
                    if (il && (il->value < 0 ||
                               static_cast<size_t>(il->value) >= depth)) {
                        err("dimension out of bounds: `" + t->str() + "` has only " + std::to_string(depth) +
                                " dimensions",
                            ln->dim.get());
                    }
                } else if (t && t->kind == TypeKind::Slice) {
                    if (il && (il->value < 0 || il->value >= t->rank)) {
                        err("dimension out of bounds: `" + t->str() + "` has only " + std::to_string(t->rank) +
                                " dimensions",
                            ln->dim.get());
                    }
                } else if (t && t->kind != TypeKind::Array) {
                    err("`len(seq, k)` can only be used on a multidimensional array", ln->dim.get());
                }
            }
            if (t && (t->kind == TypeKind::Array || t->kind == TypeKind::Slice)) {
                return Type::make_prim("usize");
            }
            err("`len` does not support " + (t ? t->str() : "void"), e);
        }

        if (dynamic_cast<RangeExpr*>(e)) err("range `a..b` can only be used in `for-in`", e);

        err("unknown expression", e);
    }

    TypePtr check_name(Name* e) {
        if (e->ident == "super") {
            err("`super` can only be used as `super(...)` or `super.method(...)`", e);
        }
        if (e->ident == "this" && lookup_local("this") == nullptr) {
            err("`this` can only appear inside a class or struct method, constructor, or destructor", e);
        }
        if (LocalVar* lv = lookup_local(e->ident)) {
            e->kind = "local";
            e->sym = lv;
            return lv->rty;
        }
        if (cur_class_) {
            if (TypePtr ft = cur_class_->field_type(e->ident)) {
                e->kind = "field";
                e->implicit_this = true;
                e->field_index = cur_class_->field_index(e->ident);
                return ft;
            }
        }
        UnitInfo& ui = info();
        if (auto g = ui.globals.find(e->ident); g != ui.globals.end()) {
            e->kind = "global";
            e->sym = g->second;
            return g->second->rty;
        }
        if (auto f = ui.funcs.find(e->ident); f != ui.funcs.end() && !f->second.empty()) {
            e->kind = "func";
            e->sym = f->second.front();
            err("function `" + e->ident + "` cannot be used as a value", e);
        }
        if (EnumConst* ec = lookup_enum_const(e->ident)) {
            e->kind = "enum_const";
            e->sym = ec;
            return ec->ty;
        }

        // 从直接 use 的 unit 导入（符号直接可见；多个来源同名时报歧义要求限定名）
        GlobalVar* imp_g = nullptr;
        std::string imp_g_unit;
        EnumConst* imp_ec = nullptr;
        std::string imp_ec_unit;
        FuncDecl* imp_f = nullptr;
        for (auto& use : cur_->uses) {
            if (!unit_visible(use->unit_name)) continue;
            auto it = infos_.find(use->unit_name);
            if (it == infos_.end()) continue;
            UnitInfo& o = it->second;
            if (auto g = o.globals.find(e->ident); g != o.globals.end() && g->second->is_public) {
                if (imp_g) {
                    err("`" + e->ident + "` is ambiguous: `" + imp_g_unit + "." + e->ident + "` and `" +
                            use->unit_name + "." + e->ident + "`; use a qualified name",
                        e);
                }
                imp_g = g->second;
                imp_g_unit = use->unit_name;
            }
            if (auto f = o.funcs.find(e->ident);
                f != o.funcs.end() && !f->second.empty() && f->second.front()->is_public) {
                if (!imp_f) imp_f = f->second.front();
            }
            if (EnumConst* ec = lookup_enum_const_in(use->unit_name, e->ident, true)) {
                if (imp_ec) {
                    err("`" + e->ident + "` is ambiguous: `" + imp_ec_unit + "." + e->ident + "` and `" +
                            use->unit_name + "." + e->ident + "`; use a qualified name",
                        e);
                }
                imp_ec = ec;
                imp_ec_unit = use->unit_name;
            }
        }
        if (imp_g) {
            e->kind = "global";
            e->sym = imp_g;
            return imp_g->rty;
        }
        if (imp_f) {
            e->kind = "func";
            e->sym = imp_f;
            err("function `" + e->ident + "` cannot be used as a value", e);
        }
        if (imp_ec) {
            e->kind = "enum_const";
            e->sym = imp_ec;
            return imp_ec->ty;
        }
        err("undefined identifier `" + e->ident + "`", e);
    }

    TypePtr check_member(Member* e) {
        Expr* obj = e->obj.get();
        if (!obj) err("missing object for member access", e);

        if (auto nm = dynamic_cast<Name*>(obj); nm && lookup_local(nm->ident) == nullptr) {
            const std::string uname = nm->ident;
            auto it = infos_.find(uname);
            if (it != infos_.end() && uname != cur_->name) {
                if (!unit_imported(uname)) err("unit `" + uname + "` is not imported with `use`", e);
                if (!unit_visible(uname)) {
                    err("unit `" + uname + "` is only visible within its library; cross-library use requires `public unit`", e);
                }
                UnitInfo& ui = it->second;
                nm->kind = "unit";
                nm->ty = Type::make_module(uname);
                if (auto f = ui.funcs.find(e->name); f != ui.funcs.end() && !f->second.empty()) {
                    e->kind = "func";
                    e->sym = f->second.front();
                    err("function `" + uname + "." + e->name + "` cannot be used as a value", e);
                }
                if (auto g = ui.globals.find(e->name); g != ui.globals.end()) {
                    if (!g->second->is_public) {
                        err("`" + uname + "." + e->name + "` is not declared public", e);
                    }
                    e->kind = "global";
                    e->sym = g->second;
                    return g->second->rty;
                }
                err("unit `" + uname + "` has no member `" + e->name + "`", e);
            }
        }

        // 枚举限定引用：`Color.Red` / `unit.Color.Red`
        {
            Expr* base = e->obj.get();
            TypePtr et;
            if (auto nm = dynamic_cast<Name*>(base)) {
                if (lookup_local(nm->ident) == nullptr &&
                    info().globals.find(nm->ident) == info().globals.end()) {
                    et = find_type_in_expr(nm->ident, e);
                    if (et) nm->ty = et;
                }
            } else if (auto inner = dynamic_cast<Member*>(base)) {
                auto un = dynamic_cast<Name*>(inner->obj.get());
                if (un && lookup_local(un->ident) == nullptr) {
                    auto it = infos_.find(un->ident);
                    if (it != infos_.end() && un->ident != cur_->name) {
                        if (!unit_imported(un->ident)) {
                            err("unit `" + un->ident + "` is not imported with `use`", e);
                        }
                        if (!unit_visible(un->ident)) {
                            err("unit `" + un->ident +
                                    "` is only visible within its library; cross-library use requires `public unit`",
                                e);
                        }
                        TypePtr t = find_type_named(un->ident, inner->name, false);
                        if (!t) {
                            err("unit `" + un->ident + "` has no type `" + inner->name + "`", e);
                        }
                        if (!type_is_public(t)) {
                            err("type `" + un->ident + "." + inner->name +
                                    "` is not declared public",
                                e);
                        }
                        et = t;
                        inner->ty = et;
                    }
                }
            }
            if (et) {
                if (et->kind != TypeKind::Enum) {
                    err("`" + et->str() + "` is not an enum and cannot be referenced as `Type.Member`", e);
                }
                EnumConst* ec = find_enum_member(et, e->name);
                if (!ec) err("enum `" + et->str() + "` has no member `" + e->name + "`", e);
                e->kind = "enum_const";
                e->sym = ec;
                return et;
            }
        }

        TypePtr ot = check_expr(obj);
        if (ot && (ot->kind == TypeKind::Struct || ot->kind == TypeKind::Class)) {
            TypePtr ft = ot->field_type(e->name);
            if (!ft) {
                if (ot->kind == TypeKind::Class && has_method_named(ot, e->name)) {
                    err("method `" + e->name + "` must be called (`x." + e->name + "(...)`)", e);
                }
                err("type `" + ot->str() + "` has no field `" + e->name + "`", e);
            }
            for (auto& f : ot->fields) {
                if (f.name != e->name) continue;
                if (!f.is_public && (!cur_ || cur_->name != ot->unit)) {
                    err("field `" + e->name + "` is not declared public (type `" + ot->str() + "`)", e);
                }
                break;
            }
            e->kind = "field";
            e->field_index = ot->field_index(e->name);
            return ft;
        }
        err("type " + (ot ? ot->str() : "void") + " has no member `" + e->name + "`", e);
    }

    TypePtr check_unary(Unary* e) {
        if (e->op == "&") {
            if (auto nm = dynamic_cast<Name*>(e->operand.get());
                nm && nm->ident == "this" && lookup_local("this") != nullptr) {
                err("cannot take the address of `this`: it is a reference to the current object", e);
            }
            if (!is_lvalue(e->operand.get())) err("`&` requires an addressable object", e);
            TypePtr t = check_expr(e->operand.get());
            if (auto m = dynamic_cast<Member*>(e->operand.get()); m && m->kind == "enum_const") {
                err("enum members are not addressable", e);
            }
            // U21：RC 生命周期无法用裸指针表达——禁止对 class/interface 句柄、
            // 以及经引用访问的成员/数组元素取地址（`&c`、`&c.x`、`&h.items[0]`、`&catArray[i]`）。
            if (is_reference_type(t)) {
                err("cannot take the address of a class or interface reference (`" + t->str() +
                        "`): its RC lifetime cannot be expressed with a raw pointer",
                    e);
            }
            if (rooted_at_reference(e->operand.get())) {
                err("cannot take the address of a member or element reached through a class or interface reference: copy it to a local, "
                    "use it, then assign it back (backlog U21)",
                    e);
            }
            // U24：含引用字段的 struct 由编译器逐字段 RC；裸指针可绕过 retain/release
            // （也能把弱引用/字段地址带到生命周期之外），第一阶段禁止取址。
            if (t && t->kind == TypeKind::Struct && has_ref_fields(t) && !t->has_dtor) {
                err("cannot take the address of struct `" + t->str() +
                        "` which has reference fields: a raw pointer could bypass RC (backlog U24); pass or return it by value, or use a class"
                        " (structs with a user-defined destructor are still used through pointers and are not affected)",
                    e);
            }
            return Type::make_ptr(t);
        }

        TypePtr t = check_expr(e->operand.get());

        // 枚举不是算术类型：一元运算（`~`/`-`/`+`）需要显式 `as int`
        if (is_enum_t(t) && (e->op == "~" || e->op == "-" || e->op == "+")) {
            err("enum `" + t->str() + "` does not support unary `" + e->op + "` (write `x as int`)", e);
        }

        if (e->op == "*") {
            if (t && t->kind == TypeKind::Ptr) return t->elem;
            err("`*` requires a pointer, found " + (t ? t->str() : "void"), e);
        }
        if (e->op == "~") {
            if (is_integer(t)) return t;
            if (is_user_type(t)) {
                auto cands = collect_operators("~", {t});
                std::vector<FuncDecl*> unary;
                for (auto* f : cands) {
                    if (f->pty.size() == 1) unary.push_back(f);
                }
                if (unary.empty()) {
                    err("type `" + t->str() + "` has no unary `~` operator overload", e);
                }
                auto f = choose_overload(unary, {e->operand.get()}, "operator~", e);
                e->op_func = f;
                return f->rty;
            }
            const std::string hint = (is_prim(t) && t->name == "bool")
                                         ? " (use `not` for logical negation of `bool`)"
                                         : "";
            err("`~` is bitwise NOT and requires an integer" + hint, e);
        }
        if (e->op == "-" || e->op == "+") {
            if (is_numeric(t)) return t;
            if (is_user_type(t)) {
                auto cands = collect_operators(e->op, {t});
                std::vector<FuncDecl*> unary;
                for (auto* f : cands) {
                    if (f->pty.size() == 1) unary.push_back(f);
                }
                if (unary.empty()) {
                    err("type `" + t->str() + "` has no unary `" + e->op + "` operator overload", e);
                }
                auto f = choose_overload(unary, {e->operand.get()}, "operator" + e->op, e);
                e->op_func = f;
                return f->rty;
            }
            err("`" + e->op + "` requires a numeric operand, found " + (t ? t->str() : "void"), e);
        }
        err("unknown unary operator `" + e->op + "`", e);
    }

    // 条件表达式 `cond ? a : b`（SPEC §5.2）：
    // 条件必须是 bool；两侧类型取「同一类型 → 隐式加宽 → null 适配」，都不成立则要求显式 `as`。
    TypePtr check_ternary(Ternary* e, const TypePtr& expected = nullptr) {
        TypePtr ct = check_expr(e->cond.get());
        if (!(is_prim(ct) && ct->name == "bool")) {
            err("the condition of `?:` must be `bool`, found " + (ct ? ct->str() : "void"),
                e->cond.get());
        }
        // 期望类型透传给两个分支：字面量按上下文适配（`char c = pick ? 'a' : 'b';`）
        TypePtr a = check_expr(e->then_e.get(), expected);
        TypePtr b = check_expr(e->else_e.get(), expected);
        if (!a || !b) err("`?:` is missing a branch expression", e);
        adapt_lit(e->then_e.get(), a, b);
        adapt_lit(e->else_e.get(), b, a);
        a = e->then_e->ty;
        b = e->else_e->ty;

        // null 字面量适配到另一侧（指针 / class / interface / 动态数组）
        auto null_target = [](const TypePtr& t) {
            return t && (t->kind == TypeKind::Ptr || t->kind == TypeKind::Class ||
                         t->kind == TypeKind::Interface || t->kind == TypeKind::Slice);
        };
        if (a->kind == TypeKind::Null && b->kind != TypeKind::Null && null_target(b)) {
            e->then_e->ty = b;
            a = b;
        } else if (b->kind == TypeKind::Null && a->kind != TypeKind::Null && null_target(a)) {
            e->else_e->ty = a;
            b = a;
        }

        if (same_type(a, b)) return a;
        if (implicit_ok(a, b)) return b;
        if (implicit_ok(b, a)) return a;
        err("the branches of `?:` have different types (" + a->str() + " and " + b->str() +
                "); cast one of them explicitly with `as`",
            e);
    }

    TypePtr check_binary(Binary* e) {
        const std::string& op = e->op;

        if (op == "and" || op == "or") {
            check_cond_type(e->left.get(), "`and`/`or`");
            check_cond_type(e->right.get(), "`and`/`or`");
            return t_bool();
        }

        TypePtr lt = check_expr(e->left.get());
        TypePtr rt = check_expr(e->right.get());
        adapt_lit(e->left.get(), lt, rt);
        adapt_lit(e->right.get(), rt, lt);
        lt = e->left->ty;
        rt = e->right->ty;
        if (!lt || !rt) err("binary operator is missing an operand", e);

        if (is_user_type(lt) || is_user_type(rt)) {
            if (op != "==" && op != "!=") {
                auto cands = collect_operators(op, {lt, rt});
                if (cands.empty()) {
                    err("type `" + lt->str() + "` and `" + rt->str() + "` has no `" + op +
                            "` operator overload",
                        e);
                }
                auto f = choose_overload(cands, {e->left.get(), e->right.get()},
                                         "operator" + op, e);
                e->op_func = f;
                return f->rty;
            }
        }

        // 枚举是名义类型：只与同一枚举类型比较；与整数之间没有隐式转换（双向都要 `as`）
        if (is_enum_t(lt) || is_enum_t(rt)) {
            if (!(is_enum_t(lt) && is_enum_t(rt)) || !same_type(lt, rt)) {
                const TypePtr& en = is_enum_t(lt) ? lt : rt;
                err("enum `" + en->str() + "` can only be compared or combined with values of the same enum type; there is no implicit" +
                        " conversion between enums and integers (write `as`)",
                    e);
            }
            if (op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" &&
                op != ">=") {
                err("enum `" + lt->str() + "` only supports == != < <= > >= (cast to `int` first for arithmetic or bitwise operations)", e);
            }
            return t_bool();
        }

        if (op == "+" || op == "-") {
            if (lt->kind == TypeKind::Ptr && is_integer(rt)) {
                if (lt->elem && lt->elem->kind == TypeKind::Void)
                    err("pointer arithmetic is not allowed on `void*`", e);
                return lt;
            }
            if (op == "+" && is_integer(lt) && rt->kind == TypeKind::Ptr) {
                if (rt->elem && rt->elem->kind == TypeKind::Void)
                    err("pointer arithmetic is not allowed on `void*`", e);
                return rt;
            }
            if (op == "-" && lt->kind == TypeKind::Ptr && rt->kind == TypeKind::Ptr) {
                if (!same_type(lt->elem, rt->elem)) err("pointer subtraction requires the same element type", e);
                if (lt->elem && lt->elem->kind == TypeKind::Void)
                    err("pointer arithmetic is not allowed on `void*`", e);
                return Type::make_prim("isize");
            }
            TypePtr res = usual_arith(lt, rt);
            if (!res) err("`" + op + "` is not supported for " + lt->str() + " and " + rt->str(), e);
            return res;
        }

        if (op == "*" || op == "/" || op == "%") {
            TypePtr res = usual_arith(lt, rt);
            if (!res) err("`" + op + "` is not supported for " + lt->str() + " and " + rt->str(), e);
            return res;
        }

        if (op == "<<" || op == ">>") {
            if (!is_integer(lt) || !is_integer(rt)) err("`both sides of `" + op + "` must be integers", e);
            return lt;
        }

        if (op == "&" || op == "|" || op == "^") {
            TypePtr res = usual_arith(lt, rt);
            if (!res || !is_integer(res)) err("`both sides of `" + op + "` must be integers", e);
            return res;
        }

        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (lt->kind == TypeKind::Ptr && rt->kind == TypeKind::Ptr) return t_bool();
            if (is_numeric(lt) && is_numeric(rt)) return t_bool();
            err("`" + op + "` is not supported for " + lt->str() + " and " + rt->str(), e);
        }

        if (op == "==" || op == "!=") {
            if (lt->kind == TypeKind::Null || rt->kind == TypeKind::Null) {
                const TypePtr& other = (lt->kind == TypeKind::Null) ? rt : lt;
                if (is_str(other)) {
                    err("`string` cannot be compared with `null` (use `len(s) == 0` or `s == \"\"`)", e);
                }
                if (other->kind == TypeKind::Slice) {
                    err("slice cannot be compared with `null` (use `len(s) == 0`)", e);
                }
                if (other->kind == TypeKind::Ptr || other->kind == TypeKind::Slice ||
                    other->kind == TypeKind::Null || other->kind == TypeKind::Class ||
                    other->kind == TypeKind::Interface) {
                    return t_bool();
                }
            }
            if (is_str(lt) && is_str(rt)) return t_bool();
            if (is_numeric(lt) && is_numeric(rt)) return t_bool();
            const bool eq = same_type(lt, rt);
            if ((eq && (lt->kind == TypeKind::Ptr || lt->kind == TypeKind::Enum ||
                        lt->kind == TypeKind::Class || lt->kind == TypeKind::Interface)) ||
                (is_prim(lt) && lt->name == "bool" && same_type(rt, lt))) {
                return t_bool();
            }
            // 引用类型之间：可相互转换（基类/接口）即可做身份比较
            if (is_reference_type(lt) && is_reference_type(rt) &&
                (implicitly_convertible(lt, rt) || implicitly_convertible(rt, lt))) {
                return t_bool();
            }
            if (is_user_type(lt) || is_user_type(rt)) {
                auto cands = collect_operators(op, {lt, rt});
                if (cands.empty() && op == "!=") {
                    auto eqs = collect_operators("==", {lt, rt});
                    if (!eqs.empty()) {
                        choose_overload(eqs, {e->left.get(), e->right.get()}, "operator==", e);
                        return t_bool();
                    }
                }
                if (cands.empty()) {
                    err("type `" + lt->str() + "` and `" + rt->str() + "` has no `" + op +
                            "` operator overload",
                        e);
                }
                auto f = choose_overload(cands, {e->left.get(), e->right.get()},
                                         "operator" + op, e);
                e->op_func = f;
                return f->rty;
            }
            err("`" + op + "` is not supported for " + lt->str() + " and " + rt->str(), e);
        }

        err("unknown binary operator `" + op + "`", e);
    }

    TypePtr check_cond_type(Expr* e, const std::string& what) {
        TypePtr t = check_expr(e);
        if (!(is_prim(t) && t->name == "bool")) {
            err(what + " must be `bool`, found " + (t ? t->str() : "void"), e);
        }
        return t;
    }

    void adapt_lit(Expr* e, const TypePtr& self_ty, const TypePtr& other) {
        (void)self_ty;
        if (auto cl = dynamic_cast<CharLit*>(e)) {
            // 字符字面量是字节：按对端的 8 位整数类型适配（char/byte）
            if (is_prim(other) && other->is_integer() && other->bits() == 8) {
                cl->ty = other;
            }
            return;
        }
        auto il = dynamic_cast<IntLit*>(e);
        if (!il) return;
        if (is_prim(other) && other->is_numeric() && promote_int_literal(il->value, other)) {
            il->ty = other;
        }
    }

    bool can_cast(const TypePtr& src, const TypePtr& dst) {
        if (!src || !dst) return false;
        if (same_type(src, dst)) return true;
        // 枚举是名义类型：只与整数（非枚举）显式互转，且不与浮点互换
        if (src->kind == TypeKind::Enum || dst->kind == TypeKind::Enum) {
            if (src->kind == TypeKind::Enum && dst->kind == TypeKind::Enum) return false;
            return src->kind == TypeKind::Enum ? is_plain_int(dst) : is_plain_int(src);
        }
        if (is_numeric(src) && is_numeric(dst)) return true;
        // 引用类型：向上/向下转换（向下在运行期检查，失败得 null）
        if (is_class_type(src) && is_class_type(dst)) {
            return is_subclass_of(src, dst) || is_subclass_of(dst, src);
        }
        if (is_interface_type(src) && is_interface_type(dst)) {
            return implements_interface(src, dst);  // 仅向上；向下暂缓
        }
        if (is_interface_type(src) && is_class_type(dst)) return true;
        if (src->kind == TypeKind::Ptr && dst->kind == TypeKind::Ptr) return true;
        if (src->kind == TypeKind::Slice && dst->kind == TypeKind::Slice) {
            return src->rank == dst->rank && same_type(src->elem, dst->elem);
        }
        if (src->kind == TypeKind::Ptr && is_integer(dst) && dst->bits() >= 64) return true;
        if (dst->kind == TypeKind::Ptr && is_integer(src) && src->bits() >= 64) return true;
        if (is_str(src) && dst->kind == TypeKind::Ptr && dst->elem &&
            dst->elem->is_const &&
            dst->elem->kind == TypeKind::Prim && dst->elem->bits() == 8) {
            return true;  // Str -> readonly char*/byte*（要求 NUL 结尾）
        }
        return false;
    }

    // ------------------------------------------------------------ 调用
    TypePtr check_call(Call* c) {
        Expr* callee = c->callee.get();
        if (!callee) err("call is missing a callee", c);

        if (auto nm = dynamic_cast<Name*>(callee)) {
            if (nm->ident == "alloc" || nm->ident == "realloc" || nm->ident == "free") {
                return check_builtin(c, nm->ident);
            }
            if (nm->ident == "super") return check_super_call(c);
            // 类内部对自身方法的非限定调用
            if (lookup_local(nm->ident) == nullptr && cur_class_) {
                std::vector<const MethodSig*> ms;
                collect_methods(cur_class_, nm->ident, ms);
                if (!ms.empty()) {
                    auto sig = choose_method(ms, raw_args(c->args), "method `" + nm->ident + "`", c);
                    check_method_args(c, sig, "method `" + nm->ident + "`");
                    c->callee_kind =
                        cur_class_->kind == TypeKind::Struct ? "struct_method" : "method";
                    c->method = sig;
                    return sig->ret;
                }
            }
        }

        FuncDecl* target = nullptr;
        TypePtr struct_ty = nullptr;

        if (auto nm = dynamic_cast<Name*>(callee)) {
            if (lookup_local(nm->ident) != nullptr) {
                err("`" + nm->ident + "` is a variable, not a function", c);
            }
            // 候选：当前 unit + 直接 use 的 unit（只收 public）+ std.string（隐式）
            std::vector<FuncDecl*> cands;
            auto add_funcs = [&](UnitInfo& o, bool require_pub) {
                auto f = o.funcs.find(nm->ident);
                if (f == o.funcs.end()) return;
                for (auto* fd : f->second) {
                    if (!require_pub || fd->is_public) cands.push_back(fd);
                }
            };
            UnitInfo& ui = info();
            add_funcs(ui, false);
            // 本地有同名函数时完全遮蔽导入（与本地类型/变量一致）
            if (cands.empty()) {
                for (auto& use : cur_->uses) {
                    if (use->unit_name == "std.string") continue;  // 下面统一按隐式导入加入
                    if (!unit_visible(use->unit_name)) continue;
                    auto it = infos_.find(use->unit_name);
                    if (it != infos_.end()) add_funcs(it->second, true);
                }
                if (UnitInfo* stdn = std_info(); stdn && cur_->name != "std.string") {
                    add_funcs(*stdn, true);
                }
            }

            if (!cands.empty()) {
                FuncDecl* chosen = choose_overload(cands, raw_args(c->args),
                                                   "function `" + nm->ident + "`", c);
                nm->kind = "func";
                nm->sym = chosen;
                target = chosen;
            } else {
                // struct 构造：当前 unit 优先，再找 use 的 unit 的 public 类型
                TypePtr st = find_type_named(cur_->name, nm->ident, false);
                if (!st) {
                    std::string src;
                    for (auto& use : cur_->uses) {
                        if (!unit_visible(use->unit_name)) continue;
                        TypePtr t = find_type_named(use->unit_name, nm->ident, true);
                        if (!t) continue;
                        if (st) {
                            err("type `" + nm->ident + "` is ambiguous: `" + src + "` and `" +
                                    use->unit_name + "`; use a qualified name",
                                c);
                        }
                        st = t;
                        src = use->unit_name;
                    }
                }
                if (!st && std_info()) st = find_type_named("std.string", nm->ident, true);
                if (st) {
                    if (st->kind != TypeKind::Struct) {
                        err("`" + nm->ident + "` is not a struct and cannot be constructed by value", c);
                    }
                    struct_ty = st;
                } else if (lookup_enum_const(nm->ident) != nullptr) {
                    err("`" + nm->ident + "` is an enum member and cannot be called", c);
                } else {
                    err("undefined function `" + nm->ident + "`", c);
                }
            }
        } else if (auto m = dynamic_cast<Member*>(callee)) {
            Expr* obj = m->obj.get();
            // super.method(...)
            if (auto on = dynamic_cast<Name*>(obj);
                on && on->ident == "super" && lookup_local("super") == nullptr) {
                if (!cur_class_ || !cur_class_->base) err("the current class has no base class", c);
                std::vector<const MethodSig*> ms;
                collect_class_methods(cur_class_->base, m->name, ms);
                if (ms.empty()) err("base class has no method `" + m->name + "`", c);
                auto sig = choose_method(ms, raw_args(c->args), "method `" + m->name + "`", c);
                if (sig->is_abstract) {
                    err("abstract method `" + m->name + "` cannot be called via `super`", c);
                }
                if (!decl_visible(sig->owner ? sig->owner->unit : std::string(), sig->is_public)) {
                    err("method `" + m->name + "` is not declared public and cannot be called from another unit", c);
                }
                check_method_args(c, sig, "method `" + m->name + "`");
                m->kind = "method";
                c->callee_kind = "super";
                c->method = sig;
                return sig->ret;
            }

            std::string uname;
            if (auto nm = dynamic_cast<Name*>(obj)) {
                if (lookup_local(nm->ident) == nullptr && infos_.count(nm->ident)) {
                    uname = nm->ident;
                }
            }
            if (uname.empty()) {
                TypePtr ot = check_expr(obj);
                if (is_reference_type(ot)) {
                    std::vector<const MethodSig*> ms;
                    collect_methods(ot, m->name, ms);
                    if (ms.empty()) {
                        err("type `" + ot->str() + "` has no method `" + m->name + "`", c);
                    }
                    auto sig = choose_method(ms, raw_args(c->args), "method `" + m->name + "`", c);
                    if (!decl_visible(sig->owner ? sig->owner->unit : std::string(), sig->is_public)) {
                        err("method `" + m->name + "` is not declared public and cannot be called from another unit", c);
                    }
                    check_method_args(c, sig, "method `" + m->name + "`");
                    m->kind = "method";
                    c->callee_kind = "method";
                    c->method = sig;
                    return sig->ret;
                }
                if (ot && ot->kind == TypeKind::Struct) {
                    std::vector<const MethodSig*> ms;
                    collect_methods(ot, m->name, ms);
                    if (ms.empty()) {
                        err("struct `" + ot->str() + "` has no method `" + m->name + "`", c);
                    }
                    auto sig = choose_method(ms, raw_args(c->args), "method `" + m->name + "`", c);
                    if (!decl_visible(sig->owner ? sig->owner->unit : std::string(), sig->is_public)) {
                        err("method `" + m->name + "` is not declared public and cannot be called from another unit", c);
                    }
                    check_method_args(c, sig, "method `" + m->name + "`");
                    m->kind = "method";
                    c->callee_kind = "struct_method";
                    c->method = sig;
                    return sig->ret;
                }
                err("`" + m->name + "` is not a function", c);
            }
            if (!unit_imported(uname)) err("unit `" + uname + "` is not imported with `use`", c);
            if (!unit_visible(uname)) {
                err("unit `" + uname + "` is only visible within its library; cross-library use requires `public unit`", c);
            }

            UnitInfo& ui = infos_.at(uname);
            auto f = ui.funcs.find(m->name);
            if (f == ui.funcs.end() || f->second.empty()) {
                err("unit `" + uname + "` has no function `" + m->name + "`", c);
            }
            FuncDecl* chosen = choose_overload(f->second, raw_args(c->args),
                                               "function `" + uname + "." + m->name + "`", c);
            if (!chosen->is_public) {
                err("function `" + uname + "." + m->name + "` is not declared public", c);
            }
            auto nm = dynamic_cast<Name*>(obj);
            nm->kind = "unit";
            nm->ty = Type::make_module(uname);
            m->kind = "func";
            m->sym = chosen;
            target = chosen;
        } else {
            err("only functions and struct constructors can be called", c);
        }

        if (struct_ty) {
            if (struct_ty->is_union) {
                err("union `" + struct_ty->str() + "` cannot be constructed with `T(...)`; use `{ member = value }`", c);
            }
            if (!struct_ty->ctors.empty()) {
                if (!c->named.empty()) err("struct constructors do not support named arguments", c);
                if (struct_ty->has_dtor && c != direct_init_expr_) {
                    err("a struct with a destructor can only be constructed in a declaration initializer", c);
                }
                const auto* chosen = choose_ctor(struct_ty->ctors, raw_args(c->args),
                                                 "struct `" + struct_ty->str() + "`", c);
                const size_t ci = static_cast<size_t>(chosen - struct_ty->ctors.data());
                if (ci < struct_ty->ctor_is_public.size() && !struct_ty->ctor_is_public[ci] &&
                    (!cur_ || cur_->name != struct_ty->unit)) {
                    err("constructor is not declared public and cannot be called from another unit", c);
                }
                for (size_t i = 0; i < c->args.size(); ++i) {
                    check_assign(c->args[i].get(), (*chosen)[i], "constructor argument");
                }
                c->callee_kind = "struct_ctor";
                c->ctor = struct_ty;
                c->ctor_pty = chosen;
                return struct_ty;
            }
            c->callee_kind = "ctor";
            c->ctor = struct_ty;
            check_ctor_args(c, struct_ty);
            return struct_ty;
        }

        if (!target) err("only functions and struct constructors can be called", c);
        c->callee_kind = "func";
        c->func = target;
        const auto& params = target->pty;

        if (!c->named.empty()) {
            err("function calls do not support named arguments (only struct construction does)", c);
        }
        if (c->args.size() != params.size()) {
            err("function `" + target->name + "` expects " + std::to_string(params.size()) +
                    " arguments, but " + std::to_string(c->args.size()) + " were given",
                c);
        }
        for (size_t i = 0; i < c->args.size(); ++i) {
            check_assign(c->args[i].get(), params[i], "argument of call to `" + target->name + "`");
        }
        return target->rty;
    }

    int rank_arg(Expr* e, const TypePtr& aty, const TypePtr& p) {
        if (!p || !aty) return -1;
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (same_type(aty, p)) return 0;
            if (is_numeric(p) && promote_int_literal(il->value, p)) return 1;
            return -1;
        }
        if (auto fl = dynamic_cast<FloatLit*>(e)) {
            (void)fl;
            if (same_type(aty, p)) return 0;
            if (p->kind == TypeKind::Prim && (p->name == "float" || p->name == "double")) return 1;
            return -1;
        }
        if (dynamic_cast<NullLit*>(e)) {
            if (p->kind == TypeKind::Ptr || p->kind == TypeKind::Slice ||
                p->kind == TypeKind::Class || p->kind == TypeKind::Interface) {
                return 0;
            }
            return -1;
        }
        if (same_type(aty, p)) return 0;
        if (implicit_ok(aty, p)) return 1;
        return -1;
    }

    static bool dominates(const std::vector<int>& a, const std::vector<int>& b) {
        bool strict = false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] > b[i]) return false;
            if (a[i] < b[i]) strict = true;
        }
        return strict;
    }

    static bool is_user_type(const TypePtr& t) {
        return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Class ||
                     t->kind == TypeKind::Interface);
    }

    // 是否含 RC 管理的引用字段（class/interface/切片/string/数组递归）；
    // 只看字段，不看用户析构器（含析构器的 struct 仍允许取址，通过指针使用）。
    static bool has_ref_fields(const TypePtr& t) {
        if (!t) return false;
        switch (t->kind) {
            case TypeKind::Class:
            case TypeKind::Interface:
            case TypeKind::Slice: return true;
            case TypeKind::Array: return has_ref_fields(t->elem);
            case TypeKind::Struct:
                for (const auto& f : t->fields) {
                    if (f.weak) continue;  // weak 不持有引用
                    if (has_ref_fields(f.type)) return true;
                }
                return false;
            default: return false;
        }
    }

    // 运算符查找范围：当前 unit + use 引入的 unit + 操作数类型所属 unit（ADL 风格）。
    // 未导入 unit 的运算符不可见，避免无关 unit 之间意外冲突。
    std::vector<FuncDecl*> collect_operators(const std::string& sym,
                                             const std::vector<TypePtr>& operands = {}) {
        std::unordered_set<std::string> scope;
        if (cur_) {
            scope.insert(cur_->name);
            for (auto& u : cur_->uses) scope.insert(u->unit_name);
        }
        for (auto& t : operands) {
            if (t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Class ||
                      t->kind == TypeKind::Enum || t->kind == TypeKind::Interface)) {
                if (!t->unit.empty()) scope.insert(t->unit);
            }
        }

        std::vector<FuncDecl*> out;
        const std::string name = "operator" + sym;
        for (auto& entry : infos_) {
            if (!scope.empty() && !scope.count(entry.first)) continue;
            auto it = entry.second.funcs.find(name);
            if (it != entry.second.funcs.end()) {
                out.insert(out.end(), it->second.begin(), it->second.end());
            }
        }
        return out;
    }

    static std::vector<Expr*> raw_args(const std::vector<ExprPtr>& v) {
        std::vector<Expr*> out;
        out.reserve(v.size());
        for (auto& e : v) out.push_back(e.get());
        return out;
    }

    FuncDecl* choose_overload(const std::vector<FuncDecl*>& cands,
                              const std::vector<Expr*>& args, const std::string& what,
                              Node* at) {
        std::vector<TypePtr> atys;
        for (auto* a : args) atys.push_back(check_expr(a));

        std::vector<FuncDecl*> ok;
        std::vector<std::vector<int>> ranks;
        for (auto* f : cands) {
            if (f->pty.size() != args.size()) continue;
            std::vector<int> r;
            bool good = true;
            for (size_t i = 0; i < args.size(); ++i) {
                int k = rank_arg(args[i], atys[i], f->pty[i]);
                if (k < 0) { good = false; break; }
                r.push_back(k);
            }
            if (good) { ok.push_back(f); ranks.push_back(std::move(r)); }
        }
        if (ok.empty()) {
            if (cands.size() == 1 && cands[0]->pty.size() == args.size()) {
                for (size_t i = 0; i < args.size(); ++i) {
                    check_assign(args[i], cands[0]->pty[i], what + " argument");
                }
            }
            err("no matching overload of " + what, at);
        }

        int best = -1;
        for (size_t i = 0; i < ok.size(); ++i) {
            bool unique = true;
            for (size_t j = 0; j < ok.size(); ++j) {
                if (i == j) continue;
                if (dominates(ranks[j], ranks[i]) || ranks[j] == ranks[i]) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                if (best >= 0) { best = -1; break; }
                best = static_cast<int>(i);
            }
        }
        if (best < 0) err("ambiguous overload call of `" + what + "`", at);
        return ok[static_cast<size_t>(best)];
    }

    // 只收集 class 链上的方法（接口默认方法不参与 super 调用）
    void collect_class_methods(const TypePtr& t, const std::string& name,
                               std::vector<const MethodSig*>& out, int depth = 0) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (m.name != name) continue;
            bool dup = false;
            for (auto* e : out) {
                if (same_sig(*e, m)) { dup = true; break; }
            }
            if (!dup) out.push_back(&m);
        }
        if (t->kind == TypeKind::Class && t->base) {
            collect_class_methods(t->base, name, out, depth + 1);
        }
    }

    void collect_methods(const TypePtr& t, const std::string& name,
                         std::vector<const MethodSig*>& out, int depth = 0) {
        if (!t || depth > 256) return;
        for (auto& m : t->methods) {
            if (m.name != name) continue;
            bool dup = false;
            for (auto* e : out) {
                if (same_sig(*e, m)) { dup = true; break; }
            }
            if (!dup) out.push_back(&m);  // 派生类/先访问者优先，覆盖基类同签名条目
        }
        if (t->kind == TypeKind::Class && t->base) {
            collect_methods(t->base, name, out, depth + 1);
        }
        for (auto& i : t->interfaces) {
            collect_methods(i, name, out, depth + 1);
        }
    }

    const MethodSig* choose_method(const std::vector<const MethodSig*>& cands,
                                   const std::vector<Expr*>& args,
                                   const std::string& what, Node* at) {
        std::vector<TypePtr> atys;
        for (auto* a : args) atys.push_back(check_expr(a));

        std::vector<const MethodSig*> ok;
        std::vector<std::vector<int>> ranks;
        for (auto* sig : cands) {
            if (sig->params.size() != args.size()) continue;
            std::vector<int> r;
            bool good = true;
            for (size_t i = 0; i < args.size(); ++i) {
                int k = rank_arg(args[i], atys[i], sig->params[i]);
                if (k < 0) { good = false; break; }
                r.push_back(k);
            }
            if (good) { ok.push_back(sig); ranks.push_back(std::move(r)); }
        }
        if (ok.empty()) err("no matching overload of " + what, at);

        int best = -1;
        for (size_t i = 0; i < ok.size(); ++i) {
            bool unique = true;
            for (size_t j = 0; j < ok.size(); ++j) {
                if (i == j) continue;
                if (dominates(ranks[j], ranks[i]) || ranks[j] == ranks[i]) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                if (best >= 0) { best = -1; break; }
                best = static_cast<int>(i);
            }
        }
        if (best < 0) err("ambiguous overload call of `" + what + "`", at);
        return ok[static_cast<size_t>(best)];
    }

    const std::vector<TypePtr>* choose_ctor(const std::vector<std::vector<TypePtr>>& ctors,
                                            const std::vector<Expr*>& args,
                                            const std::string& what, Node* at) {
        std::vector<TypePtr> atys;
        for (auto* a : args) atys.push_back(check_expr(a));

        std::vector<const std::vector<TypePtr>*> ok;
        std::vector<std::vector<int>> ranks;
        for (auto& cs : ctors) {
            if (cs.size() != args.size()) continue;
            std::vector<int> r;
            bool good = true;
            for (size_t i = 0; i < args.size(); ++i) {
                int k = rank_arg(args[i], atys[i], cs[i]);
                if (k < 0) { good = false; break; }
                r.push_back(k);
            }
            if (good) { ok.push_back(&cs); ranks.push_back(std::move(r)); }
        }
        if (ok.empty()) err("no matching constructor for " + what, at);

        int best = -1;
        for (size_t i = 0; i < ok.size(); ++i) {
            bool unique = true;
            for (size_t j = 0; j < ok.size(); ++j) {
                if (i == j) continue;
                if (dominates(ranks[j], ranks[i]) || ranks[j] == ranks[i]) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                if (best >= 0) { best = -1; break; }
                best = static_cast<int>(i);
            }
        }
        if (best < 0) err("ambiguous constructor call of `" + what + "`", at);
        return ok[static_cast<size_t>(best)];
    }

    void check_method_args(Call* c, const MethodSig* sig, const std::string& what) {
        if (!c->named.empty()) err("method calls do not support named arguments", c);
        if (c->args.size() != sig->params.size()) {
            err(what + " expects " + std::to_string(sig->params.size()) + " arguments, but " +
                    std::to_string(c->args.size()) + " were given",
                c);
        }
        for (size_t i = 0; i < c->args.size(); ++i) {
            check_assign(c->args[i].get(), sig->params[i], what + " argument");
        }
    }

    TypePtr check_super_call(Call* c) {
        if (!in_ctor_ || !super_ok_) {
            err("`super(...)` must be the first statement of a constructor", c);
        }
        if (!cur_class_ || !cur_class_->base) err("the current class has no base class", c);
        if (!c->named.empty()) err("constructor calls do not support named arguments", c);
        const auto& ctors = cur_class_->base->ctors;
        if (ctors.empty()) {
            if (!c->args.empty()) err("base class has no matching constructor", c);
        } else {
            const auto* chosen = choose_ctor(ctors, raw_args(c->args), "base class", c);
            for (size_t i = 0; i < c->args.size(); ++i) {
                check_assign(c->args[i].get(), (*chosen)[i], "super argument");
            }
            c->ctor_pty = chosen;
        }
        c->callee_kind = "super";
        return t_void();
    }

    [[noreturn]] void err_slice_elem_align(const TypePtr& elem, const Node* node) const {
        err("type `" + elem->str() + "` has align(" + std::to_string(type_align(elem)) +
                ") greater than 8: the data area of a dynamic array follows a 24-byte header, so elements can be aligned to at most 8 bytes; "
                "use a fixed-size array or a pointer instead, or lower `align(N)`",
            node);
    }

    TypePtr check_new(NewExpr* ne) {
        TypePtr t = resolve_type(ne->type.get(), cur_);
        if (ne->is_array) {
            if (is_void(t)) err("dynamic array element cannot be void", ne);
            if (ne->dims.empty()) err("dynamic array is missing its length", ne);
            for (auto& d : ne->dims) {
                TypePtr ct = check_expr(d.get());
                if (!(ct && ct->is_integer())) {
                    err("dynamic array length must be an integer", d.get());
                }
            }
            if (t->kind == TypeKind::Struct && t->has_dtor) {
                err("struct `" + t->str() +
                        "` has a destructor and cannot be a dynamic array element (element construction and destruction are not implemented)",
                    ne);
            }
            if (!ne->args.empty() || !ne->named.empty()) {
                err("dynamic arrays cannot take constructor arguments", ne);
            }
            ne->class_ty = t;
            if (ne->jagged_rank > 0) {
                if (ne->dims.size() != 1) {
                    err("a jagged array `new T[m][]` creates one dimension at a time; "
                        "fill rows in a loop for two dimensions, or use `new T[m,n]` for a rectangular array",
                        ne);
                }
                TypePtr row = t;
                for (int i = 0; i < ne->jagged_rank; ++i) row = Type::make_slice(row);
                ne->elem_ty = row;  // 外层数组的元素：T[]/T[][]/...
                return Type::make_slice(row);
            }
            if (type_align(t) > 8) err_slice_elem_align(t, ne);
            ne->elem_ty = t;
            return Type::make_slice(t, false, static_cast<int>(ne->dims.size()));
        }
        if (t->kind != TypeKind::Class) err("`new` can only be used with a class or a dynamic array", ne);
        if (t->is_abstract) err("abstract class `" + t->str() + "` cannot be instantiated with `new`", ne);
        if (!ne->named.empty()) err("constructors do not support named arguments yet", ne);

        const auto& ctors = t->ctors;
        ne->has_ctor = !ctors.empty();
        if (ctors.empty()) {
            if (!ne->args.empty()) err("class `" + t->str() + "` has no matching constructor", ne);
        } else {
            const auto* chosen = choose_ctor(ctors, raw_args(ne->args), "class `" + t->str() + "`", ne);
            const size_t idx = static_cast<size_t>(chosen - ctors.data());
            if (idx < t->ctor_is_public.size() && !t->ctor_is_public[idx] &&
                (!cur_ || cur_->name != t->unit)) {
                err("constructor is not declared public and cannot be invoked with `new` from another unit", ne);
            }
            for (size_t i = 0; i < ne->args.size(); ++i) {
                check_assign(ne->args[i].get(), (*chosen)[i], "constructor argument");
            }
            ne->ctor_pty = *chosen;
        }
        ne->class_ty = t;
        return t;
    }

    void check_ctor_args(Call* c, const TypePtr& ty) {
        const auto& fields = ty->fields;

        // 跨 unit 的 `S(...)` 构造要求字段全部 public
        if (cur_ && cur_->name != ty->unit) {
            for (auto& f : fields) {
                if (!f.is_public) {
                    err("field `" + f.name + "` of struct `" + ty->str() +
                            "` is not declared public and cannot be constructed from another unit",
                        c);
                }
            }
        }

        if (c->args.empty() && c->named.empty()) {
            c->field_order.clear();
            return;
        }

        if (!c->named.empty()) {
            std::unordered_map<std::string, size_t> fmap;
            for (size_t i = 0; i < fields.size(); ++i) fmap.emplace(fields[i].name, i);

            std::vector<int> order;
            std::unordered_set<std::string> seen;
            for (auto& [n, e] : c->named) {
                auto it = fmap.find(n);
                if (it == fmap.end()) {
                    err("struct `" + ty->str() + "` has no field `" + n + "`", c);
                }
                if (!seen.insert(n).second) err("field `" + n + "` is initialized more than once", c);
                const size_t idx = it->second;
                order.push_back(static_cast<int>(idx));
                check_assign(e.get(), fields[idx].type, "construction of `" + ty->str() + "`");
            }
            if (order.size() != fields.size()) {
                err("construction of `" + ty->str() + "` must initialize every field", c);
            }
            c->field_order = std::move(order);
            return;
        }

        if (c->args.size() != fields.size()) {
            err("construction of `" + ty->str() + "` expects " + std::to_string(fields.size()) +
                    " arguments, but " + std::to_string(c->args.size()) + " were given",
                c);
        }
        c->field_order.clear();
        for (size_t i = 0; i < fields.size(); ++i) {
            check_assign(c->args[i].get(), fields[i].type, "construction of `" + ty->str() + "`");
            c->field_order.push_back(static_cast<int>(i));
        }
    }

    TypePtr check_builtin(Call* c, const std::string& name) {
        c->callee_kind = "builtin";
        c->builtin = name;
        if (!c->named.empty()) err("builtin function `" + name + "` does not support named arguments", c);

        if (name == "alloc") {
            if (c->args.size() != 1) err("`alloc(bytes)` takes 1 argument", c);
            if (!is_integer(check_expr(c->args[0].get()))) {
                err("size passed to `alloc` must be an integer", c);
            }
            return Type::make_ptr(t_void());
        }
        if (name == "realloc") {
            if (c->args.size() != 2) err("`realloc(p, bytes)` takes 2 arguments", c);
            TypePtr p0 = check_expr(c->args[0].get());
            if (!p0 || p0->kind != TypeKind::Ptr) {
                err("first argument of `realloc` must be a pointer", c);
            }
            if (!is_integer(check_expr(c->args[1].get()))) {
                err("size passed to `realloc` must be an integer", c);
            }
            return Type::make_ptr(t_void());
        }
        if (name == "free") {
            if (c->args.size() != 1) err("`free(p)` takes 1 argument", c);
            TypePtr p0 = check_expr(c->args[0].get());
            if (!p0 || (p0->kind != TypeKind::Ptr && p0->kind != TypeKind::Null)) {
                err("argument of `free` must be a pointer", c);
            }
            return t_void();
        }
        err("unknown builtin function `" + name + "`", c);
    }

    // ------------------------------------------------------------ 杂项
    bool is_lvalue(Expr* e) {
        if (!e) return false;
        if (auto nm = dynamic_cast<Name*>(e)) {
            return nm->ident != "true" && nm->ident != "false" && nm->ident != "null";
        }
        if (dynamic_cast<Index*>(e)) return true;
        if (auto m = dynamic_cast<Member*>(e)) {
            auto nm = dynamic_cast<Name*>(m->obj.get());
            if (!nm) return true;
            // 局部变量 / 本 unit 全局变量优先于同名 unit（`t.c = x` 中 t 是变量而不是 unit）
            if (lookup_local(nm->ident)) return true;
            if (info().globals.count(nm->ident)) return true;
            return !infos_.count(nm->ident);
        }
        if (auto u = dynamic_cast<Unary*>(e)) return u->op == "*";
        return false;
    }

    // 该表达式是否“根”在 class/interface 引用上（含其数组/切片元素）：
    // `c.x`、`h.items[0]`、`catArray[i]` 都算。U21：这类地址无法表达 RC 生命周期。
    bool rooted_at_reference(Expr* e) {
        while (e) {
            if (auto m = dynamic_cast<Member*>(e)) {
                e = m->obj.get();
                continue;
            }
            if (auto ix = dynamic_cast<Index*>(e)) {
                e = ix->base.get();
                continue;
            }
            break;
        }
        if (!e || !e->ty) return false;
        TypePtr t = e->ty;
        if (is_reference_type(t)) return true;
        if (t->kind == TypeKind::Array || t->kind == TypeKind::Slice) {
            TypePtr el = t->elem;
            while (el && (el->kind == TypeKind::Array || el->kind == TypeKind::Slice)) {
                el = el->elem;
            }
            return is_reference_type(el);
        }
        return false;
    }

    void check_entry() {
        auto it = infos_.find(entry_);
        if (it == infos_.end()) throw SemaError("cannot find entry unit `" + entry_ + "`");
        UnitInfo& ui = it->second;
        // Python 版这里用 self.cur（此时是最后一个被检查的 unit）取 filename，
        // 可能指错文件；这里显式指向入口 unit。
        cur_ = ui.unit;

        auto f = ui.funcs.find("main");
        if (f == ui.funcs.end() || f->second.empty()) {
            throw SemaError("unit `" + entry_ + "` has no `main` function");
        }
        FuncDecl* main = nullptr;
        for (auto* cand : f->second) {
            if (cand->params.empty()) {
                main = cand;
                break;
            }
        }
        if (!main) main = f->second.front();
        if (!main->rty) err("`main` is missing its return type", main);
        if (main->rty->str() != "int") err("`main` must return `int`", main);
        if (!main->params.empty()) err("`main` cannot take parameters yet", main);
    }
};

}  // namespace

// ---------------------------------------------------------------- 出口

std::string mangle(const std::string& unit, const std::string& name) {
    return "mu_" + unit + "_" + name;
}

Program analyze(std::vector<std::unique_ptr<Unit>> units, const std::string& entry) {
    Sema sema(std::move(units), entry);
    return sema.run();
}

std::string type_str(const TypePtr& t) { return t ? t->str() : std::string(); }


