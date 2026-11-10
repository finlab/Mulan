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
    std::unordered_set<const LocalVar*> cur_params_;
    std::vector<const LocalVar*> forin_vars_;
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
            err_ts("unit `" + t->unit + "` 仅在库内可见，跨库使用需要 `public unit`", spec);
        }
        if (!type_is_public(t)) {
            err_ts("类型 `" + t->unit + "." + t->name + "` 未声明为 public", spec);
        }
    }

    // ------------------------------------------------------------ 主流程

    // ------------------------------------------------------------ 收集
    void collect() {
        std::unordered_set<std::string> seen_units;
        for (auto& u : units_) {
            if (!seen_units.insert(u->name).second) {
                cur_ = u.get();
                err("重复的 unit `" + u->name + "`");
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
                    if (ui.classes.count(c->name)) err("重复定义 class `" + c->name + "`", c);
                    ui.classes[c->name] = c;
                } else if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) {
                    if (ui.interfaces.count(i->name)) err("重复定义 interface `" + i->name + "`", i);
                    ui.interfaces[i->name] = i;
                } else if (auto g = dynamic_cast<GlobalVar*>(d.get())) {
                    if (ui.globals.count(g->name)) err("重复定义变量 `" + g->name + "`", g);
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
                        err("`" + nm + "` 与其它声明重名", node);
                    }
                    if (it == names.end()) names.emplace(nm, node);
                    continue;
                }
                if (names.count(nm)) err("`" + nm + "` 在同一 unit 中重复声明", node);
                names.emplace(nm, node);
            }
        }

        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& use : u->uses) {
                if (!infos_.count(use->unit_name)) {
                    err("找不到 unit `" + use->unit_name + "`", use.get());
                }
                if (!unit_visible(use->unit_name)) {
                    err("unit `" + use->unit_name +
                            "` 仅在库内可见，跨库使用需要 `public unit`",
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
                err(what + " 不能使用非 public 类型 `" + b->str() + "`", at);
            }
            return;
        }
        if (!unit_visible(b->unit)) {
            err(what + " 不能使用不可见 unit `" + b->unit + "` 的类型 `" + b->str() + "`", at);
        }
        if (!type_is_public(b)) {
            err(what + " 不能使用非 public 类型 `" + b->str() + "`", at);
        }
    }

    void check_public_signatures() {
        for (auto& u : units_) {
            cur_ = u.get();
            for (auto& d : u->decls) {
                if (!d->is_public) continue;
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    if (f->is_extern) continue;
                    const std::string what = "public 函数 `" + f->name + "`";
                    check_exposed_type(f->rty, what, f);
                    for (auto& p : f->pty) check_exposed_type(p, what, f);
                } else if (auto c = dynamic_cast<ClassDecl*>(d.get())) {
                    const std::string what = "public class `" + c->name + "`";
                    if (c->ty->base) check_exposed_type(c->ty->base, what + " 的基类", c);
                    for (auto& i : c->ty->interfaces) {
                        check_exposed_type(i, what + " 的接口", c);
                    }
                    for (auto& fd : c->ty->fields) {
                        if (fd.is_public) {
                            check_exposed_type(fd.type, what + " 的 public 字段 `" + fd.name + "`", c);
                        }
                    }
                    for (auto& m : c->ty->methods) {
                        if (!m.is_public) continue;
                        const std::string mw = what + " 的 public 方法 `" + m.name + "`";
                        check_exposed_type(m.ret, mw, c);
                        for (auto& p : m.params) check_exposed_type(p, mw, c);
                    }
                } else if (auto sd = dynamic_cast<StructDecl*>(d.get())) {
                    const std::string what = "public struct `" + sd->name + "`";
                    for (auto& fd : sd->ty->fields) {
                        check_exposed_type(fd.type, what + " 的字段 `" + fd.name + "`", sd);
                    }
                    for (auto& m : sd->ty->methods) {
                        if (!m.is_public) continue;
                        const std::string mw = what + " 的 public 方法 `" + m.name + "`";
                        check_exposed_type(m.ret, mw, sd);
                        for (auto& p : m.params) check_exposed_type(p, mw, sd);
                    }
                } else if (auto itf = dynamic_cast<InterfaceDecl*>(d.get())) {
                    const std::string what = "public interface `" + itf->name + "`";
                    for (auto& m : itf->ty->methods) {
                        const std::string mw = what + " 的方法 `" + m.name + "`";
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
                err("class `" + c->name + "` 字段 `" + fname + "` 重复", c);
            }
            TypePtr ft = resolve_type(spec.get(), u);
            if (is_void(ft)) err("class 字段不能是 void", c);
            if (spec->weak && !is_class_type(ft)) {
                err("`weak` 字段必须是 class 引用（接口暂不支持 weak）", c);
            }
            if (noncopyable_struct(ft)) {
                err("含析构器的 struct `" + ft->str() +
                        "` 暂不能作为字段（字段析构未实现）",
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
                    err("align(N) 的 N 必须是编译期整型常量", sd);
                }
                if (!is_pow2(n) || n > 65536) {
                    err("align(N) 的 N 必须是 2 的幂（当前为 " + std::to_string(n) + "）", sd);
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
                if (ty->align_req > 0 && !ty->is_packed) {
                    size_t natural = 1;
                    for (auto& f : ty->fields) natural = std::max(natural, type_align(f.type));
                    if (static_cast<size_t>(ty->align_req) < natural) {
                        err("align(" + std::to_string(ty->align_req) + ") 小于结构体 `" +
                                sd->name + "` 的自然对齐 " + std::to_string(natural) +
                                "；降对齐请用 `packed`",
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
                        err("packed 结构 `" + sd->name + "` 的字段 `" + f.name +
                                "` 不能是 string（未对齐指针访问）",
                            sd);
                    }
                    if (base->kind == TypeKind::Slice) {
                        err("packed 结构 `" + sd->name + "` 的字段 `" + f.name +
                                "` 不能是动态数组 `" + base->str() + "`（未对齐指针访问）",
                            sd);
                    }
                    if (is_reference_type(base)) {
                        err("packed 结构 `" + sd->name + "` 的字段 `" + f.name +
                                "` 不能是 class/interface 引用（未对齐指针访问）",
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
            std::vector<Field> fields;
            std::unordered_set<std::string> fnames;
            for (auto& [fname, spec] : sd->fields) {
                if (!fnames.insert(fname).second) {
                    err("结构体 `" + sd->name + "` 字段 `" + fname + "` 重复", sd);
                }
                TypePtr ft = resolve_type(spec.get(), u);
                if (is_void(ft)) err("结构体 `" + sd->name + "` 的字段不能是 void", sd);
                if (same_type(ft, sd->ty)) {
                    err("结构体 `" + sd->name + "` 不能按值包含自身", sd);
                }
                if (spec->weak) err("`weak` 只能用于 class 字段", sd);
                if (noncopyable_struct(ft)) {
                    err("含析构器的 struct `" + ft->str() +
                            "` 暂不能作为字段（字段析构未实现）",
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

            sd->ty->methods.clear();
            for (auto& m : sd->methods) {
                m->rty = resolve_type(m->ret.get(), u);
                if (noncopyable_struct(m->rty)) {
                    err("含析构器的 struct `" + m->rty->str() +
                            "` 不能按值返回（用指针）",
                        m.get());
                }
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("参数不能是 void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("含析构器的 struct `" + pt->str() +
                                "` 只能通过指针传递（按值会拷贝）",
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
                    if (is_void(pt)) err("参数不能是 void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("含析构器的 struct `" + pt->str() +
                                "` 只能通过指针传递（按值会拷贝）",
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
                    if (i != 0 || has_base) err("class 只允许一个基类，且必须写在最前", c);
                    has_base = true;
                    c->ty->base = bt;
                } else if (bt->kind == TypeKind::Interface) {
                    for (auto& prev : c->ty->interfaces) {
                        if (same_type(prev, bt)) {
                            err("class `" + c->name + "` 重复实现接口 `" + bt->str() + "`", c);
                        }
                    }
                    c->ty->interfaces.push_back(bt);
                } else {
                    err("`" + c->bases[i] + "` 不是 class 或 interface", c);
                }
            }

            // 字段在后面的拓扑 pass 处理（基类字段需先就绪）

            c->ty->methods.clear();
            c->ty->is_abstract = false;
            for (auto& m : c->methods) {
                m->rty = resolve_type(m->ret.get(), u);
                if (noncopyable_struct(m->rty)) {
                    err("含析构器的 struct `" + m->rty->str() +
                            "` 不能按值返回（用指针）",
                        m.get());
                }
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("参数不能是 void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("含析构器的 struct `" + pt->str() +
                                "` 只能通过指针传递（按值会拷贝）",
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
                    if (is_void(pt)) err("参数不能是 void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("含析构器的 struct `" + pt->str() +
                                "` 只能通过指针传递（按值会拷贝）",
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
                    err("interface 只能继承 interface", itf);
                }
                for (auto& prev : itf->ty->interfaces) {
                    if (same_type(prev, bt)) {
                        err("interface `" + itf->name + "` 重复继承 `" + bt->str() + "`", itf);
                    }
                }
                itf->ty->interfaces.push_back(bt);
            }
            itf->ty->methods.clear();
            for (auto& m : itf->methods) {
                m->rty = resolve_type(m->ret.get(), u);
                if (noncopyable_struct(m->rty)) {
                    err("含析构器的 struct `" + m->rty->str() +
                            "` 不能按值返回（用指针）",
                        m.get());
                }
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("参数不能是 void", p.get());
                    if (noncopyable_struct(pt)) {
                        err("含析构器的 struct `" + pt->str() +
                                "` 只能通过指针传递（按值会拷贝）",
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
                    err("枚举 `" + ed->name + "` 的成员 `" + m + "` 重复", ed);
                }
            }
        }

        for (auto& d : u->decls) {
            auto g = dynamic_cast<GlobalVar*>(d.get());
            if (!g) continue;
            g->rty = resolve_type(g->spec.get(), u);
            if (is_void(g->rty)) err("变量 `" + g->name + "` 不能是 void", g);
        }

        for (auto& d : u->decls) {
            auto f = dynamic_cast<FuncDecl*>(d.get());
            if (!f) continue;
            f->rty = resolve_type(f->ret.get(), u);
            if (noncopyable_struct(f->rty)) {
                err("含析构器的 struct `" + f->rty->str() + "` 不能按值返回（用指针）", f);
            }
            f->pty.clear();
            for (auto& p : f->params) {
                TypePtr pt = resolve_type(p->spec.get(), u);
                if (is_void(pt)) err("参数 `" + p->name + "` 不能是 void", p.get());
                if (noncopyable_struct(pt)) {
                    err("含析构器的 struct `" + pt->str() +
                            "` 只能通过指针传递（按值会拷贝）",
                        p.get());
                }
                f->pty.push_back(pt);
            }
            if (f->is_extern) {
                if (contains_reference_type(f->rty)) {
                    err("`extern` 函数 `" + f->name + "` 不能返回 class/interface（C 侧不参与引用计数）",
                        f);
                }
                for (auto& pt : f->pty) {
                    if (contains_reference_type(pt)) {
                        err("`extern` 函数 `" + f->name + "` 的参数不能是 class/interface", f);
                    }
                }
                if (contains_slice_type(f->rty) || [&] {
                        for (auto& pt : f->pty)
                            if (contains_slice_type(pt)) return true;
                        return false;
                    }()) {
                    err("`extern` 函数 `" + f->name +
                            "` 不能使用动态数组 `T[]`（内部带有 RC 头，不是 C 布局）",
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
        if (!spec) err("缺少类型");
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
                    err_ts("暂不支持指向 class/interface 的指针（引用类型本身已是句柄）", spec);
                }
                return Type::make_ptr(elem, spec->is_const);
            }
            case TypeSpec::K::Slice: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_void(elem)) err_ts("动态数组元素不能是 void", spec);
                if (noncopyable_struct(elem)) {
                    err_ts("含析构器的 struct `" + elem->str() +
                               "` 暂不能作动态数组元素",
                           spec);
                }
                return Type::make_slice(elem, spec->is_const, spec->rank);
            }
            case TypeSpec::K::Array: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_void(elem)) err_ts("数组元素不能是 void", spec);
                TypePtr base = elem;
                while (base && base->kind == TypeKind::Array) base = base->elem;
                if (noncopyable_struct(base)) {
                    err_ts("含析构器的 struct `" + base->str() +
                               "` 暂不能作为数组元素（数组析构未实现）",
                           spec);
                }
                return Type::make_array(elem, spec->size);
            }
        }
        err_ts("未知类型 " + spec_text(spec), spec);
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
                    err_ts("未通过 `use` 导入 unit `" + un + "`", spec);
                }
                if (!unit_visible(un)) {
                    err_ts("unit `" + un + "` 仅在库内可见，跨库使用需要 `public unit`", spec);
                }
                UnitInfo& ui = it->second;
                auto need_pub = [&](bool is_pub) {
                    if (un != u->name && !is_pub) {
                        err_ts("类型 `" + un + "." + tn + "` 未声明为 public", spec);
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
                err_ts("unit `" + un + "` 中没有类型 `" + tn + "`", spec);
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
                err_ts("类型 `" + name + "` 有歧义：`" + found_unit + "." + name + "` 与 `" +
                           use->unit_name + "." + name + "`；请写限定名",
                       spec);
            }
            found = t;
            found_unit = use->unit_name;
        }
        if (found) return found;
        if (UnitInfo* stdn = std_info()) {
            if (auto s = stdn->structs.find(name); s != stdn->structs.end()) {
                if (!s->second->is_public)
                    err_ts("类型 `std.string." + name + "` 未声明为 public", spec);
                return s->second->ty;
            }
            if (auto e = stdn->enums.find(name); e != stdn->enums.end()) {
                if (!e->second->is_public)
                    err_ts("类型 `std.string." + name + "` 未声明为 public", spec);
                return e->second->ty;
            }
            if (auto c = stdn->classes.find(name); c != stdn->classes.end()) {
                if (!c->second->is_public)
                    err_ts("类型 `std.string." + name + "` 未声明为 public", spec);
                return c->second->ty;
            }
            if (auto i = stdn->interfaces.find(name); i != stdn->interfaces.end()) {
                if (!i->second->is_public)
                    err_ts("类型 `std.string." + name + "` 未声明为 public", spec);
                return i->second->ty;
            }
        }
        err_ts("未知类型 `" + name + "`", spec);
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
                err("类型 `" + name + "` 有歧义：`" + found_unit + "." + name + "` 与 `" +
                        use->unit_name + "." + name + "`；请写限定名",
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
        if (sc.count(lv->name)) err("重复声明 `" + lv->name + "`", lv);
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
                err("`main` 不参与重载（每个 unit 最多一个 `main`）", vec[1]);
            }
            if (vec.size() > 1) {
                bool hasExtern = false, hasBody = false;
                for (auto* f : vec) {
                    if (f->is_extern) hasExtern = true;
                    else hasBody = true;
                }
                if (hasExtern && hasBody) {
                    err("函数 `" + entry.first + "` 不能同时声明为 extern 与普通函数", vec[1]);
                }
                if (hasExtern) err("extern 函数 `" + entry.first + "` 不能重载", vec[1]);
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
                    if (same) err("函数 `" + entry.first + "` 的签名重复", vec[j]);
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
        if (!g->rty) err("全局变量 `" + g->name + "` 缺少类型", g);
        if (noncopyable_struct(g->rty)) {
            err("含析构器的 struct `" + g->rty->str() +
                    "` 暂不能作为全局变量（无退出析构）",
                g);
        }
        if (g->rty->kind == TypeKind::Struct && g->rty->needs_cleanup() &&
            !is_str(g->rty)) {
            err("含引用字段的 struct `" + g->rty->str() +
                    "` 暂不能作为全局变量（无退出析构）",
                g);
        }
        if (g->is_extern) {
            if (g->init) err("`extern` 变量 `" + g->name + "` 不能有初始化器", g);
            return;
        }
        if (!g->init) {
            if (g->is_const) err("`const` 变量 `" + g->name + "` 必须初始化", g);
            g->zero = true;
            return;
        }
        if (!is_const_expr(g->init.get())) {
            err("unit 级变量 `" + g->name + "` 的初始化器必须是编译期常量", g);
        }
        check_decl_init(g->zero, g->rty, g->init.get(), "全局初始化", g);
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
            if (n != 1) err("`operator~` 需要 1 个参数", f);
        } else if (sym == "[]") {
            if (n != 2) err("`operator[]` 需要 2 个参数（容器, 索引）", f);
        } else if (sym == "[]=") {
            if (n != 3) err("`operator[]=` 需要 3 个参数（容器, 索引, 值）", f);
            if (!is_void(f->rty)) err("`operator[]=` 必须返回 void", f);
        } else if (unary_or_binary) {
            if (n != 1 && n != 2) err("`operator" + sym + "` 需要 1 或 2 个参数", f);
        } else {
            static const char* allowed[] = {"*", "/", "%", "<", "<=", ">", ">=", "==",
                                            "!=", "&", "|", "^", "<<", ">>"};
            bool ok = false;
            for (auto* a : allowed) {
                if (sym == a) { ok = true; break; }
            }
            if (!ok) err("不允许重载运算符 `" + sym + "`", f);
            if (n != 2) err("二元运算符需要 2 个参数", f);
        }

        bool has_user = false;
        for (auto& p : f->pty) {
            if (p->kind == TypeKind::Struct || p->kind == TypeKind::Class) has_user = true;
            if (p->kind == TypeKind::Void) err("运算符参数不能是 void", f);
        }
        if (!has_user) err("运算符至少需要一个 struct/class 操作数", f);

        if (sym == "==" || sym == "!=") {
            for (auto& p : f->pty) {
                if (p->kind == TypeKind::Class || p->kind == TypeKind::Interface) {
                    err("class/interface 的 `" + sym + "` 固定为身份比较，不能重载", f);
                }
            }
        }

        if (sym == "<" || sym == "<=" || sym == ">" || sym == ">=" || sym == "==" || sym == "!=") {
            if (!(f->rty && f->rty->kind == TypeKind::Prim && f->rty->name == "bool")) {
                err("比较运算符必须返回 bool", f);
            }
        }

        if (sym == "[]" || sym == "[]=") {
            if (!(f->pty[0]->kind == TypeKind::Struct || f->pty[0]->kind == TypeKind::Class)) {
                err("`operator" + sym + "` 的第一个参数必须是 struct/class", f);
            }
            if (!is_integer(f->pty[1])) err("`operator[]` 的索引必须是整数", f);
        }
    }

    void check_func(FuncDecl* f) {
        if (!f->rty) err("函数 `" + f->name + "` 缺少返回类型", f);
        if (f->is_operator) check_operator_decl(f);
        cur_func_ = f;
        cur_ret_ = f->rty;
        cur_name_ = f->name;
        cur_class_ = nullptr;
        in_ctor_ = false;
        in_dtor_ = false;
        scopes_.clear();
        auto_groups_.clear();
        cur_params_.clear();
        forin_vars_.clear();
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
            cur_params_.insert(f->param_locals.back().get());
        }

        if (f->body) check_block(f->body.get());

        // 非 void 函数必须保证有返回值；main 例外（掉出等价 return 0）
        if (!is_void(f->rty) && f->cname != "main" && !guarantees_return(f->body.get())) {
            err("函数 `" + f->name + "` 可能执行到末尾而没有返回值", f);
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
        cur_params_.clear();
        forin_vars_.clear();
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
        cur_params_.clear();
        forin_vars_.clear();
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
            cur_params_.insert(out.back().get());
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
            err("方法 `" + m->name + "` 可能执行到末尾而没有返回值", m);
        }
        end_member();
    }

    void check_ctor(ClassDecl* c, CtorDecl* ctor) {
        begin_member(c->ty, t_void(), c->name + " 构造器", ctor->line, ctor->col);
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
            err("构造器必须在第一条语句调用 `super(...)`", ctor);
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
        begin_member(c->ty, t_void(), c->name + " 析构器", d->line, d->col);
        in_dtor_ = true;
        check_block(d->body.get());
        in_dtor_ = false;
        end_member();
    }

    void check_struct_method(StructDecl* sd, MethodDecl* m) {
        if (!m->body) {
            err("struct 方法 `" + m->name + "` 必须有函数体", m);
        }
        begin_member(sd->ty, m->rty, sd->name + "." + m->name, m->line, m->col);
        declare_params(m->param_locals, m->params, m->pty);
        check_block(m->body.get());
        if (!is_void(m->rty) && !guarantees_return(m->body.get())) {
            err("方法 `" + m->name + "` 可能执行到末尾而没有返回值", m);
        }
        end_member();
    }

    void check_struct_ctor(StructDecl* sd, CtorDecl* ctor) {
        begin_member(sd->ty, t_void(), sd->name + " 构造器", ctor->line, ctor->col);
        in_ctor_ = true;
        declare_params(ctor->param_locals, ctor->params, ctor->pty);
        check_block(ctor->body.get());
        end_member();
    }

    void check_struct_dtor(StructDecl* sd) {
        DtorDecl* d = sd->dtor.get();
        if (!d) return;
        begin_member(sd->ty, t_void(), sd->name + " 析构器", d->line, d->col);
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
                err("struct `" + sd->name + "` 方法 `" + key + "` 重复", m.get());
            }
            if (sd->ty->field_type(m->name)) {
                err("struct `" + sd->name + "` 的方法 `" + m->name + "` 与字段同名", m.get());
            }
            check_struct_method(sd, m.get());
        }
        std::unordered_set<std::string> ctor_keys;
        for (auto& ctor : sd->ctors) {
            std::string key;
            for (auto& p : ctor->pty) key += p->str() + ",";
            if (!ctor_keys.insert(key).second) {
                err("struct `" + sd->name + "` 的构造器签名重复", ctor.get());
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
                err("class `" + c->name + "` 方法 `" + key + "` 重复", m.get());
            }
            if (c->ty->field_type(m->name)) {
                err("class `" + c->name + "` 的方法 `" + m->name + "` 与字段同名", m.get());
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
                    err("覆盖/实现基类方法必须写 `override`", m.get());
                }
                std::vector<const MethodSig*> matches;
                collect_sigs(c->ty->base ? c->ty->base : c->ty, sig, matches);
                for (auto& i : c->ty->interfaces) collect_sigs(i, sig, matches);
                for (auto* ms : matches) {
                    if (!same_type(ms->ret, m->rty)) {
                        err("override 的返回类型不一致：与 `" + ms->owner->str() + "` 的声明不符",
                            m.get());
                    }
                }
                if (!same_type(base_sig->ret, m->rty)) {
                    err("override 的返回类型不一致", m.get());
                }
            } else if (m->is_override) {
                err("`override` 没有匹配的基类/接口方法", m.get());
            }
            check_method(c, m.get());
        }
        std::unordered_set<std::string> ctor_keys;
        for (auto& ctor : c->ctors) {
            std::string key;
            for (auto& p : ctor->pty) key += p->str() + ",";
            if (!ctor_keys.insert(key).second) {
                err("class `" + c->name + "` 的构造器签名重复", ctor.get());
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
                err("class `" + c->name + "` 的基类没有无参构造器，必须显式声明构造器", c);
            }
        }
        // 派生类字段不能遮蔽基类方法名（用字段名访问会与方法调用混淆）
        if (c->ty->base) {
            for (auto& [fname, spec] : c->fields) {
                if (has_method_named(c->ty->base, fname)) {
                    err("class `" + c->name + "` 的字段 `" + fname + "` 与基类方法同名", c);
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
                err("接口方法 `" + m->name + "` 可能执行到末尾而没有返回值", m.get());
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
                    if (!seen.insert(cur.get()).second) err("class `" + c->name + "` 存在继承环", c);
                    cur = cur->base;
                }
            }
        }

        std::unordered_map<Type*, int> color;
        std::function<void(TypePtr)> visit = [&](TypePtr t) {
            if (!t || t->kind != TypeKind::Interface) return;
            int& col = color[t.get()];
            if (col == 1) err("interface 继承环：" + t->str());
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
                            err("接口默认方法 `" + defaults[i].name +
                                    "` 冲突，必须在类中覆盖",
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
            check_cond(is->cond.get(), "if 条件");
            check_block(is->then.get());
            if (is->els) {
                if (auto b = dynamic_cast<Block*>(is->els.get())) {
                    check_block(b);
                } else {
                    check_stmt(is->els.get());
                }
            }
        } else if (auto ws = dynamic_cast<WhileStmt*>(s)) {
            check_cond(ws->cond.get(), "while 条件");
            ++loop_depth_;
            check_block(ws->body.get());
            --loop_depth_;
        } else if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            check_for(fs);
        } else if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            check_switch(sw);
        } else if (dynamic_cast<BreakStmt*>(s)) {
            if (loop_depth_ == 0) err("`break` 只能出现在循环中", s);
        } else if (dynamic_cast<ContinueStmt*>(s)) {
            if (loop_depth_ == 0) err("`continue` 只能出现在循环中", s);
        } else if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            check_return(rs);
        } else if (auto d = dynamic_cast<DelStmt*>(s)) {
            TypePtr t = check_expr(d->value.get());
            if (t && t->kind == TypeKind::Null) {
                // `del null` 是空操作
            } else if (!is_reference_type(t)) {
                err("`del` 只能用于 class/interface 引用", d);
            }
            if (!is_lvalue(d->value.get()) && !(t && t->kind == TypeKind::Null)) {
                err("`del` 的目标必须是可写的引用左值", d);
            }
            if (auto n = dynamic_cast<Name*>(d->value.get()); n && n->ident == "this") {
                err("不能 `del this`", d);
            }
            if (is_weak_field_access(d->value.get())) {
                err("不能 `del` weak 字段（weak 不持有引用）", d);
            }
            if (auto ix = dynamic_cast<Index*>(d->value.get()); ix && ix->get_func) {
                err("不能 `del` 运算符下标的结果（`operator[]` 返回的是值）", d);
            }
            if (auto n = dynamic_cast<Name*>(d->value.get())) {
                if (auto lv = std::get_if<LocalVar*>(&n->sym)) {
                    if (cur_params_.count(*lv)) {
                        err("不能 `del` 函数参数（参数是借用）；先赋给局部变量", d);
                    }
                    for (auto* fv : forin_vars_) {
                        if (fv == *lv) {
                            err("不能 `del` for-in 循环变量（借用）；先赋给局部变量", d);
                        }
                    }
                }
            }
            if (root_is_const(d->value.get())) err("`del` 不能用于 `const` 绑定", d);
        } else if (auto b = dynamic_cast<Block*>(s)) {
            check_block(b);
        } else {
            err("未知语句", s);
        }
    }

    void check_local(LocalVar* lv) {
        if (lv->is_auto) {
            auto ie = dynamic_cast<InitExpr*>(lv->init.get());
            if (!ie || !ie->expr) err("`auto` 必须用表达式初始化", lv);
            Expr* e = ie->expr.get();
            if (dynamic_cast<NullLit*>(e)) err("不能从 `null` 推断类型", lv);
            TypePtr t = check_expr(e);
            if (is_void(t)) err("不能从 void 表达式推断类型", lv);
            if (t->kind == TypeKind::Range) err("不能从区间推断类型", lv);
            lv->rty = t;
            // 同一声明语句的多个 auto 声明符必须推断出同一类型
            auto it = auto_groups_.find(lv->group);
            if (it == auto_groups_.end()) {
                auto_groups_[lv->group] = t;
            } else if (!same_type(it->second, t)) {
                err("同一声明中的 `auto` 推断出不同类型：" + it->second->str() + " 与 " +
                        t->str(),
                    lv);
            }
        } else {
            lv->rty = resolve_type(lv->spec.get(), cur_);
            if (is_void(lv->rty)) err("变量不能是 void 类型", lv);
        }

        if (lv->is_const && !lv->init) {
            err("`const` 变量 `" + lv->name + "` 必须初始化", lv);
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
                        "` 没有无参构造器，声明时必须用构造器初始化",
                    lv);
            }
        }

        declare_local(lv);
        if (lv->init) check_decl_init(lv->zero, lv->rty, lv->init.get(), "初始化", lv);
    }

    void check_decl_init(bool& zero_flag, const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node) {
        if (!init) err("缺少初始化器", node);
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            if (ia->items.empty()) {
                if (!is_zeroable(ty)) {
                    err("类型 `" + ty->str() +
                            "` 不能用 `{}` 初始化；只有聚合类型（struct/数组/切片/string）可用 `{}`，"
                            "标量请用各自的常量值（0、0.0、false、null）",
                        node);
                }
                zero_flag = true;
                return;
            }
        }
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            if (auto il = dynamic_cast<IntLit*>(ie->expr.get())) {
                if (il->value == 0 && is_zeroable(ty)) {
                    err("聚合类型 `" + ty->str() + "` 的零初始化请用 `{}`，不再支持 `= 0`", node);
                }
            }
        }
        check_init_into(ty, init, what, node);
    }

    // 单个初始化位置：声明初始化、聚合成员、嵌套花括号都走这里
    void check_init_into(const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node) {
        if (!init) err("缺少初始化器", node);
        if (auto ia0 = dynamic_cast<InitAggregate*>(init)) {
            if (ia0->items.empty()) {
                if (!is_zeroable(ty)) {
                    err("类型 `" + ty->str() +
                            "` 不能用 `{}` 初始化；只有聚合类型（struct/数组/切片/string）可用 `{}`，"
                            "标量请用各自的常量值（0、0.0、false、null）",
                        node);
                }
                return;
            }
        }
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            Expr* e = ie->expr.get();
            if (!e) err("缺少初始化表达式", node);
            if (auto il = dynamic_cast<IntLit*>(e)) {
                if (il->value == 0 && is_zeroable(ty)) {
                    err("聚合类型 `" + ty->str() + "` 的零初始化请用 `{}`，不再支持 `= 0`", node);
                }
            }
            if (auto call = dynamic_cast<Call*>(e);
                call && ty->kind == TypeKind::Struct && !ty->ctors.empty()) {
                direct_init_expr_ = e;
                check_expr(e, ty);
                direct_init_expr_ = nullptr;
                if (call->callee_kind == "struct_ctor") return;  // 就地构造
                if (!same_type(call->ty, ty)) {
                    err("有构造器的 struct `" + ty->str() +
                            "` 必须用 `T(...)` 构造，或由同类型表达式拷贝初始化",
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
        err("未知的初始化器", node);
    }

    void check_aggregate(InitAggregate* init, const TypePtr& ty, const std::string& what) {
        auto& items = init->items;

        if (ty->kind == TypeKind::Struct && !ty->ctors.empty()) {
            err("有构造器的 struct `" + ty->str() + "` 不能使用聚合初始化", init);
        }

        if (ty->kind == TypeKind::Struct) {
            const auto& fields = ty->fields;
            // 跨 unit 的聚合初始化要求字段全部 public
            if (cur_ && cur_->name != ty->unit) {
                for (auto& f : fields) {
                    if (!f.is_public) {
                        err("struct `" + ty->str() + "` 的字段 `" + f.name +
                                "` 未声明为 public，不能从其他 unit 聚合初始化",
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
            if (any_positional && any_named) err("命名字段与位置初始化不能混用", init);
            if (!any_named) {
                if (items.size() != fields.size()) {
                    err("结构体 `" + ty->str() + "` 需要 " + std::to_string(fields.size()) +
                            " 个初始值，实际 " + std::to_string(items.size()) + " 个",
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
                    err("结构体 `" + ty->str() + "` 没有字段 `" + n + "`", init);
                }
                if (!seen.insert(n).second) err("字段 `" + n + "` 重复初始化", init);
                check_init_into(fields[it->second].type, sub.get(), what, init);
            }

            std::string missing;
            for (auto& f : fields) {
                if (seen.count(f.name)) continue;
                if (!missing.empty()) missing += ", ";
                missing += f.name;
            }
            if (!missing.empty()) {
                err("结构体 `" + ty->str() + "` 缺少字段初始化：" + missing, init);
            }
            return;
        }

        if (ty->kind == TypeKind::Array) {
            for (auto& item : items) {
                if (!item.first.empty()) err("数组初始化不能用命名字段", init);
            }
            if (items.size() > ty->array_size) {
                err("数组长度 " + std::to_string(ty->array_size) + "，却给了 " +
                        std::to_string(items.size()) + " 个初始值",
                    init);
            }
            for (auto& item : items) check_init_into(ty->elem, item.second.get(), what, init);
            return;
        }

        if (ty->kind == TypeKind::Slice) {
            if (type_align(ty->elem) > 8) err_slice_elem_align(ty->elem, init);
            if (ty->rank != 1) {
                err("动态矩形数组 `" + ty->str() + "` 请用 `new T[m,n]` 初始化", init);
            }
            for (auto& item : items) {
                if (!item.first.empty()) err("切片初始化不能用命名字段", init);
            }
            for (auto& item : items) check_init_into(ty->elem, item.second.get(), what, init);
            return;
        }

        err("类型 " + ty->str() + " 不能用花括号初始化", init);
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
            if (n->ident == "this") err("不能给 `this` 赋值", t);
            if (root_is_const(t)) err("`" + n->ident + "` 是 const 绑定，不能赋值", t);
            return;
        }
        if (dynamic_cast<Member*>(t)) {
            if (root_is_const(t)) err("不能修改 const 绑定的成员", t);
            return;
        }
        if (auto ix = dynamic_cast<Index*>(t)) {
            TypePtr bt = ix->base->ty;
            if (is_str(bt)) err("string 不可变，不能写元素（如需构造请用 byte[]/str_concat）", t);
            if (bt && bt->kind == TypeKind::Slice && bt->elem && bt->elem->is_const)
                err("不能通过 const 切片写入", t);
            if (bt && bt->kind == TypeKind::Ptr && bt->elem && bt->elem->is_const)
                err("不能通过 const 指针写入", t);
            if (root_is_const(ix->base.get())) err("不能修改 const 绑定的元素", t);
            return;
        }
        if (auto u = dynamic_cast<Unary*>(t)) {
            if (u->op == "*") {
                TypePtr ot = u->operand->ty;
                if (ot && ot->kind == TypeKind::Ptr && ot->elem && ot->elem->is_const)
                    err("不能通过 const 指针写入", t);
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
        if (!is_lvalue(s->target.get())) err("赋值左侧必须是可赋值的对象", s);

        if (auto ix = dynamic_cast<Index*>(s->target.get())) {
            TypePtr bt = check_expr(ix->base.get());
            if (is_user_type(bt)) {
                if (is_str(bt))
                    err("string 不可变，不能写元素（如需构造请用 byte[]/str_concat）",
                        s->target.get());
                if (ix->indices.size() != 1)
                    err("用户 `operator[]=` 只接受一个下标", s->target.get());
                TypePtr it = check_expr(ix->indices[0].get());
                if (!is_integer(it)) err("下标必须是整数", s->target.get());
                if (s->op != "=") err("用户下标暂只支持 `=` 赋值", s);
                auto cands = collect_operators("[]=", {bt, it});
                if (cands.empty()) {
                    err("类型 `" + bt->str() + "` 不支持下标赋值（没有 `operator[]=`）", s);
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
            err("枚举成员是常量，不能赋值", s);
        }
        check_writable(s->target.get());
        if (s->op == "=") {
            check_assign(s->value.get(), tt, "赋值");
            return;
        }
        if (is_enum_t(tt)) {
            err("枚举 `" + tt->str() + "` 不支持 `" + s->op +
                    "`（请写 `x = (x as int " + s->op.substr(0, s->op.size() - 1) +
                    " y) as " + tt->name + "`）",
                s);
        }
        const std::string op = s->op.empty() ? std::string() : s->op.substr(0, s->op.size() - 1);
        TypePtr vt = check_expr(s->value.get(), tt);
        if (op == "<<" || op == ">>") {
            if (!is_integer(tt) || !is_integer(vt)) err("`" + s->op + "` 两侧必须是整数", s);
            return;
        }
        if (op == "&" || op == "|" || op == "^") {
            if (!is_integer(tt) || !is_integer(vt)) err("`" + s->op + "` 两侧必须是整数", s);
            return;
        }
        if ((op == "+" || op == "-") && tt->kind == TypeKind::Ptr) {
            if (tt->elem && tt->elem->kind == TypeKind::Void) err("void* 不能做指针算术", s);
            if (!is_integer(vt)) err("指针加减只能配合整数", s);
            return;
        }
        if (!(is_numeric(tt) && is_numeric(vt))) err("`" + s->op + "` 需要数值操作数", s);
    }

    void check_return(ReturnStmt* s) {
        if (in_dtor_) err("析构器中不允许 `return`（字段释放与基类析构在函数末尾执行）", s);
        if (!cur_ret_) err("`return` 只能出现在函数/方法中", s);
        TypePtr rt = cur_ret_;
        if (!s->value) {
            if (!is_void(rt)) {
                err("`" + cur_name_ + "` 必须返回 " + (rt ? rt->str() : "void") + " 类型的值", s);
            }
            return;
        }
        if (is_void(rt)) err("void 函数/方法 `" + cur_name_ + "` 不能返回值", s);
        check_assign(s->value.get(), rt, "返回值");
    }

    void check_cond(Expr* e, const std::string& what) {
        TypePtr t = check_expr(e);
        if (!(is_prim(t) && t->name == "bool")) {
            err(what + "必须是 bool，实际是 " + (t ? t->str() : "void"), e);
        }
    }

    // switch 分支模式求值：只接受编译期常量（字面量 / 枚举成员）
    long long checked_int_pattern(long long v, const TypePtr& st, Expr* e) {
        if (!promote_int_literal(v, st)) {
            err("switch 模式值 " + std::to_string(v) + " 超出 " + st->str() + " 的取值范围", e);
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
                err("枚举 `" + st->str() + "` 没有成员 `" + n->ident + "`", e);
            }
            if (auto* m = dynamic_cast<Member*>(e)) {
                TypePtr pt = check_expr(e);
                auto* ec = std::get_if<EnumConst*>(&m->sym);
                if (!ec || !*ec || !pt || !same_type(pt, st)) {
                    err("switch 模式类型与被匹配值不一致：期望 " + st->str(), e);
                }
                return (*ec)->value;
            }
            err("枚举 switch 的模式必须是 " + st->str() + " 的成员", e);
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
                err("模式 `" + n->ident + "` 是枚举成员，不能匹配 " + st->str(), e);
            }
            err("switch 分支的模式必须是编译期常量：`" + n->ident +
                    "` 是变量；请用字面量或枚举成员",
                e);
        }
        err("switch 分支的模式必须是编译期常量（整数字面量、字符字面量或枚举成员）", e);
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
        if (!st) err("switch 缺少被匹配的表达式", s);
        const bool enum_sw = st->kind == TypeKind::Enum;
        if (!enum_sw && !(is_prim(st) && is_integer(st))) {
            err("switch 的被匹配值必须是整数或枚举（实际是 " + st->str() + "）", s->scrut.get());
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
                if (s->has_else) err("switch 只能有一个 `else` 分支", s);
                s->has_else = true;
            }
            if (arm->pats.empty() && !arm->is_else) {
                err("switch 分支缺少模式", s);
            }
            for (auto& p : arm->pats) {
                p.vlo = pattern_value(p.lo.get(), st);
                Iv iv;
                iv.lo = p.vlo;
                iv.node = p.lo.get();
                if (p.is_range) {
                    if (enum_sw) {
                        err("枚举 switch 不支持区间分支（枚举成员没有数值序）；请逐个列出成员",
                            p.lo.get());
                    }
                    p.vhi = pattern_value(p.hi.get(), st);
                    if (p.vlo >= p.vhi) {
                        err("switch 区间 `" + std::to_string(p.vlo) + ".." +
                                std::to_string(p.vhi) +
                                "` 为空：半开区间的上界必须大于下界",
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
                err("switch 分支重叠：值 " + std::to_string(cur.lo) +
                        " 已被前面的分支覆盖（区间半开，`a..b` 不含 b）",
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
                    err("switch 没有覆盖枚举 " + st->str() + " 的全部成员（缺少 " + missing +
                            "）；补分支或写 `else:`",
                        s);
                }
                s->enum_exhaustive = true;
            } else if (missing.empty()) {
                warn("`else` 分支不可达：已覆盖枚举 `" + st->str() + "` 的全部成员", s);
            }
        }
    }

    void check_for(ForInStmt* s) {
        ScopeGuard guard(*this);

        if (auto r = dynamic_cast<RangeExpr*>(s->seq.get())) {
            TypePtr lt = check_expr(r->lo.get());
            TypePtr ht = check_expr(r->hi.get());
            if (is_enum_t(lt) || is_enum_t(ht)) {
                err("区间端点不能是枚举（请写 `x as int`）", r);
            }
            if (!is_integer(lt) || !is_integer(ht)) err("区间两端必须是整数", r);
            TypePtr vt = usual_arith(lt, ht);
            if (!vt) vt = t_int();
            if (!is_integer(vt)) err("区间的循环变量必须是整数", r);
            s->loop_kind = "range";
            s->seq_ty = t_range();
            s->elem_ty = vt;
        } else {
            TypePtr st = check_expr(s->seq.get());
            if (!st) err("for-in 缺少序列", s);
            if (st->kind == TypeKind::Array) {
                s->loop_kind = "array";
                s->elem_ty = st->elem;
            } else if (st->kind == TypeKind::Slice) {
                s->loop_kind = "slice";
                s->elem_ty = st->elem;
            } else if (is_str(st)) {
                s->loop_kind = "string";
                s->elem_ty = Type::make_prim("byte");
            } else {
                err("for-in 不能遍历 " + st->str() + "（只支持区间、数组、切片、string）",
                    s->seq.get());
            }
            s->seq_ty = st;
        }

        if (s->by_ref && s->loop_kind == "string") {
            err("string 不可变，不能用 `for ref` 遍历", s);
        }
        if (s->by_ref && s->loop_kind == "range") {
            err("区间循环不能用 `for ref`", s);
        }
        if (s->by_ref && s->elem_ty && is_reference_type(s->elem_ty)) {
            err("暂不支持对 class/interface 元素使用 `for ref`（用值绑定句柄）", s);
        }
        if (s->by_ref && s->seq_ty && s->seq_ty->kind == TypeKind::Slice && s->seq_ty->elem &&
            s->seq_ty->elem->is_const) {
            err("不能对 const 切片使用 `for ref`", s->seq.get());
        }
        if (s->by_ref && root_is_const(s->seq.get())) {
            err("不能对 const 绑定使用 `for ref`", s->seq.get());
        }
        if (s->by_ref && !simple_seq(s->seq.get())) {
            err("`for ref` 的序列必须是变量或成员访问（可寻址）", s->seq.get());
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

        ++loop_depth_;
        forin_vars_.push_back(s->lvar.get());
        check_block(s->body.get());
        forin_vars_.pop_back();
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
            err(what + "：含析构器的 struct `" + dst->str() +
                    "` 不能按值拷贝（用指针/显式 clone）",
                e);
        }
        TypePtr src = check_expr(e, dst);
        if (assignable(e, src, dst)) return src;
        if (is_enum_t(src) || is_enum_t(dst)) {
            err(what + "：不能把 " + (src ? src->str() : "void") + " 赋给 " +
                    (dst ? dst->str() : "void") + "（枚举与整数之间没有隐式转换，请写 `as`）",
                e);
        }
        err(what + "：不能把 " + (src ? src->str() : "void") + " 赋给 " +
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
        if (!e) err("缺少表达式");
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
            if (ix->indices.empty()) err("下标不能为空", e);
            std::vector<TypePtr> its;
            for (auto& idxExpr : ix->indices) {
                TypePtr it = check_expr(idxExpr.get());
                if (is_enum_t(it)) {
                    err("下标不能是枚举 `" + it->str() + "`（请写 `x as int`）", idxExpr.get());
                }
                if (!is_integer(it)) {
                    err("下标必须是整数，实际是 " + (it ? it->str() : "void"),
                        idxExpr.get());
                }
                its.push_back(it);
            }
            if (!bt) err("类型不能下标访问", e);
            const size_t n = ix->indices.size();
            if (bt->kind == TypeKind::Array) {
                TypePtr cur = bt;
                size_t depth = 0;
                while (cur && cur->kind == TypeKind::Array) {
                    cur = cur->elem;
                    depth++;
                }
                if (n > depth) {
                    err("下标过多：`" + bt->str() + "` 只有 " + std::to_string(depth) +
                            " 维",
                        e);
                }
                TypePtr r = bt;
                for (size_t i = 0; i < n; ++i) r = r->elem;
                return r;
            }
            if (bt->kind == TypeKind::Slice) {
                if (static_cast<int>(n) != bt->rank) {
                    err("`" + bt->str() + "` 需要 " + std::to_string(bt->rank) +
                            " 个下标（实际 " + std::to_string(n) + " 个）",
                        e);
                }
                return bt->elem;
            }
            if (bt->kind == TypeKind::Ptr) {
                if (n != 1) err("指针只接受一个下标", e);
                return bt->elem;
            }
            if (is_str(bt)) {
                if (n != 1) err("string 只接受一个下标", e);
                return Type::make_prim("byte");
            }
            if (is_user_type(bt)) {
                if (n != 1) err("用户 `operator[]` 只接受一个下标", e);
                auto cands = collect_operators("[]", {bt, its[0]});
                if (cands.empty()) {
                    err("类型 `" + bt->str() + "` 不支持下标（没有 `operator[]`）", e);
                }
                auto f = choose_overload(cands, {ix->base.get(), ix->indices[0].get()},
                                         "operator[]", e);
                ix->get_func = f;
                return f->rty;
            }
            err("类型 " + bt->str() + " 不能下标访问", e);
        }

        if (auto u = dynamic_cast<Unary*>(e)) return check_unary(u);

        if (auto nt = dynamic_cast<NotExpr*>(e)) {
            TypePtr t = check_expr(nt->operand.get());
            if (!(is_prim(t) && t->name == "bool")) {
                err("`not` 需要 bool 操作数，实际是 " + (t ? t->str() : "void"), e);
            }
            return t_bool();
        }

        if (auto b = dynamic_cast<Binary*>(e)) return check_binary(b);

        if (auto te = dynamic_cast<Ternary*>(e)) return check_ternary(te, expected);

        if (auto c = dynamic_cast<Cast*>(e)) {
            TypePtr st = check_expr(c->expr.get());
            TypePtr dt = resolve_type(c->spec.get(), cur_);
            if (!can_cast(st, dt)) {
                err("不能用 `as` 把 " + (st ? st->str() : "void") + " 转成 " + dt->str(), e);
            }
            e->ty = dt;
            return dt;
        }

        if (auto ne = dynamic_cast<NewExpr*>(e)) return check_new(ne);

        if (auto c = dynamic_cast<Call*>(e)) return check_call(c);

        if (auto sz = dynamic_cast<SizeofExpr*>(e)) {
            TypePtr t = resolve_type(sz->spec.get(), cur_);
            if (is_void(t)) err("`" + sz->which + "(void)` 非法", e);
            e->ty = Type::make_prim("usize");
            return e->ty;
        }

        if (auto ln = dynamic_cast<Len*>(e)) {
            TypePtr t = check_expr(ln->seq.get());
            if (is_str(t)) {
                if (ln->dim) err("string 的长度没有维度参数", ln->dim.get());
                return Type::make_prim("usize");
            }
            if (ln->dim) {
                TypePtr dt = check_expr(ln->dim.get());
                if (!is_integer(dt)) err("`len(seq, k)` 的维度必须是整数", ln->dim.get());
                auto il = dynamic_cast<IntLit*>(ln->dim.get());
                if (t && t->kind == TypeKind::Array) {
                    if (!il) err("静态数组的维度参数必须是整数字面量", ln->dim.get());
                    size_t depth = 0;
                    for (TypePtr c = t; c && c->kind == TypeKind::Array; c = c->elem) depth++;
                    if (il && (il->value < 0 ||
                               static_cast<size_t>(il->value) >= depth)) {
                        err("维度越界：`" + t->str() + "` 只有 " + std::to_string(depth) +
                                " 维",
                            ln->dim.get());
                    }
                } else if (t && t->kind == TypeKind::Slice) {
                    if (il && (il->value < 0 || il->value >= t->rank)) {
                        err("维度越界：`" + t->str() + "` 只有 " + std::to_string(t->rank) +
                                " 维",
                            ln->dim.get());
                    }
                } else if (t && t->kind != TypeKind::Array) {
                    err("`len(seq, k)` 只能用于多维数组", ln->dim.get());
                }
            }
            if (t && (t->kind == TypeKind::Array || t->kind == TypeKind::Slice)) {
                return Type::make_prim("usize");
            }
            err("`len` 不支持 " + (t ? t->str() : "void"), e);
        }

        if (dynamic_cast<RangeExpr*>(e)) err("区间 `a..b` 只能用于 for-in", e);

        err("未知表达式", e);
    }

    TypePtr check_name(Name* e) {
        if (e->ident == "super") {
            err("`super` 只能用于 `super(...)` 或 `super.method(...)`", e);
        }
        if (e->ident == "this" && lookup_local("this") == nullptr) {
            err("`this` 只能出现在 class/struct 方法/构造器/析构器中", e);
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
            err("函数 `" + e->ident + "` 不能作为值使用", e);
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
                    err("`" + e->ident + "` 有歧义：`" + imp_g_unit + "." + e->ident + "` 与 `" +
                            use->unit_name + "." + e->ident + "`；请写限定名",
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
                    err("`" + e->ident + "` 有歧义：`" + imp_ec_unit + "." + e->ident + "` 与 `" +
                            use->unit_name + "." + e->ident + "`；请写限定名",
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
            err("函数 `" + e->ident + "` 不能作为值使用", e);
        }
        if (imp_ec) {
            e->kind = "enum_const";
            e->sym = imp_ec;
            return imp_ec->ty;
        }
        err("未定义的标识符 `" + e->ident + "`", e);
    }

    TypePtr check_member(Member* e) {
        Expr* obj = e->obj.get();
        if (!obj) err("缺少成员访问的对象", e);

        if (auto nm = dynamic_cast<Name*>(obj); nm && lookup_local(nm->ident) == nullptr) {
            const std::string uname = nm->ident;
            auto it = infos_.find(uname);
            if (it != infos_.end() && uname != cur_->name) {
                if (!unit_imported(uname)) err("未通过 `use` 导入 unit `" + uname + "`", e);
                if (!unit_visible(uname)) {
                    err("unit `" + uname + "` 仅在库内可见，跨库使用需要 `public unit`", e);
                }
                UnitInfo& ui = it->second;
                nm->kind = "unit";
                nm->ty = Type::make_module(uname);
                if (auto f = ui.funcs.find(e->name); f != ui.funcs.end() && !f->second.empty()) {
                    e->kind = "func";
                    e->sym = f->second.front();
                    err("函数 `" + uname + "." + e->name + "` 不能作为值使用", e);
                }
                if (auto g = ui.globals.find(e->name); g != ui.globals.end()) {
                    if (!g->second->is_public) {
                        err("`" + uname + "." + e->name + "` 未声明为 public", e);
                    }
                    e->kind = "global";
                    e->sym = g->second;
                    return g->second->rty;
                }
                err("unit `" + uname + "` 中没有 `" + e->name + "`", e);
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
                            err("未通过 `use` 导入 unit `" + un->ident + "`", e);
                        }
                        if (!unit_visible(un->ident)) {
                            err("unit `" + un->ident +
                                    "` 仅在库内可见，跨库使用需要 `public unit`",
                                e);
                        }
                        TypePtr t = find_type_named(un->ident, inner->name, false);
                        if (!t) {
                            err("unit `" + un->ident + "` 中没有类型 `" + inner->name + "`", e);
                        }
                        if (!type_is_public(t)) {
                            err("类型 `" + un->ident + "." + inner->name +
                                    "` 未声明为 public",
                                e);
                        }
                        et = t;
                        inner->ty = et;
                    }
                }
            }
            if (et) {
                if (et->kind != TypeKind::Enum) {
                    err("`" + et->str() + "` 不是枚举，不能用 `类型.成员` 引用", e);
                }
                EnumConst* ec = find_enum_member(et, e->name);
                if (!ec) err("枚举 `" + et->str() + "` 没有成员 `" + e->name + "`", e);
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
                    err("方法 `" + e->name + "` 必须被调用（`x." + e->name + "(...)`）", e);
                }
                err("类型 `" + ot->str() + "` 没有字段 `" + e->name + "`", e);
            }
            for (auto& f : ot->fields) {
                if (f.name != e->name) continue;
                if (!f.is_public && (!cur_ || cur_->name != ot->unit)) {
                    err("字段 `" + e->name + "` 未声明为 public（类型 `" + ot->str() + "`）", e);
                }
                break;
            }
            e->kind = "field";
            e->field_index = ot->field_index(e->name);
            return ft;
        }
        err("类型 " + (ot ? ot->str() : "void") + " 没有成员 `" + e->name + "`", e);
    }

    TypePtr check_unary(Unary* e) {
        if (e->op == "&") {
            if (auto nm = dynamic_cast<Name*>(e->operand.get());
                nm && nm->ident == "this" && lookup_local("this") != nullptr) {
                err("不能对 `this` 取地址：`this` 是当前对象的引用", e);
            }
            if (!is_lvalue(e->operand.get())) err("`&` 需要一个可寻址的对象", e);
            TypePtr t = check_expr(e->operand.get());
            if (auto m = dynamic_cast<Member*>(e->operand.get()); m && m->kind == "enum_const") {
                err("枚举成员不是可寻址的对象，不能取地址", e);
            }
            return Type::make_ptr(t);
        }

        TypePtr t = check_expr(e->operand.get());

        // 枚举不是算术类型：一元运算（`~`/`-`/`+`）需要显式 `as int`
        if (is_enum_t(t) && (e->op == "~" || e->op == "-" || e->op == "+")) {
            err("枚举 `" + t->str() + "` 不支持一元 `" + e->op + "`（请写 `x as int`）", e);
        }

        if (e->op == "*") {
            if (t && t->kind == TypeKind::Ptr) return t->elem;
            err("`*` 需要指针，实际是 " + (t ? t->str() : "void"), e);
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
                    err("类型 `" + t->str() + "` 没有一元 `~` 运算符重载", e);
                }
                auto f = choose_overload(unary, {e->operand.get()}, "operator~", e);
                e->op_func = f;
                return f->rty;
            }
            const std::string hint = (is_prim(t) && t->name == "bool")
                                         ? "（对 bool 取逻辑非请用 `not`）"
                                         : "";
            err("`~` 是按位取反，需要整数" + hint, e);
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
                    err("类型 `" + t->str() + "` 没有一元 `" + e->op + "` 运算符重载", e);
                }
                auto f = choose_overload(unary, {e->operand.get()}, "operator" + e->op, e);
                e->op_func = f;
                return f->rty;
            }
            err("`" + e->op + "` 需要数值操作数，实际是 " + (t ? t->str() : "void"), e);
        }
        err("未知一元运算符 `" + e->op + "`", e);
    }

    // 条件表达式 `cond ? a : b`（SPEC §5.2）：
    // 条件必须是 bool；两侧类型取「同一类型 → 隐式加宽 → null 适配」，都不成立则要求显式 `as`。
    TypePtr check_ternary(Ternary* e, const TypePtr& expected = nullptr) {
        TypePtr ct = check_expr(e->cond.get());
        if (!(is_prim(ct) && ct->name == "bool")) {
            err("`?:` 的条件必须是 bool，实际是 " + (ct ? ct->str() : "void"),
                e->cond.get());
        }
        // 期望类型透传给两个分支：字面量按上下文适配（`char c = pick ? 'a' : 'b';`）
        TypePtr a = check_expr(e->then_e.get(), expected);
        TypePtr b = check_expr(e->else_e.get(), expected);
        if (!a || !b) err("`?:` 缺少分支表达式", e);
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
        err("`?:` 两侧类型不一致（" + a->str() + " 与 " + b->str() +
                "）；请用 `as` 显式转换其中一侧",
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
        if (!lt || !rt) err("二元运算缺少操作数", e);

        if (is_user_type(lt) || is_user_type(rt)) {
            if (op != "==" && op != "!=") {
                auto cands = collect_operators(op, {lt, rt});
                if (cands.empty()) {
                    err("类型 `" + lt->str() + "` 与 `" + rt->str() + "` 没有运算符 `" + op +
                            "` 重载",
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
                err("枚举 `" + en->str() + "` 只能与同一枚举类型的值比较/运算；枚举与整数" +
                        "之间没有隐式转换（请写 `as`）",
                    e);
            }
            if (op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" &&
                op != ">=") {
                err("枚举 `" + lt->str() + "` 只支持 == != < <= > >=（算术/位运算请先 `as int`）", e);
            }
            return t_bool();
        }

        if (op == "+" || op == "-") {
            if (lt->kind == TypeKind::Ptr && is_integer(rt)) {
                if (lt->elem && lt->elem->kind == TypeKind::Void)
                    err("void* 不能做指针算术", e);
                return lt;
            }
            if (op == "+" && is_integer(lt) && rt->kind == TypeKind::Ptr) {
                if (rt->elem && rt->elem->kind == TypeKind::Void)
                    err("void* 不能做指针算术", e);
                return rt;
            }
            if (op == "-" && lt->kind == TypeKind::Ptr && rt->kind == TypeKind::Ptr) {
                if (!same_type(lt->elem, rt->elem)) err("指针相减要求元素类型相同", e);
                if (lt->elem && lt->elem->kind == TypeKind::Void)
                    err("void* 不能做指针算术", e);
                return Type::make_prim("isize");
            }
            TypePtr res = usual_arith(lt, rt);
            if (!res) err("`" + op + "` 不支持 " + lt->str() + " 与 " + rt->str(), e);
            return res;
        }

        if (op == "*" || op == "/" || op == "%") {
            TypePtr res = usual_arith(lt, rt);
            if (!res) err("`" + op + "` 不支持 " + lt->str() + " 与 " + rt->str(), e);
            return res;
        }

        if (op == "<<" || op == ">>") {
            if (!is_integer(lt) || !is_integer(rt)) err("`" + op + "` 两侧必须是整数", e);
            return lt;
        }

        if (op == "&" || op == "|" || op == "^") {
            TypePtr res = usual_arith(lt, rt);
            if (!res || !is_integer(res)) err("`" + op + "` 两侧必须是整数", e);
            return res;
        }

        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (lt->kind == TypeKind::Ptr && rt->kind == TypeKind::Ptr) return t_bool();
            if (is_numeric(lt) && is_numeric(rt)) return t_bool();
            err("`" + op + "` 不支持 " + lt->str() + " 与 " + rt->str(), e);
        }

        if (op == "==" || op == "!=") {
            if (lt->kind == TypeKind::Null || rt->kind == TypeKind::Null) {
                const TypePtr& other = (lt->kind == TypeKind::Null) ? rt : lt;
                if (is_str(other)) {
                    err("string 不能与 null 比较（用 len(s) == 0 或 s == \"\"）", e);
                }
                if (other->kind == TypeKind::Slice) {
                    err("切片不能与 null 比较（用 len(s) == 0）", e);
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
                    err("类型 `" + lt->str() + "` 与 `" + rt->str() + "` 没有运算符 `" + op +
                            "` 重载",
                        e);
                }
                auto f = choose_overload(cands, {e->left.get(), e->right.get()},
                                         "operator" + op, e);
                e->op_func = f;
                return f->rty;
            }
            err("`" + op + "` 不支持 " + lt->str() + " 与 " + rt->str(), e);
        }

        err("未知二元运算符 `" + op + "`", e);
    }

    TypePtr check_cond_type(Expr* e, const std::string& what) {
        TypePtr t = check_expr(e);
        if (!(is_prim(t) && t->name == "bool")) {
            err(what + " 需要 bool，实际是 " + (t ? t->str() : "void"), e);
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
        if (!callee) err("调用缺少被调用者", c);

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
                    auto sig = choose_method(ms, raw_args(c->args), "方法 `" + nm->ident + "`", c);
                    check_method_args(c, sig, "方法 `" + nm->ident + "`");
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
                err("`" + nm->ident + "` 是变量，不是函数", c);
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
                                                   "函数 `" + nm->ident + "`", c);
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
                            err("类型 `" + nm->ident + "` 有歧义：`" + src + "` 与 `" +
                                    use->unit_name + "`；请写限定名",
                                c);
                        }
                        st = t;
                        src = use->unit_name;
                    }
                }
                if (!st && std_info()) st = find_type_named("std.string", nm->ident, true);
                if (st) {
                    if (st->kind != TypeKind::Struct) {
                        err("`" + nm->ident + "` 不是 struct，不能按值构造", c);
                    }
                    struct_ty = st;
                } else if (lookup_enum_const(nm->ident) != nullptr) {
                    err("`" + nm->ident + "` 是枚举成员，不能调用", c);
                } else {
                    err("未定义的函数 `" + nm->ident + "`", c);
                }
            }
        } else if (auto m = dynamic_cast<Member*>(callee)) {
            Expr* obj = m->obj.get();
            // super.method(...)
            if (auto on = dynamic_cast<Name*>(obj);
                on && on->ident == "super" && lookup_local("super") == nullptr) {
                if (!cur_class_ || !cur_class_->base) err("当前类没有基类", c);
                std::vector<const MethodSig*> ms;
                collect_class_methods(cur_class_->base, m->name, ms);
                if (ms.empty()) err("基类没有方法 `" + m->name + "`", c);
                auto sig = choose_method(ms, raw_args(c->args), "方法 `" + m->name + "`", c);
                if (sig->is_abstract) {
                    err("不能通过 `super` 调用抽象方法 `" + m->name + "`", c);
                }
                if (!decl_visible(sig->owner ? sig->owner->unit : std::string(), sig->is_public)) {
                    err("方法 `" + m->name + "` 未声明为 public，不能从其他 unit 调用", c);
                }
                check_method_args(c, sig, "方法 `" + m->name + "`");
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
                        err("类型 `" + ot->str() + "` 没有方法 `" + m->name + "`", c);
                    }
                    auto sig = choose_method(ms, raw_args(c->args), "方法 `" + m->name + "`", c);
                    if (!decl_visible(sig->owner ? sig->owner->unit : std::string(), sig->is_public)) {
                        err("方法 `" + m->name + "` 未声明为 public，不能从其他 unit 调用", c);
                    }
                    check_method_args(c, sig, "方法 `" + m->name + "`");
                    m->kind = "method";
                    c->callee_kind = "method";
                    c->method = sig;
                    return sig->ret;
                }
                if (ot && ot->kind == TypeKind::Struct) {
                    std::vector<const MethodSig*> ms;
                    collect_methods(ot, m->name, ms);
                    if (ms.empty()) {
                        err("struct `" + ot->str() + "` 没有方法 `" + m->name + "`", c);
                    }
                    auto sig = choose_method(ms, raw_args(c->args), "方法 `" + m->name + "`", c);
                    if (!decl_visible(sig->owner ? sig->owner->unit : std::string(), sig->is_public)) {
                        err("方法 `" + m->name + "` 未声明为 public，不能从其他 unit 调用", c);
                    }
                    check_method_args(c, sig, "方法 `" + m->name + "`");
                    m->kind = "method";
                    c->callee_kind = "struct_method";
                    c->method = sig;
                    return sig->ret;
                }
                err("`" + m->name + "` 不是函数", c);
            }
            if (!unit_imported(uname)) err("未通过 `use` 导入 unit `" + uname + "`", c);
            if (!unit_visible(uname)) {
                err("unit `" + uname + "` 仅在库内可见，跨库使用需要 `public unit`", c);
            }

            UnitInfo& ui = infos_.at(uname);
            auto f = ui.funcs.find(m->name);
            if (f == ui.funcs.end() || f->second.empty()) {
                err("unit `" + uname + "` 中没有函数 `" + m->name + "`", c);
            }
            FuncDecl* chosen = choose_overload(f->second, raw_args(c->args),
                                               "函数 `" + uname + "." + m->name + "`", c);
            if (!chosen->is_public) {
                err("函数 `" + uname + "." + m->name + "` 未声明为 public", c);
            }
            auto nm = dynamic_cast<Name*>(obj);
            nm->kind = "unit";
            nm->ty = Type::make_module(uname);
            m->kind = "func";
            m->sym = chosen;
            target = chosen;
        } else {
            err("只能调用函数或结构体构造", c);
        }

        if (struct_ty) {
            if (!struct_ty->ctors.empty()) {
                if (!c->named.empty()) err("struct 构造器不支持命名参数", c);
                if (struct_ty->has_dtor && c != direct_init_expr_) {
                    err("含析构器的 struct 构造只能用于声明初始化", c);
                }
                const auto* chosen = choose_ctor(struct_ty->ctors, raw_args(c->args),
                                                 "struct `" + struct_ty->str() + "`", c);
                const size_t ci = static_cast<size_t>(chosen - struct_ty->ctors.data());
                if (ci < struct_ty->ctor_is_public.size() && !struct_ty->ctor_is_public[ci] &&
                    (!cur_ || cur_->name != struct_ty->unit)) {
                    err("构造器未声明为 public，不能从其他 unit 调用", c);
                }
                for (size_t i = 0; i < c->args.size(); ++i) {
                    check_assign(c->args[i].get(), (*chosen)[i], "构造参数");
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

        if (!target) err("只能调用函数或结构体构造", c);
        c->callee_kind = "func";
        c->func = target;
        const auto& params = target->pty;

        if (!c->named.empty()) {
            err("函数调用不支持命名参数（只有结构体构造支持）", c);
        }
        if (c->args.size() != params.size()) {
            err("函数 `" + target->name + "` 需要 " + std::to_string(params.size()) +
                    " 个参数，实际 " + std::to_string(c->args.size()) + " 个",
                c);
        }
        for (size_t i = 0; i < c->args.size(); ++i) {
            check_assign(c->args[i].get(), params[i], "调用 `" + target->name + "` 的参数");
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
                    check_assign(args[i], cands[0]->pty[i], what + " 的参数");
                }
            }
            err("没有匹配的 " + what + " 重载", at);
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
        if (best < 0) err("`" + what + "` 的重载调用有歧义", at);
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
        if (ok.empty()) err("没有匹配的 " + what + " 重载", at);

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
        if (best < 0) err("`" + what + "` 的重载调用有歧义", at);
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
        if (ok.empty()) err(what + " 没有匹配的构造器", at);

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
        if (best < 0) err("`" + what + "` 的构造器调用有歧义", at);
        return ok[static_cast<size_t>(best)];
    }

    void check_method_args(Call* c, const MethodSig* sig, const std::string& what) {
        if (!c->named.empty()) err("方法调用不支持命名参数", c);
        if (c->args.size() != sig->params.size()) {
            err(what + " 需要 " + std::to_string(sig->params.size()) + " 个参数，实际 " +
                    std::to_string(c->args.size()) + " 个",
                c);
        }
        for (size_t i = 0; i < c->args.size(); ++i) {
            check_assign(c->args[i].get(), sig->params[i], what + " 的参数");
        }
    }

    TypePtr check_super_call(Call* c) {
        if (!in_ctor_ || !super_ok_) {
            err("`super(...)` 必须是构造器的第一条语句", c);
        }
        if (!cur_class_ || !cur_class_->base) err("当前类没有基类", c);
        if (!c->named.empty()) err("构造器调用不支持命名参数", c);
        const auto& ctors = cur_class_->base->ctors;
        if (ctors.empty()) {
            if (!c->args.empty()) err("基类没有匹配的构造器", c);
        } else {
            const auto* chosen = choose_ctor(ctors, raw_args(c->args), "基类", c);
            for (size_t i = 0; i < c->args.size(); ++i) {
                check_assign(c->args[i].get(), (*chosen)[i], "super 参数");
            }
            c->ctor_pty = chosen;
        }
        c->callee_kind = "super";
        return t_void();
    }

    [[noreturn]] void err_slice_elem_align(const TypePtr& elem, const Node* node) const {
        err("类型 `" + elem->str() + "` 的 align(" + std::to_string(type_align(elem)) +
                ") 超过 8：动态数组数据区在 24 字节头部之后，元素最多 8 字节对齐；"
                "请改用定长数组/指针，或降低 align(N)",
            node);
    }

    TypePtr check_new(NewExpr* ne) {
        TypePtr t = resolve_type(ne->type.get(), cur_);
        if (ne->is_array) {
            if (is_void(t)) err("动态数组元素不能是 void", ne);
            if (ne->dims.empty()) err("动态数组缺少长度", ne);
            for (auto& d : ne->dims) {
                TypePtr ct = check_expr(d.get());
                if (!(ct && ct->is_integer())) {
                    err("动态数组长度必须是整数", d.get());
                }
            }
            if (t->kind == TypeKind::Struct && t->has_dtor) {
                err("含析构器的 struct `" + t->str() +
                        "` 暂不能作动态数组元素（元素构造/析构未实现）",
                    ne);
            }
            if (!ne->args.empty() || !ne->named.empty()) {
                err("动态数组不能带构造器参数", ne);
            }
            ne->class_ty = t;
            if (ne->jagged_rank > 0) {
                if (ne->dims.size() != 1) {
                    err("锯齿数组 `new T[m][]` 一次只能创建一维；"
                        "二维请循环填行，矩形数组用 `new T[m,n]`",
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
        if (t->kind != TypeKind::Class) err("`new` 只能用于 class 或动态数组", ne);
        if (t->is_abstract) err("抽象类 `" + t->str() + "` 不能 `new`", ne);
        if (!ne->named.empty()) err("构造器暂不支持命名参数", ne);

        const auto& ctors = t->ctors;
        ne->has_ctor = !ctors.empty();
        if (ctors.empty()) {
            if (!ne->args.empty()) err("class `" + t->str() + "` 没有匹配的构造器", ne);
        } else {
            const auto* chosen = choose_ctor(ctors, raw_args(ne->args), "class `" + t->str() + "`", ne);
            const size_t idx = static_cast<size_t>(chosen - ctors.data());
            if (idx < t->ctor_is_public.size() && !t->ctor_is_public[idx] &&
                (!cur_ || cur_->name != t->unit)) {
                err("构造器未声明为 public，不能从其他 unit `new`", ne);
            }
            for (size_t i = 0; i < ne->args.size(); ++i) {
                check_assign(ne->args[i].get(), (*chosen)[i], "构造参数");
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
                    err("struct `" + ty->str() + "` 的字段 `" + f.name +
                            "` 未声明为 public，不能从其他 unit 构造",
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
                    err("结构体 `" + ty->str() + "` 没有字段 `" + n + "`", c);
                }
                if (!seen.insert(n).second) err("字段 `" + n + "` 重复初始化", c);
                const size_t idx = it->second;
                order.push_back(static_cast<int>(idx));
                check_assign(e.get(), fields[idx].type, "构造 `" + ty->str() + "`");
            }
            if (order.size() != fields.size()) {
                err("构造 `" + ty->str() + "` 必须给全部字段", c);
            }
            c->field_order = std::move(order);
            return;
        }

        if (c->args.size() != fields.size()) {
            err("构造 `" + ty->str() + "` 需要 " + std::to_string(fields.size()) +
                    " 个参数，实际 " + std::to_string(c->args.size()) + " 个",
                c);
        }
        c->field_order.clear();
        for (size_t i = 0; i < fields.size(); ++i) {
            check_assign(c->args[i].get(), fields[i].type, "构造 `" + ty->str() + "`");
            c->field_order.push_back(static_cast<int>(i));
        }
    }

    TypePtr check_builtin(Call* c, const std::string& name) {
        c->callee_kind = "builtin";
        c->builtin = name;
        if (!c->named.empty()) err("内建函数 `" + name + "` 不支持命名参数", c);

        if (name == "alloc") {
            if (c->args.size() != 1) err("`alloc(bytes)` 需要 1 个参数", c);
            if (!is_integer(check_expr(c->args[0].get()))) {
                err("`alloc` 的字节数必须是整数", c);
            }
            return Type::make_ptr(t_void());
        }
        if (name == "realloc") {
            if (c->args.size() != 2) err("`realloc(p, bytes)` 需要 2 个参数", c);
            TypePtr p0 = check_expr(c->args[0].get());
            if (!p0 || p0->kind != TypeKind::Ptr) {
                err("`realloc` 的第一个参数必须是指针", c);
            }
            if (!is_integer(check_expr(c->args[1].get()))) {
                err("`realloc` 的字节数必须是整数", c);
            }
            return Type::make_ptr(t_void());
        }
        if (name == "free") {
            if (c->args.size() != 1) err("`free(p)` 需要 1 个参数", c);
            TypePtr p0 = check_expr(c->args[0].get());
            if (!p0 || (p0->kind != TypeKind::Ptr && p0->kind != TypeKind::Null)) {
                err("`free` 的参数必须是指针", c);
            }
            return t_void();
        }
        err("未知内建函数 `" + name + "`", c);
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
            return !(nm && infos_.count(nm->ident));
        }
        if (auto u = dynamic_cast<Unary*>(e)) return u->op == "*";
        return false;
    }

    void check_entry() {
        auto it = infos_.find(entry_);
        if (it == infos_.end()) throw SemaError("找不到入口 unit `" + entry_ + "`");
        UnitInfo& ui = it->second;
        // Python 版这里用 self.cur（此时是最后一个被检查的 unit）取 filename，
        // 可能指错文件；这里显式指向入口 unit。
        cur_ = ui.unit;

        auto f = ui.funcs.find("main");
        if (f == ui.funcs.end() || f->second.empty()) {
            throw SemaError("unit `" + entry_ + "` 中没有 `main` 函数");
        }
        FuncDecl* main = nullptr;
        for (auto* cand : f->second) {
            if (cand->params.empty()) {
                main = cand;
                break;
            }
        }
        if (!main) main = f->second.front();
        if (!main->rty) err("`main` 缺少返回类型", main);
        if (main->rty->str() != "int") err("`main` 必须返回 int", main);
        if (!main->params.empty()) err("第一阶段 `main` 不能带参数", main);
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


