// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Mulan AST
//
// 设计：与 Python 版一一对应，语义分析把结果直接写回节点字段。
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "types.hpp"

struct Node {
    int line = 0;
    int col = 0;
    virtual ~Node() = default;
};

// ---------------------------------------------------------------- 类型语法

struct TypeSpec {
    enum class K { Named, Ptr, Array, Slice } kind = K::Named;
    std::string name;          // Named: "int" / "Point" / "math.Point"
    std::unique_ptr<TypeSpec> elem;
    size_t size = 0;           // Array
    int rank = 1;              // Slice：维数
    bool is_const = false;  // C 模型：该类型节点是否 const（前缀 const 修饰基类型，`* const` 修饰指针层）
    bool weak = false;         // class 字段的 weak 修饰
    int line = 0;
    int col = 0;
    std::string str() const;
};
using TypeSpecPtr = std::unique_ptr<TypeSpec>;

// ---------------------------------------------------------------- 前向声明

struct LocalVar;
struct GlobalVar;
struct FuncDecl;
struct EnumConst;
struct CtorDecl;
struct DtorDecl;
struct MethodDecl;

// 名字解析结果（sema 写入，codegen 读取）
using Sym = std::variant<std::monostate, LocalVar*, GlobalVar*, FuncDecl*, EnumConst*, Type*>;

// ---------------------------------------------------------------- 表达式

struct Expr : Node {
    TypePtr ty;
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLit : Expr {
    long long value = 0;
};
struct FloatLit : Expr {
    double value = 0;
    bool is_float32 = false;
};
struct CharLit : Expr {
    int value = 0;
};
struct StringLit : Expr {
    std::string value;  // 原始 UTF-8 字节
};
struct BoolLit : Expr {
    bool value = false;
};
struct NullLit : Expr {};

struct Name : Expr {
    std::string ident;
    std::string kind;  // "local" | "global" | "func" | "enum_const" | "field"
    Sym sym;
    int field_index = -1;
    bool implicit_this = false;  // 方法内直接访问字段
};

struct Member : Expr {
    ExprPtr obj;
    std::string name;
    std::string kind;  // "field" | "global" | "func" | "unit"
    Sym sym;
    int field_index = -1;
};

struct Index : Expr {
    ExprPtr base;
    std::vector<ExprPtr> indices;  // a[i] / a[i,j] / a[i,j,k]
    // sema: 用户类型下标走运算符重载时记录选中的函数
    FuncDecl* get_func = nullptr;
};

struct Unary : Expr {
    std::string op;  // - + & * !
    ExprPtr operand;
    FuncDecl* op_func = nullptr;  // sema: 用户运算符重载
};

struct NotExpr : Expr {  // 关键字 not
    ExprPtr operand;
};

struct Binary : Expr {
    std::string op;  // + - * / % << >> & | ^ < <= > >= == != and or
    ExprPtr left;
    ExprPtr right;
    FuncDecl* op_func = nullptr;  // sema: 用户运算符重载
};

struct Cast : Expr {
    ExprPtr expr;
    TypeSpecPtr spec;
};

// 条件表达式：cond ? then_e : else_e（右结合，只求值被选中的分支）
struct Ternary : Expr {
    ExprPtr cond;
    ExprPtr then_e;
    ExprPtr else_e;
};

struct Call : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    // sema
    std::string callee_kind;  // "func" | "ctor" | "builtin" | "method" | "super"
    FuncDecl* func = nullptr;
    TypePtr ctor;             // 结构体构造目标类型
    std::string builtin;      // alloc / realloc / free
    std::vector<int> field_order;
    const MethodSig* method = nullptr;      // 选中的方法/接口默认方法
    const std::vector<TypePtr>* ctor_pty = nullptr;  // 选中的构造器（super 用）
};

struct NewExpr : Expr {
    TypeSpecPtr type;   // 目标 class 类型 / 动态数组元素类型
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    bool is_array = false;  // new T[..]
    std::vector<ExprPtr> dims;  // 矩形各维尺寸（运行期表达式）
    int jagged_rank = 0;        // new T[m][]：外层再包几层 T[]
    // sema
    TypePtr class_ty;
    TypePtr elem_ty;                // 动态数组元素类型（最内层）
    CtorDecl* ctor = nullptr;
    bool has_ctor = false;          // class 声明了构造器（选中 ne->ctor_pty）
    std::vector<TypePtr> ctor_pty;  // 选中的构造器参数表（空 = 默认零初始化构造）
};

struct SizeofExpr : Expr {
    TypeSpecPtr spec;
    std::string which;  // "sizeof" | "alignof"
};

struct Len : Expr {
    ExprPtr seq;
    ExprPtr dim;  // len(a, k)：可选维度
};

struct RangeExpr : Expr {
    ExprPtr lo;
    ExprPtr hi;
};

// ---------------------------------------------------------------- 初始化器

struct Init : Node {
    enum class K { Expr, Aggregate } kind = K::Expr;
    virtual ~Init() = default;
};
using InitPtr = std::unique_ptr<Init>;

struct InitExpr : Init {
    ExprPtr expr;
};

struct InitAggregate : Init {
    // name 为空表示位置初始化；子项本身可以是嵌套的花括号初始化
    std::vector<std::pair<std::string, InitPtr>> items;
};

// ---------------------------------------------------------------- 声明

struct Decl : Node {
    // 可见性：默认仅在所属 unit 内可见；public 表示跨 unit 可见。
    // unit 的可见性见 Unit::is_public（`public unit foo;`）。
    bool is_public = false;
    virtual ~Decl() = default;
};

struct UseDecl : Decl {
    std::string unit_name;
};

struct Param : Node {
    std::string name;
    TypeSpecPtr spec;
};

struct GlobalVar : Decl {
    std::string name;
    TypeSpecPtr spec;
    InitPtr init;
    bool is_extern = false;
    bool is_const = false;
    // sema
    TypePtr rty;
    bool zero = false;
    std::string cname;
};

struct StructDecl : Decl {
    std::string name;
    std::vector<std::pair<std::string, TypeSpecPtr>> fields;
    // 字段的最终可见性（parser 在解析时按默认值/分段标签/成员前缀算好）
    std::unordered_map<std::string, bool> field_visibility;
    std::vector<std::unique_ptr<MethodDecl>> methods;  // 值类型方法（静态派发）
    std::vector<std::unique_ptr<CtorDecl>> ctors;
    std::unique_ptr<DtorDecl> dtor;
    // 布局修饰符
    bool is_packed = false;
    bool is_union = false;  // union 声明（布局：所有成员偏移 0）
    ExprPtr align_expr;  // align(N) 的 N（parser 保留原始表达式，sema 求值）
    TypePtr ty;  // sema: 结构体类型（占位后填充 fields）
};

struct EnumDecl : Decl {
    std::string name;
    std::vector<std::pair<std::string, std::optional<long long>>> members;
    TypePtr ty;  // sema: 枚举类型
};

// ---------------------------------------------------------------- 语句

struct Stmt : Node {
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct Block : Stmt {
    std::vector<StmtPtr> stmts;
};

struct LocalVar : Stmt {
    std::string name;
    TypeSpecPtr spec;
    InitPtr init;
    bool is_const = false;
    bool is_auto = false;
    // sema
    TypePtr rty;
    bool zero = false;
    std::string cname;
    bool by_ref = false;  // for-in ref 绑定：C 侧是 T*，使用点解引用
    int group = 0;        // 同一声明语句的多个声明符共享（用于 auto 一致性检查）
};

struct ExprStmt : Stmt {
    ExprPtr expr;
};

struct AssignStmt : Stmt {
    ExprPtr target;
    std::string op;  // = += -= *= /= %= &= |= ^= <<= >>=
    ExprPtr value;
    FuncDecl* set_func = nullptr;  // sema: operator[]= 重载
};

struct IfStmt : Stmt {
    ExprPtr cond;
    std::unique_ptr<Block> then;
    StmtPtr els;  // Block 或 IfStmt
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    std::unique_ptr<Block> body;
};

struct ForInStmt : Stmt {
    bool by_ref = false;
    std::string var;
    bool has_idx = false;      // `for i, x in seq`
    std::string idx_name;
    ExprPtr seq;
    std::unique_ptr<Block> body;
    // sema
    std::string loop_kind;  // "range" | "array" | "slice" | "string" | "index"
    TypePtr elem_ty;
    TypePtr seq_ty;
    TypePtr var_ty;
    TypePtr idx_ty;                      // `for i, x` 的 i：usize
    std::unique_ptr<LocalVar> idx_lvar;  // 索引变量符号（codegen 用其 cname）
    const MethodSig* len_sig = nullptr;  // 索引形态：length() -> usize
    FuncDecl* idx_fn = nullptr;          // 索引形态：operator[](C, usize) -> T
    std::string cname;
    std::unique_ptr<LocalVar> lvar;  // 循环变量的符号（codegen 用其 cname/by_ref）
};

// switch 分支模式：单值或半开区间 [vlo, vhi)
struct SwitchPat {
    ExprPtr lo;
    ExprPtr hi;  // 区间上界（is_range 时非空）
    bool is_range = false;
    // sema
    long long vlo = 0;
    long long vhi = 0;
};

struct SwitchArm {
    std::vector<SwitchPat> pats;
    bool is_else = false;
    std::unique_ptr<Block> body;
    int line = 0;
    int col = 0;
};

struct SwitchStmt : Stmt {
    ExprPtr scrut;
    std::vector<std::unique_ptr<SwitchArm>> arms;
    // sema
    TypePtr sty;
    bool has_else = false;
    bool enum_exhaustive = false;  // 枚举已覆盖全部成员（没有 else 也算穷尽）
};

struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};

struct ReturnStmt : Stmt {
    ExprPtr value;
};

// ---------------------------------------------------------------- 对象模型声明

struct MethodDecl : Node {
    std::string name;
    TypeSpecPtr ret;
    std::vector<std::unique_ptr<Param>> params;
    std::unique_ptr<Block> body;  // nullptr = 抽象方法
    bool is_override = false;
    bool is_public = false;  // 默认仅 unit 内可见
    // sema
    TypePtr rty;
    std::vector<TypePtr> pty;
    std::vector<std::unique_ptr<LocalVar>> param_locals;
    std::string cname;  // 方法符号名
};

struct CtorDecl : Node {
    std::vector<std::unique_ptr<Param>> params;
    std::unique_ptr<Block> body;
    bool is_public = false;  // 默认仅 unit 内可见
    // sema
    std::vector<TypePtr> pty;
    std::vector<std::unique_ptr<LocalVar>> param_locals;
    std::string cname;  // 构造器符号名
    bool has_explicit_super = false;
};

struct DtorDecl : Node {
    std::unique_ptr<Block> body;
    std::string cname;  // 析构器符号名
};

struct ClassDecl : Decl {
    std::string name;
    std::vector<std::string> bases;  // 第一个是基类，其余是接口
    std::vector<std::pair<std::string, TypeSpecPtr>> fields;
    std::unordered_map<std::string, bool> field_visibility;  // 字段的最终可见性
    std::vector<std::unique_ptr<MethodDecl>> methods;
    std::vector<std::unique_ptr<CtorDecl>> ctors;
    std::unique_ptr<DtorDecl> dtor;
    TypePtr ty;  // sema 预留
};

struct InterfaceDecl : Decl {
    std::string name;
    std::vector<std::string> bases;
    std::vector<std::unique_ptr<MethodDecl>> methods;
    TypePtr ty;
};

struct FuncDecl : Decl {
    std::string name;
    TypeSpecPtr ret;
    std::vector<std::unique_ptr<Param>> params;
    std::unique_ptr<Block> body;
    bool is_extern = false;
    bool is_varargs = false;  // extern 函数的 `...`（C varargs）
    bool is_operator = false;
    // sema
    TypePtr rty;
    std::vector<TypePtr> pty;
    std::vector<std::unique_ptr<LocalVar>> param_locals;
    std::string cname;
    FuncDecl* sym = nullptr;  // self
};

struct Unit : Node {
    std::string name;
    std::string filename;
    bool is_public = false;  // `public unit foo;`：跨库可见
    std::vector<std::unique_ptr<UseDecl>> uses;
    std::vector<std::unique_ptr<Decl>> decls;
};
