// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Mulan AST
//
// Design: one-to-one with the Python version; semantic analysis writes the results back into the node fields.
#pragma once

#include <cstdint>
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

// ---------------------------------------------------------------- Type syntax

struct TypeSpec {
    enum class K { Named, Ptr, Array, Slice, Func } kind = K::Named;
    std::string name;          // Named: "int" / "Point" / "math.Point" / "Vec"
    // Non-empty when Named is a generic instantiation: the type arguments of `Vec<int>` (`Vec` itself is a template).
    std::vector<std::unique_ptr<TypeSpec>> args;
    std::unique_ptr<TypeSpec> elem;
    // Func: `(T1, T2) => R` — parameter types and the return type
    std::vector<std::unique_ptr<TypeSpec>> params;
    std::unique_ptr<TypeSpec> ret;
    size_t size = 0;           // Array
    int rank = 1;              // Slice: rank
    bool is_const = false;  // C model: whether this type node is const (a prefix const qualifies the base type, `* const` qualifies the pointer level)
    bool is_self = false;   // `Self` inside an interface body (the implementor type); only allowed in interface member signatures
    bool weak = false;         // the weak modifier on a class field
    int line = 0;
    int col = 0;
    std::string str() const;
};
using TypeSpecPtr = std::unique_ptr<TypeSpec>;

// ---------------------------------------------------------------- Forward declarations

struct LocalVar;
struct GlobalVar;
struct FuncDecl;
struct Param;
struct Block;
struct Stmt;
struct EnumConst;struct CtorDecl;
struct DtorDecl;
struct MethodDecl;
struct ConstMember;

// The name resolution result (written by sema, read by codegen)
using Sym = std::variant<std::monostate, LocalVar*, GlobalVar*, FuncDecl*, EnumConst*,
                         ConstMember*, Type*>;

// ---------------------------------------------------------------- Expressions

// An implicit struct construction inserted at a coercion point (3.6): `string s = cstr;` uses
// `implicit string(const char*)`. Allocated from a Sema-owned pool, so a node may hold it long-term.
struct ImplicitConv {
    TypePtr to;
    std::vector<TypePtr> pty;
    std::string cname;
};

struct Expr : Node {
    TypePtr ty;
    const ImplicitConv* conv = nullptr;  // sema: this expression is converted by an implicit constructor
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
    std::string value;  // the raw UTF-8 bytes
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
    bool implicit_this = false;  // direct field access inside a method
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
    // sema: the selected function is recorded when subscripting a user type goes through an operator overload
    FuncDecl* get_func = nullptr;
    const MethodSig* get_method = nullptr;  // sema: member `operator[]` on a struct (borrowed receiver)
    bool by_ref = false;      // `operator[]` returns a borrow: the expression type is the referent and the value is the element address
    bool ref_const = false;   // whether the borrow is read-only (`ref const T`)
};

struct Unary : Expr {
    std::string op;  // - + & * !
    ExprPtr operand;
    FuncDecl* op_func = nullptr;              // sema: user operator overload (free function)
    const MethodSig* op_method = nullptr;     // sema: member operator on a struct (borrowed receiver)
};

struct NotExpr : Expr {  // the not keyword
    ExprPtr operand;
};

struct Binary : Expr {
    std::string op;  // + - * / % << >> & | ^ < <= > >= == != and or
    ExprPtr left;
    ExprPtr right;
    FuncDecl* op_func = nullptr;              // sema: user operator overload (free function)
    const MethodSig* op_method = nullptr;     // sema: member operator on a struct (borrowed receiver)
    bool negate_result = false;               // sema: `a != b` implemented through `operator==` (the result is negated)
};

struct Cast : Expr {
    ExprPtr expr;
    TypeSpecPtr spec;
};

// Conditional expression: cond ? then_e : else_e (right-associative, only the selected branch is evaluated)
struct Ternary : Expr {
    ExprPtr cond;
    ExprPtr then_e;
    ExprPtr else_e;
};

struct Call : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    // the explicit type arguments of a generic call: `f<int>(x)` (empty = a normal call)
    std::vector<TypeSpecPtr> type_args;
    // sema
    std::string callee_kind;  // "func" | "ctor" | "builtin" | "method" | "super" | "fnptr"
    FuncDecl* func = nullptr;
    TypePtr fn_ty;            // callee_kind == "fnptr": the function type being called
    TypePtr ctor;             // the target type of the struct construction
    std::string builtin;      // alloc / realloc / free
    std::vector<int> field_order;
    const MethodSig* method = nullptr;      // the selected method / interface default method
    const std::vector<TypePtr>* ctor_pty = nullptr;  // the selected constructor (used by super)
    bool by_ref = false;      // the called function/method returns a borrow (the expression type is the referent and the value is the address)
    bool ref_const = false;   // whether the borrow is read-only (`ref const T`)
};

// Lambda expression: `(int a) => expr` / `(int a) => { ... }` (no captures).
// sema lowers it to a hidden `private` function (`synth`) and the expression's value is that function's address.
struct Lambda : Expr {
    std::vector<std::unique_ptr<Param>> params;
    ExprPtr expr_body;            // `=> expr`
    std::unique_ptr<Block> block_body;  // `=> { ... }`
    // sema
    FuncDecl* synth = nullptr;
};

struct NewExpr : Expr {
    TypeSpecPtr type;   // the target class type / dynamic array element type
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    bool is_array = false;  // new T[..]
    std::vector<ExprPtr> dims;  // the size of each dimension of the rectangle (runtime expressions)
    int jagged_rank = 0;        // new T[m][]: how many more T[] levels the outer part wraps
    // sema
    TypePtr class_ty;
    TypePtr elem_ty;                // the dynamic array element type (innermost)
    CtorDecl* ctor = nullptr;
    bool has_ctor = false;          // the class declares a constructor (ne->ctor_pty is selected)
    std::vector<TypePtr> ctor_pty;  // the parameter list of the selected constructor (empty = the default zero-initialized construction)
};

struct SizeofExpr : Expr {
    TypeSpecPtr spec;   // `sizeof(T)` / `alignof(T)` / `offsetof(T, field)`
    ExprPtr expr;       // `sizeof expr`: the type is taken from the operand and the operand is never evaluated
    std::string field;  // `offsetof` only: the member name
    std::string which;  // "sizeof" | "alignof" | "offsetof"
    // sema: the resolved type and the folded byte value (codegen emits `cval`; it never re-resolves `spec`)
    TypePtr spec_ty;
    uint64_t cval = 0;
};

struct Len : Expr {
    ExprPtr seq;
    ExprPtr dim;  // len(a, k): optional dimension
};

struct RangeExpr : Expr {
    ExprPtr lo;
    ExprPtr hi;
    bool inclusive = false;  // `..=`: the upper bound is inclusive
};

// ---------------------------------------------------------------- Initializers

struct Init : Node {
    enum class K { Expr, Aggregate } kind = K::Expr;
    virtual ~Init() = default;
};
using InitPtr = std::unique_ptr<Init>;

struct InitExpr : Init {
    ExprPtr expr;
};

struct InitAggregate : Init {
    // an empty name means positional initialization; an item itself can be a nested brace initialization
    std::vector<std::pair<std::string, InitPtr>> items;
};

// range fill inside braces: `{1, 2, 5..9, 10}` (fixed-size arrays only, the endpoints must be compile-time constants)
struct InitRange : Init {
    ExprPtr lo;
    ExprPtr hi;
    // sema
    long long lo_val = 0;
    long long count = 0;  // the number of elements after expansion (0 when a >= b)
};

// ---------------------------------------------------------------- Declarations

// Generic type parameters: `<T, U: I>` / `<T: int | long>`.
// Constraints initially support a flat list plus a single operator (all `&` or all `|`); mixing them is an error (see parse_type_params).
struct TypeParam {
    enum class Op { None, And, Or } op = Op::None;
    std::string name;
    std::vector<TypeSpecPtr> bounds;  // empty = no constraint
    int line = 0;
    int col = 0;
};

struct Decl : Node {
    // Visibility: visible only inside the owning unit by default; public means visible across units.
    // The visibility of a unit is Unit::is_public (`public unit foo;`).
    bool is_public = false;
    std::vector<TypeParam> type_params;  // empty = not generic
    bool is_template = false;            // sema: a generic template, not checked/emitted directly, only used for instantiation
    virtual ~Decl() = default;
};

struct UseDecl : Decl {
    std::string unit_name;
};

// Type alias: `type A = B;` — a transparent alias (produces no new type), and each declaration can define only one.
// `type` is a **contextual keyword**: it is parsed as an alias only at the start of a declaration, in the form `type Ident =`.
struct TypeAliasDecl : Decl {
    std::string name;
    TypeSpecPtr spec;
    // sema
    TypePtr rty;             // the resolution result (memo; equivalent to the underlying type)
    bool resolved = false;
    bool resolving = false;  // circular dependency detection
};

struct Param : Node {
    std::string name;
    TypeSpecPtr spec;
    // `const` is a read-only marker on the parameter binding (not a type qualifier): once inside the function body it cannot be rebound and fields cannot be written through it.
    bool is_const = false;
    // A `ref` parameter borrows the caller's variable (a pointer is passed on the C side) and cannot be stored/returned.
    bool by_ref = false;
};

struct GlobalVar : Decl {    std::string name;
    TypeSpecPtr spec;
    InitPtr init;
    bool is_extern = false;
    bool is_const = false;
    // sema
    TypePtr rty;
    bool zero = false;
    // 9.1: the initializer is not a compile-time constant (or the type needs a parameterless
    // constructor); the variable is initialized by the unit init sequence instead of the data area.
    bool runtime_init = false;
    std::string cname;
    // the compile-time evaluation result (unit-level const only; it stays false when the evaluation fails and falls back to the constant generation in codegen)
    bool const_evaluated = false;
    bool const_evaluating = false;
    bool const_is_float = false;
    long long const_ival = 0;
    double const_fval = 0;
    // 3.16: the initializer is a function value (an address constant); codegen emits the function's address
    FuncDecl* const_func = nullptr;
};

// Type-level compile-time constant: `const int MaxAge = 20;` (only allowed inside a struct / class body)
// It occupies no instance storage: it does not go into fields, does not affect sizeof / alignof / layout / ABI, and takes no part in construction/destruction/RC.
struct ConstMember : Node {
    std::string name;
    TypeSpecPtr spec;
    ExprPtr init;
    bool is_public = true;
    // sema
    TypePtr rty;   // the declared type (scalar or enum)
    TypePtr owner;  // the struct / class type that declares it
    bool evaluated = false;
    bool evaluating = false;  // under lazy evaluation (circular dependency detection)
    bool is_float = false;
    long long ival = 0;
    double fval = 0;
};

struct StructDecl : Decl {
    std::string name;
    std::vector<std::string> bases;  // only interfaces can be listed (`struct S : I1, I2`), see backlog U33
    std::vector<TypeSpecPtr> base_specs;  // parallel to bases; a generic base class needs structured type arguments
    std::vector<std::pair<std::string, TypeSpecPtr>> fields;
    // the final visibility of the fields (computed by the parser while parsing, from the default values/section labels/member prefixes)
    std::unordered_map<std::string, bool> field_visibility;
    std::vector<std::unique_ptr<ConstMember>> const_members;  // type-level compile-time constants
    std::vector<std::unique_ptr<MethodDecl>> methods;  // value type methods (static dispatch)
    std::vector<std::unique_ptr<CtorDecl>> ctors;
    std::unique_ptr<DtorDecl> dtor;
    // Layout modifiers
    bool is_packed = false;
    bool is_union = false;  // a union declaration (layout: all members at offset 0)
    ExprPtr align_expr;  // the N of align(N) (the parser keeps the original expression and sema evaluates it)
    TypePtr ty;  // sema: the struct type (the fields are filled in after the placeholder is created)
};

struct EnumDecl : Decl {
    std::string name;
    std::vector<std::pair<std::string, std::optional<long long>>> members;
    TypePtr ty;  // sema: the enum type
};

// ---------------------------------------------------------------- Statements

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
    bool by_ref = false;  // a for-in ref binding / ref parameter: it is T* on the C side and is dereferenced at the use site
    bool is_ref_param = false;  // a `ref` parameter: it cannot be rebound (a for-in ref variable can write elements)
    int group = 0;        // shared by multiple declarators in the same declaration statement (used for the auto consistency check)
};

struct ExprStmt : Stmt {
    ExprPtr expr;
};

struct AssignStmt : Stmt {
    ExprPtr target;
    std::string op;  // = += -= *= /= %= &= |= ^= <<= >>=
    ExprPtr value;
    FuncDecl* set_func = nullptr;              // sema: operator[]= overload (free function)
    const MethodSig* set_method = nullptr;     // sema: member operator[]= on a struct
};

struct IfStmt : Stmt {
    ExprPtr cond;
    std::unique_ptr<Block> then;
    StmtPtr els;  // Block or IfStmt
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
    ExprPtr step;              // `for i in a..b step n` (ranges only; n must be a nonzero compile-time constant)
    long long step_val = 1;    // sema: the step (signed)
    std::unique_ptr<Block> body;
    // sema
    std::string loop_kind;  // "range" | "array" | "slice" | "string" | "index"
    TypePtr elem_ty;
    TypePtr seq_ty;
    TypePtr var_ty;
    TypePtr idx_ty;                      // the i of `for i, x`: usize
    std::unique_ptr<LocalVar> idx_lvar;  // the index variable symbol (codegen uses its cname)
    const MethodSig* len_sig = nullptr;  // the index form: length() -> usize
    FuncDecl* idx_fn = nullptr;          // the index form: operator[](C, usize) -> T
    const MethodSig* idx_method = nullptr;  // the index form: member operator[](usize) -> T
    std::string cname;
    std::unique_ptr<LocalVar> lvar;  // the loop variable symbol (codegen uses its cname/by_ref)
    // cursor form (6.4.2): contract declared in core.iter (IIterator<T>)
    TypePtr it_ty;                     // the cursor struct returned by iterator()
    const MethodSig* iter_sig = nullptr;
    const MethodSig* next_sig = nullptr;
    const MethodSig* cur_sig = nullptr;
};

// `para { { ... } { ... } }`: structured fork/join (SPEC 3.17.2)
struct ParaStmt : Stmt {
    std::vector<std::unique_ptr<Block>> sections;
    // sema: per section, the enclosing locals it reads (first-use order); codegen builds one frame per section
    std::vector<std::vector<LocalVar*>> captures;
};

// `para for i in a..b { ... }`: concurrent iteration (SPEC 3.17.3). v1: ranges only.
struct ParaForStmt : Stmt {
    std::unique_ptr<ForInStmt> loop;
    // sema: enclosing locals read by the body (first-use order)
    std::vector<LocalVar*> captures;
};

// switch branch pattern: a single value or a half-open range [vlo, vhi)
struct SwitchPat {
    ExprPtr lo;
    ExprPtr hi;  // the upper bound of the range (non-null when is_range)
    bool is_range = false;
    bool inclusive = false;  // `..=`: the upper bound is included (sema normalizes it into the vhi of a half-open range)
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
    bool enum_exhaustive = false;  // the enum covers all members (it is exhaustive even without an else)
};

struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};

struct ReturnStmt : Stmt {
    ExprPtr value;
};

struct ThrowStmt : Stmt {
    ExprPtr value;
};

struct CatchClause : Node {
    std::string name;
    TypeSpecPtr spec;
    std::unique_ptr<Block> body;
    // sema
    TypePtr rty;                     // the caught class type
    std::unique_ptr<LocalVar> var;   // catch binding (borrow; read-only)
};

struct TryStmt : Stmt {
    std::unique_ptr<Block> body;
    std::vector<std::unique_ptr<CatchClause>> catches;
    std::unique_ptr<Block> finally_body;
};

// ---------------------------------------------------------------- Object model declarations

struct MethodDecl : Node {
    std::string name;
    TypeSpecPtr ret;
    bool ret_ref = false;  // `ref T m()`: returns a borrow
    // an operator signature in an interface: `T operator+(T a, T b);` (no this, the parameter list holds all operands)
    bool is_operator = false;
    std::vector<std::unique_ptr<Param>> params;
    std::unique_ptr<Block> body;  // nullptr = an abstract method
    bool is_override = false;
    bool is_public = false;  // visible only inside the unit by default
    // sema
    TypePtr rty;
    std::vector<TypePtr> pty;
    std::vector<std::unique_ptr<LocalVar>> param_locals;
    std::string cname;  // the method symbol name
};

struct CtorDecl : Node {
    bool is_implicit = false;  // `implicit T(...)`: usable as an implicit conversion (struct only)
    std::vector<std::unique_ptr<Param>> params;
    std::unique_ptr<Block> body;
    bool is_public = false;  // visible only inside the unit by default
    // sema
    std::vector<TypePtr> pty;
    std::vector<std::unique_ptr<LocalVar>> param_locals;
    std::string cname;  // the constructor symbol name
    bool has_explicit_super = false;
};

struct DtorDecl : Node {
    std::unique_ptr<Block> body;
    std::string cname;  // the destructor symbol name
};

struct ClassDecl : Decl {
    std::string name;
    std::vector<std::string> bases;  // the first is the base class, the rest are interfaces
    std::vector<TypeSpecPtr> base_specs;  // parallel to bases; a generic base class needs structured type arguments
    std::vector<std::pair<std::string, TypeSpecPtr>> fields;
    std::unordered_map<std::string, bool> field_visibility;  // the final visibility of the fields
    std::vector<std::unique_ptr<ConstMember>> const_members;  // type-level compile-time constants
    std::vector<std::unique_ptr<MethodDecl>> methods;
    std::vector<std::unique_ptr<CtorDecl>> ctors;
    std::unique_ptr<DtorDecl> dtor;
    TypePtr ty;  // sema reserved
};

struct InterfaceDecl : Decl {
    std::string name;
    std::vector<std::string> bases;
    std::vector<TypeSpecPtr> base_specs;  // parallel to bases
    std::vector<std::unique_ptr<MethodDecl>> methods;
    TypePtr ty;
};

struct FuncDecl : Decl {
    std::string name;
    TypeSpecPtr ret;
    bool ret_ref = false;  // `ref T f()`: returns a borrow
    std::vector<std::unique_ptr<Param>> params;
    std::unique_ptr<Block> body;
    bool is_extern = false;
    bool is_varargs = false;  // the `...` of an extern function (C varargs)
    bool is_operator = false;
    // sema
    TypePtr rty;
    std::vector<TypePtr> pty;
    std::vector<bool> pty_ref;   // whether each parameter is a `ref` borrow (one pointer on the C side)
    std::vector<bool> pty_cref;  // `ref const T`: a read-only borrow
    std::vector<std::unique_ptr<LocalVar>> param_locals;
    std::string cname;
    bool is_entry = false;    // the entry unit's `main`: renamed to `mu_main` and called by the generated wrapper
    FuncDecl* sym = nullptr;  // self
};

struct Unit : Node {
    std::string name;
    std::string filename;
    bool is_public = false;  // `public unit foo;`: visible across libraries
    std::vector<std::unique_ptr<UseDecl>> uses;
    std::vector<std::unique_ptr<Decl>> decls;
    // Generic templates (`type_params` non-empty) are moved here during the sema collection phase: they do not
    // take part in the regular checks of the first phase and are only looked up by the instantiator via `(unit, name, args)`.
    std::vector<std::unique_ptr<Decl>> templates;
    // Functions synthesized for lambda expressions (checked inline with the expression; emitted by codegen).
    std::vector<std::unique_ptr<FuncDecl>> lambdas;
};
