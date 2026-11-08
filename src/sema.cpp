// 语义分析：名字解析、类型检查、类型注解
//
// 流程与 Python 版一致：
//   collect -> place_structs -> resolve_unit_types -> check_unit -> check_entry
// 所有解析结果直接写回 AST 节点（契约见 ast.hpp 顶部）。
#include "sema.hpp"

#include <cstddef>
#include <deque>
#include <functional>
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
            return spec_text(s->elem.get()) + "[]";
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
        check_hierarchy_cycles();
        compute_abstract_all();
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

    // ------------------------------------------------------------ 工具

    [[noreturn]] void err(const std::string& msg, const Node* node = nullptr) const {
        const int line = node ? node->line : 0;
        const int col = node ? node->col : 0;
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string());
    }

    // TypeSpec 不是 Node（自带 line/col），单独一个报错入口。
    [[noreturn]] void err_ts(const std::string& msg, const TypeSpec* spec) const {
        const int line = spec ? spec->line : 0;
        const int col = spec ? spec->col : 0;
        throw SemaError(msg, line, col, cur_ ? cur_->filename : std::string());
    }

    UnitInfo& info() { return infos_.at(cur_->name); }

    bool uses_unit(const std::string& uname) const {
        if (!cur_) return false;
        for (const auto& use : cur_->uses) {
            if (use->unit_name == uname) return true;
        }
        return false;
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
                if (auto fn = dynamic_cast<FuncDecl*>(node)) {
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

    void resolve_unit_types(Unit* u) {
        cur_ = u;

        // 结构体：占位类型已就绪，这里填字段（字段可引用别的结构体/枚举）
        for (auto& d : u->decls) {
            auto sd = dynamic_cast<StructDecl*>(d.get());
            if (!sd) continue;
            if (!sd->ty) sd->ty = Type::make_struct(u->name, sd->name);
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
                if (contains_reference_type(ft)) {
                    err("struct 字段不能包含 class/interface 引用（见 3.8）", sd);
                }
                if (spec->weak) err("`weak` 只能用于 class 字段", sd);
                fields.push_back(Field{fname, ft});
            }
            sd->ty->fields = std::move(fields);
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
                fields.push_back(Field{fname, ft, spec->weak});
            }
            c->ty->fields = std::move(fields);

            c->ty->methods.clear();
            c->ty->is_abstract = false;
            for (auto& m : c->methods) {
                m->rty = resolve_type(m->ret.get(), u);
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("参数不能是 void", p.get());
                    m->pty.push_back(pt);
                }
                MethodSig sig;
                sig.name = m->name;
                sig.ret = m->rty;
                sig.params = m->pty;
                sig.is_abstract = !m->body;
                sig.is_override = m->is_override;
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
                    ctor->pty.push_back(pt);
                }
                ctor->cname = ctor_cname(u->name, c->name, ctor->pty);
                c->ty->ctors.push_back(ctor->pty);
            }
            if (c->dtor) c->dtor->cname = dtor_cname(u->name, c->name);
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
                m->pty.clear();
                for (auto& p : m->params) {
                    TypePtr pt = resolve_type(p->spec.get(), u);
                    if (is_void(pt)) err("参数不能是 void", p.get());
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
            f->pty.clear();
            for (auto& p : f->params) {
                TypePtr pt = resolve_type(p->spec.get(), u);
                if (is_void(pt)) err("参数 `" + p->name + "` 不能是 void", p.get());
                f->pty.push_back(pt);
            }
            if (f->is_extern) {
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
            case TypeSpec::K::Named:
                return resolve_named_type(spec->name, u, spec);
            case TypeSpec::K::Ptr: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_reference_type(elem)) {
                    err_ts("暂不支持指向 class/interface 的指针（引用类型本身已是句柄）", spec);
                }
                return Type::make_ptr(elem, spec->readonly);
            }
            case TypeSpec::K::Slice: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_reference_type(elem)) {
                    err_ts("暂不支持 class/interface 切片（用静态数组或容器类）", spec);
                }
                return Type::make_slice(elem, spec->readonly);
            }
            case TypeSpec::K::Array: {
                TypePtr elem = resolve_type(spec->elem.get(), u);
                if (is_void(elem)) err_ts("数组元素不能是 void", spec);
                return Type::make_array(elem, spec->size);
            }
        }
        err_ts("未知类型 " + spec_text(spec), spec);
    }

    TypePtr resolve_named_type(const std::string& name, Unit* u, const TypeSpec* spec) {
        if (name == "void") return t_void();
        if (name == "string") return t_string();
        if (is_prim_name(name)) return Type::make_prim(name);

        const auto dot = name.find('.');
        if (dot != std::string::npos) {
            const std::string un = name.substr(0, dot);
            const std::string tn = name.substr(dot + 1);
            auto it = infos_.find(un);
            if (it == infos_.end()) err_ts("未导入的 unit `" + un + "`", spec);
            if (un != u->name && !uses_unit(un)) {
                err_ts("未通过 `use` 导入 unit `" + un + "`", spec);
            }
            UnitInfo& ui = it->second;
            if (auto s = ui.structs.find(tn); s != ui.structs.end()) return s->second->ty;
            if (auto e = ui.enums.find(tn); e != ui.enums.end()) return e->second->ty;
            if (auto c = ui.classes.find(tn); c != ui.classes.end()) return c->second->ty;
            if (auto i = ui.interfaces.find(tn); i != ui.interfaces.end()) return i->second->ty;
            err_ts("unit `" + un + "` 中没有类型 `" + tn + "`", spec);
        }

        UnitInfo& ui = infos_.at(u->name);
        if (auto s = ui.structs.find(name); s != ui.structs.end()) return s->second->ty;
        if (auto e = ui.enums.find(name); e != ui.enums.end()) return e->second->ty;
        if (auto c = ui.classes.find(name); c != ui.classes.end()) return c->second->ty;
        if (auto i = ui.interfaces.find(name); i != ui.interfaces.end()) return i->second->ty;
        err_ts("未知类型 `" + name + "`", spec);
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

    EnumConst* lookup_enum_const(const std::string& name) {
        // 按声明顺序遍历（Python 的 dict 保留插入顺序）
        for (auto& d : cur_->decls) {
            auto ed = dynamic_cast<EnumDecl*>(d.get());
            if (!ed) continue;
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
                if (m == name) return intern_enum_const(cur_->name, ed->name, m, value, ed->ty);
            }
        }
        return nullptr;
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
            if (auto i = dynamic_cast<InterfaceDecl*>(d.get())) check_interface(i);
        }
    }

    void check_global(GlobalVar* g) {
        if (!g->rty) err("全局变量 `" + g->name + "` 缺少类型", g);
        if (g->is_extern) {
            if (g->init) err("`extern` 变量 `" + g->name + "` 不能有初始化器", g);
            return;
        }
        if (!g->init) {
            if (g->is_const) err("`const` 变量 `" + g->name + "` 必须初始化", g);
            g->zero = true;
            return;
        }
        if (auto ie = dynamic_cast<InitExpr*>(g->init.get())) {
            if (auto il = dynamic_cast<IntLit*>(ie->expr.get())) {
                if (il->value == 0 && is_zeroable(g->rty)) {
                    g->zero = true;
                    il->ty = t_int();
                    return;
                }
            }
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
        return false;
    }

    void check_operator_decl(FuncDecl* f) {
        const std::string sym = f->name.substr(8);
        const size_t n = f->pty.size();
        const bool unary_or_binary = (sym == "-" || sym == "+");
        if (sym == "[]") {
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
        } else if (dynamic_cast<BreakStmt*>(s)) {
            if (loop_depth_ == 0) err("`break` 只能出现在循环中", s);
        } else if (dynamic_cast<ContinueStmt*>(s)) {
            if (loop_depth_ == 0) err("`continue` 只能出现在循环中", s);
        } else if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            check_return(rs);
        } else if (auto d = dynamic_cast<DelStmt*>(s)) {
            TypePtr t = check_expr(d->value.get());
            if (!is_reference_type(t)) err("`del` 只能用于 class/interface 引用", d);
            if (!is_lvalue(d->value.get())) err("`del` 的目标必须是可写的引用左值", d);
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

        declare_local(lv);
        if (lv->init) check_decl_init(lv->zero, lv->rty, lv->init.get(), "初始化", lv);
    }

    void check_decl_init(bool& zero_flag, const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node) {
        if (!init) err("缺少初始化器", node);
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            if (auto il = dynamic_cast<IntLit*>(ie->expr.get())) {
                if (il->value == 0 && is_zeroable(ty)) {
                    zero_flag = true;
                    il->ty = t_int();
                    return;
                }
            }
        }
        check_init_into(ty, init, what, node);
    }

    // 单个初始化位置：声明初始化、聚合成员、嵌套花括号都走这里
    void check_init_into(const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node) {
        if (!init) err("缺少初始化器", node);
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            Expr* e = ie->expr.get();
            if (!e) err("缺少初始化表达式", node);
            if (auto il = dynamic_cast<IntLit*>(e)) {
                if (il->value == 0 && is_zeroable(ty)) {
                    il->ty = t_int();
                    return;
                }
            }
            if (ty->kind == TypeKind::Array && is_char(ty->elem) &&
                dynamic_cast<StringLit*>(e)) {
                check_expr(e, t_string());
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

        if (ty->kind == TypeKind::Struct) {
            const auto& fields = ty->fields;
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
            if (bt && bt->kind == TypeKind::String) err("string 不可变，不能写元素", t);
            if (bt && bt->kind == TypeKind::Slice && bt->readonly)
                err("不能通过 readonly 切片写入", t);
            if (bt && bt->kind == TypeKind::Ptr && bt->readonly)
                err("不能通过 readonly 指针写入", t);
            if (root_is_const(ix->base.get())) err("不能修改 const 绑定的元素", t);
            return;
        }
        if (auto u = dynamic_cast<Unary*>(t)) {
            if (u->op == "*") {
                TypePtr ot = u->operand->ty;
                if (ot && ot->kind == TypeKind::Ptr && ot->readonly)
                    err("不能通过 readonly 指针写入", t);
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
            TypePtr it = check_expr(ix->index.get());
            if (is_user_type(bt)) {
                if (!is_integer(it)) err("下标必须是整数", s->target.get());
                if (s->op != "=") err("用户下标暂只支持 `=` 赋值", s);
                auto cands = collect_operators("[]=", {bt, it});
                if (cands.empty()) {
                    err("类型 `" + bt->str() + "` 不支持下标赋值（没有 `operator[]=`）", s);
                }
                check_writable(s->target.get());
                s->set_func = choose_overload(cands,
                                              {ix->base.get(), ix->index.get(), s->value.get()},
                                              "operator[]=", s);
                return;
            }
        }

        TypePtr tt = check_expr(s->target.get());
        check_writable(s->target.get());
        if (s->op == "=") {
            check_assign(s->value.get(), tt, "赋值");
            return;
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

    void check_for(ForInStmt* s) {
        ScopeGuard guard(*this);

        if (auto r = dynamic_cast<RangeExpr*>(s->seq.get())) {
            TypePtr lt = check_expr(r->lo.get());
            TypePtr ht = check_expr(r->hi.get());
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
            } else if (st->kind == TypeKind::String) {
                s->loop_kind = "string";
                s->elem_ty = Type::make_prim("char");
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
        if (s->by_ref && s->seq_ty && s->seq_ty->kind == TypeKind::Slice && s->seq_ty->readonly) {
            err("不能对 readonly 切片使用 `for ref`", s->seq.get());
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
        return false;
    }

    static bool is_zeroable(const TypePtr& t) {
        if (!t) return false;
        return t->kind == TypeKind::Struct || t->kind == TypeKind::Array ||
               t->kind == TypeKind::Slice || t->kind == TypeKind::String;
    }

    static bool is_char(const TypePtr& t) {
        if (!is_prim(t)) return false;
        return t->name == "char" || t->name == "ubyte" || t->name == "byte";
    }

    TypePtr check_assign(Expr* e, const TypePtr& dst, const std::string& what) {
        TypePtr src = check_expr(e, dst);
        if (assignable(e, src, dst)) return src;
        err(what + "：不能把 " + (src ? src->str() : "void") + " 赋给 " +
                (dst ? dst->str() : "void"),
            e);
    }

    bool assignable(Expr* e, const TypePtr& src, const TypePtr& dst) {
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
        if (dynamic_cast<NullLit*>(e) && dst) {
            if (dst->kind == TypeKind::Ptr || dst->kind == TypeKind::Slice ||
                dst->kind == TypeKind::String || dst->kind == TypeKind::Class ||
                dst->kind == TypeKind::Interface) {
                e->ty = dst;
                return true;
            }
        }
        if (src && dst && src->kind == TypeKind::Array && dst->kind == TypeKind::Slice &&
            same_type(src->elem, dst->elem)) {
            return true;
        }
        return implicitly_convertible(src, dst);
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

        if (dynamic_cast<CharLit*>(e)) return Type::make_prim("char");
        if (dynamic_cast<StringLit*>(e)) return t_string();
        if (dynamic_cast<BoolLit*>(e)) return t_bool();

        if (dynamic_cast<NullLit*>(e)) {
            if (expected) {
                const auto k = expected->kind;
                if (k == TypeKind::Ptr || k == TypeKind::Slice || k == TypeKind::String ||
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
            TypePtr it = check_expr(ix->index.get());
            if (!is_integer(it)) {
                err("下标必须是整数，实际是 " + (it ? it->str() : "void"), ix->index.get());
            }
            if (!bt) err("类型不能下标访问", e);
            if (bt->kind == TypeKind::Array || bt->kind == TypeKind::Slice ||
                bt->kind == TypeKind::Ptr) {
                return bt->elem;
            }
            if (bt->kind == TypeKind::String) return Type::make_prim("char");
            if (is_user_type(bt)) {
                auto cands = collect_operators("[]", {bt, it});
                if (cands.empty()) {
                    err("类型 `" + bt->str() + "` 不支持下标（没有 `operator[]`）", e);
                }
                auto f = choose_overload(cands, {ix->base.get(), ix->index.get()},
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
            if (t && (t->kind == TypeKind::Array || t->kind == TypeKind::Slice ||
                      t->kind == TypeKind::String)) {
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
            err("`this` 只能出现在 class 方法/构造器/析构器中", e);
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
        err("未定义的标识符 `" + e->ident + "`", e);
    }

    TypePtr check_member(Member* e) {
        Expr* obj = e->obj.get();
        if (!obj) err("缺少成员访问的对象", e);

        if (auto nm = dynamic_cast<Name*>(obj); nm && lookup_local(nm->ident) == nullptr) {
            const std::string uname = nm->ident;
            auto it = infos_.find(uname);
            if (it != infos_.end() && uname != cur_->name) {
                if (!uses_unit(uname)) err("未通过 `use` 导入 unit `" + uname + "`", e);
                UnitInfo& ui = it->second;
                nm->kind = "unit";
                nm->ty = Type::make_module(uname);
                if (auto f = ui.funcs.find(e->name); f != ui.funcs.end() && !f->second.empty()) {
                    e->kind = "func";
                    e->sym = f->second.front();
                    err("函数 `" + uname + "." + e->name + "` 不能作为值使用", e);
                }
                if (auto g = ui.globals.find(e->name); g != ui.globals.end()) {
                    e->kind = "global";
                    e->sym = g->second;
                    return g->second->rty;
                }
                err("unit `" + uname + "` 中没有 `" + e->name + "`", e);
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
            e->kind = "field";
            e->field_index = ot->field_index(e->name);
            return ft;
        }
        err("类型 " + (ot ? ot->str() : "void") + " 没有成员 `" + e->name + "`", e);
    }

    TypePtr check_unary(Unary* e) {
        if (e->op == "&") {
            if (!is_lvalue(e->operand.get())) err("`&` 需要一个可寻址的对象", e);
            TypePtr t = check_expr(e->operand.get());
            return Type::make_ptr(t);
        }

        TypePtr t = check_expr(e->operand.get());

        if (e->op == "*") {
            if (t && t->kind == TypeKind::Ptr) return t->elem;
            err("`*` 需要指针，实际是 " + (t ? t->str() : "void"), e);
        }
        if (e->op == "!") {
            if (!is_integer(t)) {
                const std::string hint = (is_prim(t) && t->name == "bool")
                                             ? "（对 bool 取逻辑非请用 `not`）"
                                             : "";
                err("`!` 是按位取反，需要整数" + hint, e);
            }
            return t;
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
                if (other->kind == TypeKind::String) {
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
            if (lt->kind == TypeKind::String && rt->kind == TypeKind::String) return t_bool();
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
        auto il = dynamic_cast<IntLit*>(e);
        if (!il) return;
        if (is_prim(other) && other->is_numeric() && promote_int_literal(il->value, other)) {
            il->ty = other;
        }
    }

    bool can_cast(const TypePtr& src, const TypePtr& dst) {
        if (!src || !dst) return false;
        if (same_type(src, dst)) return true;
        if (is_numeric(src) && is_numeric(dst)) return true;
        // 引用类型：向上/向下转换（向下在运行期检查，失败得 null）
        if (is_class_type(src) && is_class_type(dst)) {
            return is_subclass_of(src, dst) || is_subclass_of(dst, src);
        }
        if (is_interface_type(src) && is_interface_type(dst)) {
            return implements_interface(src, dst);  // 仅向上；向下暂缓
        }
        if (is_interface_type(src) && is_class_type(dst)) return true;
        const bool sp = src->kind == TypeKind::Ptr || src->kind == TypeKind::Slice;
        const bool dp = dst->kind == TypeKind::Ptr || dst->kind == TypeKind::Slice;
        if (sp && dp) return true;
        if (src->kind == TypeKind::Ptr && is_integer(dst) && dst->bits() >= 64) return true;
        if (dst->kind == TypeKind::Ptr && is_integer(src) && src->bits() >= 64) return true;
        if (src->kind == TypeKind::String && dst->kind == TypeKind::Ptr) return true;
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
                    c->callee_kind = "method";
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
            UnitInfo& ui = info();
            auto f = ui.funcs.find(nm->ident);
            if (f != ui.funcs.end() && !f->second.empty()) {
                FuncDecl* chosen = choose_overload(f->second, raw_args(c->args),
                                                   "函数 `" + nm->ident + "`", c);
                nm->kind = "func";
                nm->sym = chosen;
                target = chosen;
            } else {
                auto s = ui.structs.find(nm->ident);
                if (s != ui.structs.end()) {
                    struct_ty = s->second->ty;
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
                collect_methods(cur_class_->base, m->name, ms);
                if (ms.empty()) err("基类没有方法 `" + m->name + "`", c);
                auto sig = choose_method(ms, raw_args(c->args), "方法 `" + m->name + "`", c);
                if (sig->is_abstract) {
                    err("不能通过 `super` 调用抽象方法 `" + m->name + "`", c);
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
                    check_method_args(c, sig, "方法 `" + m->name + "`");
                    m->kind = "method";
                    c->callee_kind = "method";
                    c->method = sig;
                    return sig->ret;
                }
                err("`" + m->name + "` 不是函数", c);
            }
            if (!uses_unit(uname)) err("未通过 `use` 导入 unit `" + uname + "`", c);

            UnitInfo& ui = infos_.at(uname);
            auto f = ui.funcs.find(m->name);
            if (f == ui.funcs.end() || f->second.empty()) {
                err("unit `" + uname + "` 中没有函数 `" + m->name + "`", c);
            }
            FuncDecl* chosen = choose_overload(f->second, raw_args(c->args),
                                               "函数 `" + uname + "." + m->name + "`", c);
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
                p->kind == TypeKind::String || p->kind == TypeKind::Class ||
                p->kind == TypeKind::Interface) {
                return 0;
            }
            return -1;
        }
        if (same_type(aty, p)) return 0;
        if (implicitly_convertible(aty, p)) return 1;
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

    TypePtr check_new(NewExpr* ne) {
        TypePtr t = resolve_type(ne->type.get(), cur_);
        if (t->kind != TypeKind::Class) err("`new` 只能用于 class", ne);
        if (t->is_abstract) err("抽象类 `" + t->str() + "` 不能 `new`", ne);
        if (!ne->named.empty()) err("构造器暂不支持命名参数", ne);

        const auto& ctors = t->ctors;
        ne->has_ctor = !ctors.empty();
        if (ctors.empty()) {
            if (!ne->args.empty()) err("class `" + t->str() + "` 没有匹配的构造器", ne);
        } else {
            const auto* chosen = choose_ctor(ctors, raw_args(ne->args), "class `" + t->str() + "`", ne);
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
