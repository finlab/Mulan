// mu-lan AST
//
// 设计：与 Python 版一一对应，语义分析把结果直接写回节点字段。
#pragma once

#include <memory>
#include <optional>
#include <string>
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
    bool readonly = false;
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
    std::string kind;  // "local" | "global" | "func" | "enum_const"
    Sym sym;
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
    ExprPtr index;
};

struct Unary : Expr {
    std::string op;  // - + & * !
    ExprPtr operand;
};

struct NotExpr : Expr {  // 关键字 not
    ExprPtr operand;
};

struct Binary : Expr {
    std::string op;  // + - * / % << >> & | ^ < <= > >= == != and or
    ExprPtr left;
    ExprPtr right;
};

struct Cast : Expr {
    ExprPtr expr;
    TypeSpecPtr spec;
};

struct Call : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    // sema
    std::string callee_kind;  // "func" | "ctor" | "builtin"
    FuncDecl* func = nullptr;
    TypePtr ctor;             // 结构体构造目标类型
    std::string builtin;      // alloc / realloc / free
    std::vector<int> field_order;
};

struct NewExpr : Expr {
    TypeSpecPtr type;   // 目标 class 类型
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    // sema
    TypePtr class_ty;
    CtorDecl* ctor = nullptr;
};

struct SizeofExpr : Expr {
    TypeSpecPtr spec;
    std::string which;  // "sizeof" | "alignof"
};

struct Len : Expr {
    ExprPtr seq;
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
    bool is_static = false;
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
    ExprPtr seq;
    std::unique_ptr<Block> body;
    // sema
    std::string loop_kind;  // "range" | "array" | "slice" | "string"
    TypePtr elem_ty;
    TypePtr seq_ty;
    TypePtr var_ty;
    std::string cname;
    std::unique_ptr<LocalVar> lvar;  // 循环变量的符号（codegen 用其 cname/by_ref）
};

struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};

struct ReturnStmt : Stmt {
    ExprPtr value;
};

struct DelStmt : Stmt {
    ExprPtr value;
};

// ---------------------------------------------------------------- 对象模型声明

struct MethodDecl : Node {
    std::string name;
    TypeSpecPtr ret;
    std::vector<std::unique_ptr<Param>> params;
    std::unique_ptr<Block> body;  // nullptr = 抽象方法
    bool is_override = false;
};

struct CtorDecl : Node {
    std::vector<std::unique_ptr<Param>> params;
    std::unique_ptr<Block> body;
};

struct DtorDecl : Node {
    std::unique_ptr<Block> body;
};

struct ClassDecl : Decl {
    std::string name;
    std::vector<std::string> bases;  // 第一个是基类，其余是接口
    std::vector<std::pair<std::string, TypeSpecPtr>> fields;
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
    std::vector<std::unique_ptr<UseDecl>> uses;
    std::vector<std::unique_ptr<Decl>> decls;
};
