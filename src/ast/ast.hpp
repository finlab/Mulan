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

struct Expr;  // TypeSpec: an array length may be an expression (`int[N]`)

struct TypeSpec {
    enum class K { Named, Ptr, Array, Slice, Func, Delegate } kind = K::Named;
    std::string name;          // Named: "int" / "Point" / "math.Point" / "Vec"
    // Non-empty when Named is a generic instantiation: the type arguments of `Vec<int>` (`Vec` itself is a template).
    std::vector<std::unique_ptr<TypeSpec>> args;
    std::unique_ptr<TypeSpec> elem;
    // Func: `(T1, T2) => R` — parameter types and the return type
    std::vector<std::unique_ptr<TypeSpec>> params;
    std::unique_ptr<TypeSpec> ret;
    size_t size = 0;           // Array: the element count when the length is an integer literal
    // Array: the length expression when it is not a literal (`int[N]`, `int[SIZE * 2]`); the semantic pass
    // folds it against the unit's compile-time constants (SPEC 3.3.2).
    std::unique_ptr<Expr> len;
    int rank = 1;              // Slice: rank
    bool is_const = false;  // C model: whether this type node is const (a prefix const qualifies the base type, `* const` qualifies the pointer level)
    bool is_self = false;   // `Self` inside an interface body (the implementor type); only allowed in interface member signatures
    bool weak = false;         // the weak modifier on a class field
    // Set by generic substitution when this position was a type parameter: the argument type, already
    // resolved in the **use-site** unit. The instantiated declaration is re-resolved in the template's
    // unit, which cannot see use-site types, so `resolve_type` returns this directly.
    TypePtr resolved;
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
    // C-style suffix: 0 none, 1 `u` (uint), 2 `l` (long), 3 `ul` (ulong). A suffixed literal has a
    // fixed type and does not adapt to the expected type.
    int isuffix = 0;
    // The magnitude is exactly 2^63 and was written without a sign. Only a directly applied unary `-`
    // turns it into the minimum of a signed 64-bit type; anywhere else it is out of range.
    bool min_magnitude = false;
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
    std::string kind;  // "local" | "global" | "func" | "enum_const" | "field" | "sum_unit"
    Sym sym;
    int field_index = -1;
    bool implicit_this = false;  // direct field access inside a method
    // contextual sum construction: `Option<int> o = None;` (the alternative resolved from the target type)
    TypePtr sum_ty;
    int sum_alt = -1;
};

struct Member : Expr {
    ExprPtr obj;
    std::string name;
    std::string kind;  // "field" | "global" | "func" | "unit" | "sum_unit"
    Sym sym;
    int field_index = -1;
    // `Sum.None`: a unit alternative used as a value (also produced by contextual construction)
    TypePtr sum_ty;
    int sum_alt = -1;
    // a generic sum's alternative: `Option<int>.None` (the type arguments of the base type)
    std::vector<TypeSpecPtr> type_args;
    // sema: how many `promote` rewrites produced this member access (promotion is one level only)
    int promote_depth = 0;
};

struct Index : Expr {
    ExprPtr base;
    std::vector<ExprPtr> indices;  // a[i] / a[i,j] / a[i,j,k]
    // sema: the member `operator[]` of a user type (the free-function form is a parse error since
    // build 161: it copied the receiver on every element access)
    const MethodSig* get_method = nullptr;  // borrowed receiver for a struct, the handle for a class
    bool by_ref = false;      // `operator[]` returns a borrow: the expression type is the referent and the value is the element address
    bool ref_const = false;   // whether the borrow is read-only (`const ref T`)
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

// `move x`: gives up ownership of a local variable or parameter (agents/value-semantics.md §8.3).
// sema records which declaration was moved so the codegen can zero the slot and stop destroying it.
struct MoveExpr : Expr {
    ExprPtr operand;
    LocalVar* moved = nullptr;  // sema: the local/parameter whose ownership was transferred
};

struct Binary : Expr {
    std::string op;  // + - * / % << >> & | ^ < <= > >= == != and or
    ExprPtr left;
    ExprPtr right;
    FuncDecl* op_func = nullptr;              // sema: user operator overload (free function)
    const MethodSig* op_method = nullptr;     // sema: member operator on a struct (borrowed receiver)
    bool negate_result = false;               // sema: `a != b` implemented through `operator==` (the result is negated)
    bool swap_operands = false;               // sema: `a > b` derived as `b < a` (the other operand is the receiver)
    // sema: the operator returns a borrow (`ref T operator<<(...)`) — the expression denotes the
    // borrowed storage, like a `ref T f()` call, and its type is the referent.
    bool by_ref = false;
    bool ref_const = false;
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
    // `T()` where `T` is a type parameter: this call is the **zero value** of that type. Filled by the
    // instantiator (substitution knows the binding); a primitive keyword form (`int()`) fills it in the
    // parser. The type is resolved in the use-site unit, like any substituted type.
    TypeSpecPtr zero_spec;
    // sema
    std::string callee_kind;  // "func" | "ctor" | "builtin" | "method" | "super" | "fnptr"
    FuncDecl* func = nullptr;
    TypePtr fn_ty;            // callee_kind == "fnptr": the function type being called
    TypePtr ctor;             // the target type of the struct construction
    TypePtr sum_ty;           // callee_kind == "sum_ctor": the sum type being constructed
    int sum_alt = -1;         // callee_kind == "sum_ctor": which alternative
    std::string builtin;      // alloc / realloc / free
    // `callee_kind == "builtin_const"`: a compiler-provided compile-time function call folded to a value
    long long builtin_value = 0;
    std::vector<int> field_order;
    const MethodSig* method = nullptr;      // the selected method / interface default method
    const std::vector<TypePtr>* ctor_pty = nullptr;  // the selected constructor (used by super)
    bool by_ref = false;      // the called function/method returns a borrow (the expression type is the referent and the value is the address)
    bool ref_const = false;   // whether the borrow is read-only (`const ref T`)
};

// Lambda expression: `(int a) => expr` / `(int a) => { ... }`, optionally with a capture list
// `[x, ref y] (int a) => ...` (U41: `[x]` copies the value into the environment, `[ref x]` stores the
// address, so writes reach the captured variable; the caller owns its lifetime).
// sema lowers it to a hidden `private` function (`synth`). Without captures its value is that
// function's address (`fn`); with captures the value is a `delegate` `{code, env}`.
struct Lambda : Expr {
    // one entry of the capture list, as written
    struct Capture {
        std::string name;
        bool by_ref = false;
        bool by_weak = false;  // `[weak x]`: the environment holds an uncounted (weak) reference
        int line = 0;
        int col = 0;
    };
    std::vector<Capture> captures;  // the parsed list (empty: no captures)
    std::vector<std::unique_ptr<Param>> params;
    ExprPtr expr_body;            // `=> expr`
    std::unique_ptr<Block> block_body;  // `=> { ... }`
    // sema
    FuncDecl* synth = nullptr;
    TypePtr env_ty;                        // synthesized environment class (null when not capturing)
    std::vector<const LocalVar*> env_src;  // the outer variable copied/made addressable (the creation site)
    std::vector<bool> env_src_ref;         // whether that capture is `[ref x]`
    std::vector<bool> env_src_weak;        // whether that capture is `[weak x]`
};

// `x is Alt(a, b)` / `x is Cat d`: a runtime discrimination test (SPEC 10.1.2). For a sum value the
// pattern is an alternative (optionally binding its payload fields); for a class/interface value it is
// a subtype test (optionally binding the downcast as that class). Other types have no runtime
// discrimination and are rejected.
struct IsExpr : Expr {
    ExprPtr obj;
    std::string name;
    std::vector<std::string> binds;
    // sema
    bool is_sum = false;
    TypePtr sum_ty;
    int alt = -1;
    TypePtr test_ty;                      // class/interface test: the class being tested for
    std::vector<TypePtr> bind_types;
    std::vector<LocalVar*> bind_locals;   // declared by the `if` checker (then-branch scope)
    int line = 0;
    int col = 0;
};

struct NewExpr : Expr {
    TypeSpecPtr type;   // the target class type / dynamic array element type
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    bool is_array = false;  // new T[..]
    bool uninit = false;    // `uninit new T[n]`: the element block is not zeroed (trivial elements only)
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

// `swap(a, b)`: exchanges the values at two writable places. Built in rather than a library function
// because it needs the addresses of both places even for types whose address must not be taken (`&` is
// refused for a struct with reference fields), and because a byte exchange is exactly right for a
// reference-counted handle — the counts do not change, so neither retain nor release may run.
struct SwapExpr : Expr {
    ExprPtr a;
    ExprPtr b;
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
    // `export`: this declaration belongs to the binary export surface (implies `public`). Whether an
    // export surface is actually emitted depends on the target and `--exports=public|export`.
    bool is_export = false;
    std::vector<TypeParam> type_params;  // empty = not generic
    bool is_template = false;            // sema: a generic template, not checked/emitted directly, only used for instantiation
    virtual ~Decl() = default;
};

// ---------------------------------------------------------------- conditional lists (P3)
//
// `#if` in a declaration/statement/member list keeps its arms as data so the semantic pass can pick one
// after the constants are known. The parser fills these nodes and never selects an arm itself.

// `#error "msg"` inside a conditional arm: collected by the parser and raised by the semantic pass only
// when the arm is actually selected.
struct ErrorDecl : Decl {
    std::string message;
};

struct DirectiveMembers;

// `promote field;` / `promote field(f, g);` inside a type body (SPEC 3.19).
struct PromoteSpec {
    std::string field;
    std::vector<std::string> methods;  // empty: every public method of the field's type
    int line = 0;
    int col = 0;
};

// A type body's member lists (the parallel vectors of StructDecl/ClassDecl/InterfaceDecl).
struct MemberGroup {
    // A `#if` chain inside the body, at the position where it was written. `*_before` counts the
    // members of each kind declared before the group, so the selected arm is spliced back at the same
    // position (field order is layout; method order is the vtable order).
    struct Group {
        size_t fields_before = 0;
        size_t consts_before = 0;
        size_t methods_before = 0;
        size_t ctors_before = 0;
        size_t promotes_before = 0;
        std::unique_ptr<DirectiveMembers> dir;
    };
    std::vector<std::pair<std::string, TypeSpecPtr>> fields;
    std::unordered_map<std::string, bool> field_visibility;
    // `const T x = expr;`: the default member initializer of a field (SPEC 3.7). A constructor that
    // does not name the field gets this expression (sema synthesizes the init-list entry).
    std::unordered_map<std::string, ExprPtr> field_default;
    std::vector<std::unique_ptr<ConstMember>> const_members;
    std::vector<std::unique_ptr<MethodDecl>> methods;
    std::vector<std::unique_ptr<CtorDecl>> ctors;
    std::unique_ptr<DtorDecl> dtor;
    std::vector<PromoteSpec> promotes;  // `promote field;` (SPEC 3.19)
    std::vector<Group> groups;
    std::vector<std::unique_ptr<ErrorDecl>> errors;  // `#error` inside a conditional member arm
};

struct DirectiveMembers : Node {
    struct Arm {
        ExprPtr cond;  // null: `#else`
        MemberGroup members;
    };
    std::vector<Arm> arms;
};

struct DirectiveDecl : Decl {
    struct Arm {
        ExprPtr cond;  // null: `#else`
        std::vector<std::unique_ptr<Decl>> decls;
    };
    std::vector<Arm> arms;
};

struct UseDecl : Decl {
    std::string unit_name;
};

// Type alias: `type A = B;` — a transparent alias (produces no new type), and each declaration can define only one.
// `type` is a **contextual keyword**: it is parsed as an alias only at the start of a declaration, in the form `type Ident =`.
// `extend I with J;` (SPEC 3.18): the interface `I`'s implementors gain `J`'s default methods in the
// units that can see this declaration. `J` must derive from `I` and add default methods only.
struct ExtendDecl : Decl {
    std::string target;    // `I`
    std::string with;      // `J`
    int line = 0;
    int col = 0;
};

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
    // A `move` parameter takes over the caller's value: `T(move T other)` is the move constructor.
    bool by_move = false;
};

struct GlobalVar : Decl {    std::string name;
    TypeSpecPtr spec;
    InitPtr init;
    bool is_extern = false;
    bool is_const = false;
    // `builtin const T name;` — a compiler-provided compile-time constant: no storage, no address
    // (decided 2026-11-17). The value is bound during sema from the compiler's built-in table.
    bool is_builtin = false;
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
    // `const T x = expr;`: the default member initializer of a field (SPEC 3.7).
    std::unordered_map<std::string, ExprPtr> field_default;
    std::vector<std::unique_ptr<ConstMember>> const_members;  // type-level compile-time constants
    std::vector<std::unique_ptr<MethodDecl>> methods;  // value type methods (static dispatch)
    std::vector<std::unique_ptr<CtorDecl>> ctors;
    std::unique_ptr<DtorDecl> dtor;
    std::vector<PromoteSpec> promotes;  // `promote field;` (SPEC 3.19)
    // The body as parsed: `collect` expands the conditional member groups into the lists above.
    MemberGroup body;
    // Layout modifiers
    bool is_packed = false;
    // `nocopy`: the type must not be copied by value (SPEC 3.13); only meaningful on a struct.
    bool nocopy = false;
    // `nomove`: the type must not be moved either (its address is part of its value, like a node in an
    // intrusive list or a handle registered elsewhere). Only meaningful on a struct.
    bool nomove = false;
    bool is_union = false;  // a union declaration (layout: all members at offset 0)
    ExprPtr align_expr;  // the N of align(N) (the parser keeps the original expression and sema evaluates it)
    TypePtr ty;  // sema: the struct type (the fields are filled in after the placeholder is created)
    // sema: the field table has been resolved (collect may resolve it early for a `sizeof` constant)
    bool fields_resolved = false;
};

struct EnumDecl : Decl {
    std::string name;
    std::vector<std::pair<std::string, std::optional<long long>>> members;
    TypePtr ty;  // sema: the enum type
};

// Sum type (SPEC 10.1, "language-level sum types"): `type Value = Int(int) | Text(string) | None;`
// A value carries a tag plus one active payload. Alternatives may have 0..n payload fields; the
// payload area is laid out like a `union` (all alternatives start at the same offset).
struct SumAlt {
    std::string name;
    std::vector<TypeSpecPtr> fields;  // payload types (empty: a unit alternative)
    int line = 0;
    int col = 0;
};

struct SumDecl : Decl {
    std::string name;
    std::vector<SumAlt> alts;
    // sema
    TypePtr ty;  // the sum type
};

// ---------------------------------------------------------------- Statements

struct Stmt : Node {
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

// A statement-list `#if` (P3.1 skeleton; see DirectiveDecl).
struct DirectiveStmt : Stmt {
    struct Arm {
        ExprPtr cond;  // null: `#else`
        std::vector<StmtPtr> stmts;
    };
    std::vector<Arm> arms;
};

// `#error "msg"` inside a conditional statement arm: raised only if the arm is selected.
struct ErrorStmt : Stmt {
    std::string message;
};

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
    bool ref_const = false;  // a `for const ref` binding: a read-only borrow (cannot write the element)
    bool is_ref_param = false;  // a `ref` parameter: it cannot be rebound (a for-in ref variable can write elements)
    int env_index = -1;   // >= 0: a lambda capture; the storage is field `env_index` of the environment object
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

// C-style `for (init; cond; step)`. An omitted clause means "none": no init, always-true condition,
// no step (`for (;;) { ... }` is an infinite loop). `continue` runs the step clause (C semantics).
struct ForStmt : Stmt {
    std::vector<StmtPtr> init;  // declarations or one expression statement; empty when omitted
    ExprPtr cond;               // nullptr = true
    StmtPtr step;               // one expression statement, or nullptr
    std::unique_ptr<Block> body;
};

struct ForInStmt : Stmt {
    bool by_ref = false;
    // `for const ref x in seq`: a read-only borrow (usable on a `const` sequence, decided 2026-11-17)
    bool ref_const = false;
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
    const MethodSig* len_sig = nullptr;  // the index form: length() -> isize
    const MethodSig* idx_method = nullptr;  // the index form: member operator[](isize) -> T
    std::string cname;
    std::unique_ptr<LocalVar> lvar;  // the loop variable symbol (codegen uses its cname/by_ref)
    // cursor form (6.4.2): contract declared in std.interfaces (IIterator<T>)
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

// One constant pattern of a `switch` / `match` arm: a single value or a half-open range [vlo, vhi)
// (SPEC 6.6, 10.1.2). The two constructs share the type; only the arm node differs.
struct MatchPat {
    ExprPtr lo;
    ExprPtr hi;  // the upper bound of the range (non-null when is_range)
    bool is_range = false;
    bool inclusive = false;  // `..=`: the upper bound is included (sema normalizes it into the vhi of a half-open range)
    // sema
    long long vlo = 0;
    long long vhi = 0;
};

struct SwitchArm : Node {
    std::vector<MatchPat> pats;
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

// ---------------------------------------------------------------- `match` (SPEC 10.1.2)

// One binding of a match arm: `Alt(x)` (value), `Alt(ref x)` or `Alt(const ref x)`.
struct MatchBind {
    std::string name;
    int mode = 0;  // 0 = value, 1 = `ref`, 2 = `const ref`
    int line = 0;
    int col = 0;
};

// One arm. The body is an expression in expression form (`MatchExpr`) and a block in statement form
// (`MatchStmt`); a match never mixes the two forms.
struct MatchArm : Node {
    // One of three kinds, decided by the scrutinee's type in sema:
    //   * alternative mode: `alt` names the variant alternative (with `binds` for its payload);
    //   * constant mode: `pats` holds literals / enum members / ranges (an integer, enum or `char`
    //     scrutinee); a bare alternative name (`None`) arrives here as a single plain `Name` pat and is
    //     reinterpreted as `alt` when the scrutinee is a variant;
    //   * `is_else`: the remaining cases.
    std::string alt;
    bool is_else = false;         // `else:` covers the remaining alternatives
    std::vector<MatchBind> binds;
    std::vector<MatchPat> pats;   // constant mode
    ExprPtr guard;                // optional `if cond` after the pattern
    ExprPtr value;                // expression form
    std::unique_ptr<Block> body;  // statement form
    // sema
    int alt_index = -1;
    std::vector<LocalVar*> bind_locals;  // one local per bound payload field
    int line = 0;
    int col = 0;
};

// `match v { Alt(x): expr; ... }`: produces one value. `match move v` consumes the scrutinee.
struct MatchExpr : Expr {
    ExprPtr scrut;
    std::vector<std::unique_ptr<MatchArm>> arms;
    // sema
    TypePtr sty;
    bool has_else = false;
    bool match_ref = false;  // an arm binds by `ref`: the scrutinee is matched in place, not copied
};

// `match v { Alt(x): { ... } ... }` as a statement.
struct MatchStmt : Stmt {
    ExprPtr scrut;
    std::vector<std::unique_ptr<MatchArm>> arms;
    // sema
    TypePtr sty;
    bool has_else = false;
    bool match_ref = false;
};

struct BreakStmt : Stmt {};struct ContinueStmt : Stmt {};

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
    // `f() const`: a read-only method (decided 2026-11-17)
    bool is_const = false;
    // an operator signature in an interface: `T operator+(T a, T b);` (no this, the parameter list holds all operands)
    bool is_operator = false;
    std::vector<std::unique_ptr<Param>> params;
    std::unique_ptr<Block> body;  // nullptr = an abstract method
    bool is_override = false;
    // `virtual` on a class method: it gets a vtable slot and subclasses may override it. Methods are
    // static-dispatch by default (build 160); an abstract method is implicitly virtual.
    bool is_virtual = false;
    bool is_public = false;  // visible only inside the unit by default
    // sema: materialized from an interface default implementation (U34 M4); the body text belongs to
    // `iface_unit`, so it is re-checked with that unit as the resolution context
    bool from_iface_default = false;
    std::string iface_unit;
    // An interface default whose body reads `this`: the receiver is then used as `Self` (an operator on
    // the receiver, `return this`, passing it to a `Self` parameter), which needs the concrete implementor.
    // Such a default is static-only: it is materialized per implementor and takes no itable slot.
    bool body_self = false;
    // sema
    TypePtr rty;
    std::vector<TypePtr> pty;
    std::vector<std::unique_ptr<LocalVar>> param_locals;
    std::string cname;  // the method symbol name
};

// One entry of a constructor's construction prologue (SPEC 3.12): `Ctor(a, b) : Base(a), field(b) { }`.
// Three kinds share the slot: a base-class constructor call, a delegating call to another constructor of
// the same type, and a field initializer. The entries run in a fixed order (delegate → base → members →
// field initializers in declaration order → body), never in source order.
struct CtorInit : Node {
    enum class Kind { Field, Base, Delegate };
    Kind kind = Kind::Field;     // sema: resolved from `name`
    std::string name;            // field name / base class name / own class name
    std::vector<ExprPtr> args;   // Field: exactly one expression; Base/Delegate: the call arguments
};

struct CtorDecl : Node {
    bool is_implicit = false;  // `implicit T(...)`: usable as an implicit conversion (struct only)
    std::vector<std::unique_ptr<Param>> params;
    // `C(...) : Base(a), x(v) { }` — the construction prologue (SPEC 3.12). A `const` field must appear
    // here; a base-class call and a delegating call share the same slot.
    std::vector<CtorInit> init_list;
    std::unique_ptr<Block> body;
    bool is_public = false;  // visible only inside the unit by default
    // sema
    std::vector<TypePtr> pty;
    std::vector<std::unique_ptr<LocalVar>> param_locals;
    std::string cname;  // the constructor symbol name
    bool has_base_call = false;         // a `Base(...)` item was resolved (omitted → implicit zero-arg)
    // Synthesized because the type declares default member initializers but no constructor of its own
    // (SPEC 3.7): the prologue applies those initializers.
    bool is_synthesized = false;
    std::vector<TypePtr> base_pty;      // sema: the chosen base constructor signature
    bool is_delegating = false;         // `Derived(...) : Derived(args)` — construct through another ctor
    std::vector<TypePtr> delegate_pty;  // sema: the chosen delegate signature
    int delegate_index = -1;            // sema: its position in `ty->ctors` (cycle detection)
    bool is_move = false;  // `T(move T other)`: the move constructor
    bool is_copy = false;  // `T(ref T other)`: the copy constructor
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
    // `const T x = expr;`: the default member initializer of a field (SPEC 3.7).
    std::unordered_map<std::string, ExprPtr> field_default;
    std::vector<std::unique_ptr<ConstMember>> const_members;  // type-level compile-time constants
    std::vector<std::unique_ptr<MethodDecl>> methods;
    std::vector<std::unique_ptr<CtorDecl>> ctors;
    std::unique_ptr<DtorDecl> dtor;
    std::vector<PromoteSpec> promotes;  // `promote field;` (SPEC 3.19)
    // The body as parsed: `collect` expands the conditional member groups into the lists above.
    MemberGroup body;
    TypePtr ty;  // sema reserved
    // sema: the field table has been resolved (collect may resolve it early for a `sizeof` constant)
    bool fields_resolved = false;
};

struct InterfaceDecl : Decl {
    std::string name;
    std::vector<std::string> bases;
    std::vector<TypeSpecPtr> base_specs;  // parallel to bases
    std::vector<std::unique_ptr<MethodDecl>> methods;
    // The body as parsed: `collect` expands the conditional member groups into the list above.
    MemberGroup body;
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
    // `builtin T f();` — a compiler-provided compile-time function (no body, no symbol), 9.3.
    bool is_builtin = false;
    // sema
    TypePtr rty;
    std::vector<TypePtr> pty;
    std::vector<bool> pty_ref;   // whether each parameter is a `ref` borrow (one pointer on the C side)
    std::vector<bool> pty_cref;  // `const ref T`: a read-only borrow
    std::vector<std::unique_ptr<LocalVar>> param_locals;
    // Lambda captures: locals whose storage lives in the environment object (`LocalVar::env_index` is
    // the field). They are declared in the lambda's scope after the parameters, so `param_locals`
    // stays aligned with `pty`.
    std::vector<std::unique_ptr<LocalVar>> capture_locals;
    // Storage for locals that sema synthesizes outside a declaration (currently `match` arm bindings),
    // so the pointers stored in Name::sym stay valid after Sema is destroyed.
    std::vector<std::unique_ptr<LocalVar>> synthetic_locals;
    // `[this]` capture: the lambda body sees `this` as the environment field at `this_capture_index`.
    // A class captures the handle (RC keep-alive); a struct captures a copy, or its address with
    // `[ref this]` (writes reach the original object).
    TypePtr this_capture_ty;
    int this_capture_index = -1;
    bool this_capture_ref = false;
    // Environment fields of `[weak …]` captures, in capture order (the `[weak this]` slot included):
    // the lambda prologue tests them and returns without running the body when any is null.
    std::vector<int> weak_capture_indices;
    std::string cname;
    bool is_entry = false;    // the entry unit's `main`: renamed to `mu_main` and called by the generated wrapper
    // The symbol crosses the C boundary in at least one direction (`extern` imports one, `export`
    // publishes one for DLL consumers), so it uses the C ABI instead of the backend's own convention.
    bool c_abi = false;
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
