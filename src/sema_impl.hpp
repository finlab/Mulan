// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 语义分析内部实现：Sema 类声明与跨文件辅助函数声明（仅供 sema_*.cpp 使用）。
//
// 成员函数按功能分散在 sema_decl / sema_types / sema_class1 / sema_class2 /
// sema_const / sema_expr / sema_call 中；对外接口见 sema.hpp。
#pragma once

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

namespace muc_detail {


// ---------------------------------------------------------------- 小工具

inline bool is_integer(const TypePtr& t) { return t && t->is_integer(); }
inline bool is_numeric(const TypePtr& t) { return t && t->is_numeric(); }
inline bool is_void(const TypePtr& t) { return t && t->kind == TypeKind::Void; }
inline bool is_prim(const TypePtr& t) { return t && t->kind == TypeKind::Prim; }
inline bool is_enum_t(const TypePtr& t) { return t && t->kind == TypeKind::Enum; }
// 枚举只允许与「真正的整数类型」显式互转（bool 与枚举都不算）
inline bool is_plain_int(const TypePtr& t) { return is_prim(t) && t->is_integer(); }

// 类型语法文本化。ast.hpp 里声明了 TypeSpec::str()，但它的定义属于 parser.cpp；
// 这里自带一份，避免 sema 对 parser 的链接期依赖（只在错误信息里用）。
inline std::string spec_text(const TypeSpec* s) {
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
inline EnumConst* intern_enum_const(const std::string& unit, const std::string& enum_name,
                             const std::string& member, long long value, const TypePtr& ty) {
    static std::deque<EnumConst> pool;
    pool.push_back(EnumConst{unit, enum_name, member, value, ty});
    return &pool.back();
}

class Sema {
public:
    Sema(std::vector<std::unique_ptr<Unit>> units, std::string entry);

    Program run();

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
    std::vector<std::string> errors_;  // 收集到的错误（多错误报告）
    static constexpr size_t kMaxErrors = 20;
    int loop_depth_ = 0;
    int const_depth_ = 0;  // 常量求值递归深度（见 DepthGuard）
    std::unordered_map<int, TypePtr> auto_groups_;  // auto 同一声明的推断结果
    Expr* direct_init_expr_ = nullptr;  // 当前正在检查的直接初始化表达式（构造器就地构造）

    // ------------------------------------------------------------ 工具

    [[noreturn]] void err(const std::string& msg, const Node* node = nullptr) const;

    // 警告不中止编译，只打到 stderr（格式与错误一致，前缀 warning）。
    void warn(const std::string& msg, const Node* node) const;

    // TypeSpec 不是 Node（自带 line/col），单独一个报错入口。
    [[noreturn]] void err_ts(const std::string& msg, const TypeSpec* spec) const;

    UnitInfo& info();

    UnitInfo* std_info();

    // std.string 中的规范字符串结构体（名字就是 `string`）
    TypePtr str_type();

    bool is_str(const TypePtr& t);

    bool implicit_ok(const TypePtr& src, const TypePtr& dst);

    bool uses_unit(const std::string& uname) const;

    // std.string 始终作为隐式依赖加载，等价于已 `use`。
    bool unit_imported(const std::string& uname) const;

    // ------------------------------------------------------------ 可见性
    //
    // 库边界 = unit 名的第一段（`std.string` → `std`；无点号的 unit 属于根库）。
    // 两档：默认仅本 unit 可见；`public` 跨 unit 可见。跨库还需要目标 unit 本身 `public unit`。

    static std::string lib_of(const std::string& unit_name);

    // 目标 unit 是否可被当前 unit 引用：同库总是可以；跨库要求 `public unit`。
    bool unit_visible(const std::string& target) const;

    // 声明级可见性：本 unit 内全可见；其他 unit 要求声明为 public。
    bool decl_visible(const std::string& owner_unit, bool is_public) const;

    bool type_is_public(const TypePtr& t) const;

    // 类型声明可见性（带类型名，用于报错）。
    void require_type_visible(const TypePtr& t, const TypeSpec* spec) const;

    // ------------------------------------------------------------ 主流程

    // ------------------------------------------------------------ 收集
    void collect();

    // ------------------------------------------------------------ public 签名连锁检查
    //
    // public 声明对外暴露的类型也必须 public，且其 unit 必须可见；否则外部拿到签名却无法命名类型。

    void check_exposed_type(const TypePtr& t, const std::string& what, const Node* at);

    void check_public_signatures();

    void place_structs();

    void resolve_class_fields(ClassDecl* c, Unit* u);

    // ------------------------------------------------------------ 布局修饰符
    //
    // packed / align(N) 的最终布局由 types.cpp 的 type_align/type_size 计算；
    // 这里只负责求值 N、校验 N 与自然对齐的关系，以及 packed 的字段类型限制。

    bool eval_const_int(Expr* e, long long& out);

    static bool is_pow2(long long n);

    void check_layouts();

    // 类型级编译期常量：解析类型并校验「只允许标量 / 枚举」
    void resolve_const_members(std::vector<std::unique_ptr<ConstMember>>& list,
                               const TypePtr& owner, Unit* u, const std::string& kw,
                               const std::string& type_name);

    void resolve_unit_types(Unit* u);
    // ------------------------------------------------------------ 类型解析
    TypePtr resolve_type(const TypeSpec* spec, Unit* u);

    // 在指定 unit 中按名字找类型；require_pub 时只返回 public 声明。
    TypePtr find_type_named(const std::string& unit, const std::string& name,
                            bool require_pub) const;

    TypePtr resolve_named_type(const std::string& name, Unit* u, const TypeSpec* spec);

    // 表达式里的类型名解析（当前 unit → use 的 public 类型 → std.string），
    // 用于 `Color.Red` 这类枚举限定引用。找不到返回 nullptr。
    TypePtr find_type_in_expr(const std::string& name, const Node* at);

    // 在指定枚举里按名字找成员（顺序与声明一致，显式赋值后继续递增）。
    EnumConst* find_enum_member(const TypePtr& et, const std::string& member);

    // 在 struct / class（含基类链）里按名字找类型级编译期常量。
    ConstMember* find_const_member_in(const TypePtr& t, const std::string& name);

    // 类型级常量的可见性：public，或与声明它的类型处于同一 unit（与字段规则一致）
    bool const_member_visible(const ConstMember* cm) const;

    // 该类型（含基类链）是否声明了任意类型级常量
    bool has_any_const_member(const TypePtr& t);

    // ------------------------------------------------------------ 作用域
    // 收集错误并继续：单个声明/函数出错不应该阻止报告同一 unit 里其它的错误。
    // 出错后重置作用域等临时状态，避免下一条声明受污染。
    template <class F>
    void recover(F&& fn) {
        try {
            fn();
        } catch (const MuError& e) {
            errors_.push_back(e.what());
            scopes_.clear();
            loop_depth_ = 0;
            auto_groups_.clear();
            direct_init_expr_ = nullptr;
            if (errors_.size() >= kMaxErrors) throw MuErrorList(errors_);
        }
    }

    void push();
    void pop();

    LocalVar* lookup_local(const std::string& name) const;

    void declare_local(LocalVar* lv);

    EnumConst* lookup_enum_const_in(const std::string& unit_name, const std::string& name,
                                    bool require_pub);

    EnumConst* lookup_enum_const(const std::string& name);

    // ------------------------------------------------------------ unit 检查
    void check_unit(Unit* u);

    void check_global(GlobalVar* g);

    static bool guarantees_return(Stmt* s);

    void check_operator_decl(FuncDecl* f);

    void check_func(FuncDecl* f);
    // ------------------------------------------------------------ class / interface

    void begin_member(const TypePtr& cls, const TypePtr& ret, const std::string& name, int line,
                      int col);

    void end_member();

    void declare_params(std::vector<std::unique_ptr<LocalVar>>& out,
                        std::vector<std::unique_ptr<Param>>& params,
                        const std::vector<TypePtr>& pty);

    static bool same_sig(const MethodSig& a, const MethodSig& b);

    static const MethodSig* find_sig(const TypePtr& t, const MethodSig& sig, int depth = 0);

    static void collect_sigs(const TypePtr& t, const MethodSig& sig,
                             std::vector<const MethodSig*>& out, int depth = 0);

    static const MethodSig* find_method(const TypePtr& t, const std::string& name, size_t argc,
                                        int depth = 0);

    static bool has_method_named(const TypePtr& t, const std::string& name, int depth = 0);

    void check_method(ClassDecl* c, MethodDecl* m);

    void check_ctor(ClassDecl* c, CtorDecl* ctor);

    void check_dtor(ClassDecl* c);

    void check_struct_method(StructDecl* sd, MethodDecl* m);

    void check_struct_ctor(StructDecl* sd, CtorDecl* ctor);

    void check_struct_dtor(StructDecl* sd);

    // 类型级常量的重名检查 + 求值（值在 codegen 里作为字面常量发出）
    void check_const_members(std::vector<std::unique_ptr<ConstMember>>& list, const TypePtr& ty,
                             const std::string& kw, const std::string& type_name);

    void check_struct(StructDecl* sd);

    void check_class(ClassDecl* c);

    void check_interface(InterfaceDecl* itf);

    static bool class_has_concrete(const TypePtr& cls, const MethodSig& req);

    static bool iface_has_default(const TypePtr& t, const MethodSig& req, int depth = 0);

    static void collect_abstract_reqs(const TypePtr& t, std::vector<MethodSig>& out,
                                      int depth = 0);

    static void collect_defaults(const TypePtr& t, std::vector<MethodSig>& out, int depth = 0);

    void check_hierarchy_cycles();

    void compute_abstract_all();

    void check_block(Block* b);

    void check_stmt(Stmt* s);

    void check_local(LocalVar* lv);

    void check_decl_init(bool& zero_flag, const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node);

    // 单个初始化位置：声明初始化、聚合成员、嵌套花括号都走这里
    void check_init_into(const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node);

    void check_aggregate(InitAggregate* init, const TypePtr& ty, const std::string& what);

    static bool simple_seq(Expr* e);

    // 沿 Index/Member 找到最内层的绑定，判断它是否 const
    bool root_is_const(Expr* e);

    void check_writable(Expr* t);

    bool is_weak_field_access(Expr* e);

    void check_assign_stmt(AssignStmt* s);

    void check_return(ReturnStmt* s);

    void check_cond(Expr* e, const std::string& what);

    // switch 分支模式求值：只接受编译期常量（字面量 / 枚举成员）
    long long checked_int_pattern(long long v, const TypePtr& st, Expr* e);

    long long pattern_value(Expr* e, const TypePtr& st);

    std::vector<std::pair<std::string, long long>> enum_members_of(const TypePtr& et);

    void check_switch(SwitchStmt* s);

    // 索引形态（SPEC 6.4.1）：length() -> usize  +  operator[](C, usize) -> T
    void resolve_index_shape(ForInStmt* s, const TypePtr& st);

    void check_for(ForInStmt* s);
    // ------------------------------------------------------------ 赋值兼容
    static bool contains_reference_type(const TypePtr& t);

    static bool contains_slice_type(const TypePtr& t);

    static bool is_zeroable(const TypePtr& t);

    static bool is_char(const TypePtr& t);

    static bool noncopyable_struct(const TypePtr& t);

    TypePtr check_assign(Expr* e, const TypePtr& dst, const std::string& what);

    bool assignable(Expr* e, const TypePtr& src, const TypePtr& dst);

    // ------------------------------------------------------------ 编译期常量求值（U29）
    //
    // 惰性求值 + 记忆化 + visiting 标记：支持前向引用与任意声明顺序，
    // 循环依赖给出明确诊断。求值失败返回 false（调用方回退到 is_const_expr）。
    // 除以零 / 循环依赖属于硬错误，直接抛 SemaError。

    struct ConstVal {
        TypePtr ty;
        bool is_float = false;
        long long i = 0;
        double f = 0;
        bool lit = false;   // 来自整数字面量：允许按声明类型适配（与 check_assign 一致）
        bool flit = false;  // 来自浮点字面量：允许适配到 float / double（与 check_expr 一致）
        bool clit = false;  // 来自字符字面量：允许按位模式适配到 8 位整数（与 check_expr 一致）
    };

    // 求值某个常量期间把 cur_ 切到它所属的 unit（跨 unit 惰性求值时错误位置才正确）
    struct CurUnitGuard {
        Sema& sema;
        Unit* prev;
        CurUnitGuard(Sema& s, Unit* u) : sema(s), prev(s.cur_) {
            if (u) sema.cur_ = u;
        }
        ~CurUnitGuard() { sema.cur_ = prev; }
    };

    // 求值中的「正在求值」标志：异常路径也要复位，否则后续会误报循环依赖
    struct EvalFlagGuard {
        bool& flag;
        explicit EvalFlagGuard(bool& f) : flag(f) { flag = true; }
        ~EvalFlagGuard() { flag = false; }
    };

    // 常量求值递归深度上限（防止超长依赖链把栈打爆）
    static constexpr int kMaxConstDepth = 512;
    struct ConstDepthGuard {
        Sema& sema;
        explicit ConstDepthGuard(Sema& s) : sema(s) { ++sema.const_depth_; }
        ~ConstDepthGuard() { --sema.const_depth_; }
    };

    // 诊断里不要把声明标记 `const` 当成类型名的一部分（`const int` → `int`）
    static std::string type_str_no_const(const TypePtr& t);

    static long long normalize_int(long long v, const TypePtr& t);

    // 按给定位置宽把补码位模式符号扩展到 64 位（与 LLVM AShr 的语义一致）
    static long long sext_bits(long long v, int bits);

    template <class T>
    static bool cmp_const(const std::string& op, T a, T b) {
        if (op == "<") return a < b;
        if (op == "<=") return a <= b;
        if (op == ">") return a > b;
        if (op == ">=") return a >= b;
        if (op == "==") return a == b;
        return a != b;
    }

    ConstVal cast_const(const ConstVal& v, const TypePtr& dst);

    // 把求值结果落到声明类型：同一套赋值规则 + 整数字面量适配（不给 const 加隐式放宽）
    void convert_const_to(ConstVal& v, const TypePtr& dst, const Node* at,
                          const std::string& what);

    bool eval_const_member(ConstMember* cm, ConstVal& out);

    bool eval_global_const(GlobalVar* g, const std::string& unit_name, ConstVal& out);

    bool eval_const_name(Name* e, const TypePtr& self_ty, ConstVal& out);

    // `Type` / `unit.Type` 形式的表达式基类型（仅常量求值用；找不到返回 nullptr）
    TypePtr const_expr_base_type(Expr* base);

    bool eval_const_member_expr(Member* e, ConstVal& out);

    bool eval_const_binary(Binary* e, const TypePtr& self_ty, ConstVal& out);

    bool eval_const(Expr* e, const TypePtr& self_ty, ConstVal& out);

    // ------------------------------------------------------------ 常量表达式
    bool is_const_expr(Init* init);

    bool const_ok(Expr* e);
    // ------------------------------------------------------------ 表达式
    TypePtr check_expr(Expr* e, const TypePtr& expected = nullptr);

    TypePtr check_expr_impl(Expr* e, const TypePtr& expected);

    TypePtr check_name(Name* e);

    TypePtr check_member(Member* e);

    TypePtr check_unary(Unary* e);

    // 条件表达式 `cond ? a : b`（SPEC §5.2）：
    // 条件必须是 bool；两侧类型取「同一类型 → 隐式加宽 → null 适配」，都不成立则要求显式 `as`。
    TypePtr check_ternary(Ternary* e, const TypePtr& expected = nullptr);

    TypePtr check_binary(Binary* e);

    TypePtr check_cond_type(Expr* e, const std::string& what);

    void adapt_lit(Expr* e, const TypePtr& self_ty, const TypePtr& other);

    bool can_cast(const TypePtr& src, const TypePtr& dst);
    // ------------------------------------------------------------ 调用
    TypePtr check_call(Call* c);

    int rank_arg(Expr* e, const TypePtr& aty, const TypePtr& p);

    static bool dominates(const std::vector<int>& a, const std::vector<int>& b);

    static bool is_user_type(const TypePtr& t);

    // 是否含 RC 管理的引用字段（class/interface/切片/string/数组递归）；
    // 只看字段，不看用户析构器（含析构器的 struct 仍允许取址，通过指针使用）。
    static bool has_ref_fields(const TypePtr& t);

    // 运算符查找范围：当前 unit + use 引入的 unit + 操作数类型所属 unit（ADL 风格）。
    // 未导入 unit 的运算符不可见，避免无关 unit 之间意外冲突。
    std::vector<FuncDecl*> collect_operators(const std::string& sym,
                                             const std::vector<TypePtr>& operands = {});

    static std::vector<Expr*> raw_args(const std::vector<ExprPtr>& v);

    FuncDecl* choose_overload(const std::vector<FuncDecl*>& cands,
                              const std::vector<Expr*>& args, const std::string& what,
                              Node* at);

    // 只收集 class 链上的方法（接口默认方法不参与 super 调用）
    void collect_class_methods(const TypePtr& t, const std::string& name,
                               std::vector<const MethodSig*>& out, int depth = 0);

    void collect_methods(const TypePtr& t, const std::string& name,
                         std::vector<const MethodSig*>& out, int depth = 0);

    const MethodSig* choose_method(const std::vector<const MethodSig*>& cands,
                                   const std::vector<Expr*>& args,
                                   const std::string& what, Node* at);

    const std::vector<TypePtr>* choose_ctor(const std::vector<std::vector<TypePtr>>& ctors,
                                            const std::vector<Expr*>& args,
                                            const std::string& what, Node* at);

    void check_method_args(Call* c, const MethodSig* sig, const std::string& what);

    TypePtr check_super_call(Call* c);

    [[noreturn]] void err_slice_elem_align(const TypePtr& elem, const Node* node) const;

    TypePtr check_new(NewExpr* ne);

    void check_ctor_args(Call* c, const TypePtr& ty);

    TypePtr check_builtin(Call* c, const std::string& name);

    // ------------------------------------------------------------ 杂项
    bool is_lvalue(Expr* e);

    // 该表达式是否“根”在 class/interface 引用上（含其数组/切片元素）：
    // `c.x`、`h.items[0]`、`catArray[i]` 都算。U21：这类地址无法表达 RC 生命周期。
    bool rooted_at_reference(Expr* e);

    void check_entry();

};

}  // namespace muc_detail

