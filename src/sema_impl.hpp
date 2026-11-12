// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Semantic analysis internals: Sema class declaration and cross-file helper declarations (for use by sema_*.cpp only).
//
// Member functions are split by feature across sema_decl / sema_types / sema_class1 / sema_class2 /
// sema_const / sema_expr / sema_call; the public interface is in sema.hpp.
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


// ---------------------------------------------------------------- small helpers

inline bool is_integer(const TypePtr& t) { return t && t->is_integer(); }
inline bool is_numeric(const TypePtr& t) { return t && t->is_numeric(); }
inline bool is_void(const TypePtr& t) { return t && t->kind == TypeKind::Void; }
inline bool is_prim(const TypePtr& t) { return t && t->kind == TypeKind::Prim; }
inline bool is_enum_t(const TypePtr& t) { return t && t->kind == TypeKind::Enum; }
// Enums may only be explicitly converted to and from "real integer types" (neither bool nor enums count)
inline bool is_plain_int(const TypePtr& t) { return is_prim(t) && t->is_integer(); }

// Textual form of a type syntax. ast.hpp declares TypeSpec::str(), but its definition lives in parser.cpp;
// a local copy is kept here to avoid a link-time dependency of sema on parser (used only in error messages).
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

// Enum constants must be held by the AST long-term (Name::sym is a raw pointer), and Program has no place to store them.
// Store them in a process-level deque: push_back never invalidates existing elements, so pointers stay valid after Program is destroyed.
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

    // Leaving the scope must also happen when the for-loop body throws (the Python try/finally equivalent)
    struct ScopeGuard {
        Sema& sema;
        explicit ScopeGuard(Sema& s) : sema(s) { sema.push(); }
        ~ScopeGuard() { sema.pop(); }
    };

    // ------------------------------------------------------------ fields

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
    std::vector<std::string> errors_;  // collected errors (multiple-error reporting)
    static constexpr size_t kMaxErrors = 20;
    int loop_depth_ = 0;
    int const_depth_ = 0;  // constant evaluation recursion depth (see DepthGuard)
    std::unordered_map<int, TypePtr> auto_groups_;  // inference result shared by auto in one declaration
    Expr* direct_init_expr_ = nullptr;  // direct-initialization expression currently under check (in-place constructor construction)

    // ------------------------------------------------------------ generics (U34)
    std::unordered_map<std::string, Decl*> generic_templates_;  // "unit.Name" → template declaration
    std::unordered_map<std::string, TypePtr> generic_insts_;    // "unit.Vec<int>" → instantiated type
    std::unordered_map<std::string, FuncDecl*> generic_func_insts_;  // "unit.f<int>" → instantiated function
    std::unordered_set<std::string> generic_busy_;              // being instantiated (recursion guard)
    std::vector<std::unique_ptr<Decl>> generic_decls_;          // instantiation products (lifetime)

    // ------------------------------------------------------------ utilities

    [[noreturn]] void err(const std::string& msg, const Node* node = nullptr) const;

    // Warnings do not abort compilation; they only go to stderr (same format as errors, prefixed with warning).
    void warn(const std::string& msg, const Node* node) const;

    // TypeSpec is not a Node (it carries its own line/col), so it gets a separate error entry point.
    [[noreturn]] void err_ts(const std::string& msg, const TypeSpec* spec) const;

    UnitInfo& info();

    UnitInfo* std_info();

    // The canonical string struct in std.string (its name is just `string`)
    TypePtr str_type();

    bool is_str(const TypePtr& t);

    bool implicit_ok(const TypePtr& src, const TypePtr& dst);

    bool uses_unit(const std::string& uname) const;

    // std.string is always loaded as an implicit dependency, equivalent to having been `use`d.
    bool unit_imported(const std::string& uname) const;

    // ------------------------------------------------------------ visibility
    //
    // Library boundary = first segment of the unit name (`std.string` → `std`; a unit without dots belongs to the root library).
    // Two levels: by default visible only within this unit; `public` is visible across units. Cross-library also requires the target unit itself to be `public unit`.

    static std::string lib_of(const std::string& unit_name);

    // Whether the target unit can be referenced by the current unit: same library always allows it; cross-library requires `public unit`.
    bool unit_visible(const std::string& target) const;

    // Declaration-level visibility: fully visible within this unit; other units require the declaration to be public.
    bool decl_visible(const std::string& owner_unit, bool is_public) const;

    bool type_is_public(const TypePtr& t) const;

    // Type declaration visibility (carries the type name, used for errors).
    void require_type_visible(const TypePtr& t, const TypeSpec* spec) const;

    // ------------------------------------------------------------ main flow

    // ------------------------------------------------------------ collection
    void collect();

    // ------------------------------------------------------------ public signature chain check
    //
    // Types exposed by public declarations must also be public, and their units must be visible; otherwise outsiders get a signature they cannot name types with.

    void check_exposed_type(const TypePtr& t, const std::string& what, const Node* at);

    void check_public_signatures();

    void place_structs();

    void resolve_class_fields(ClassDecl* c, Unit* u);

    // ------------------------------------------------------------ layout modifiers
    //
    // The final layout of packed / align(N) is computed by type_align/type_size in types.cpp;
    // here we only evaluate N, check N against the natural alignment, and enforce the packed field type restrictions.

    bool eval_const_int(Expr* e, long long& out);

    static bool is_pow2(long long n);

    void check_layouts();

    // Type-level compile-time constants: resolve the type and check that "only scalars / enums are allowed"
    void resolve_const_members(std::vector<std::unique_ptr<ConstMember>>& list,
                               const TypePtr& owner, Unit* u, const std::string& kw,
                               const std::string& type_name);

    void resolve_unit_types(Unit* u);
    // ------------------------------------------------------------ type resolution
    TypePtr resolve_type(const TypeSpec* spec, Unit* u);

    // ------------------------------------------------------------ generic instantiation (U34)
    // Collect `Unit::templates` into `generic_templates_` (called once at the end of the collect stage)
    void index_templates();
    // `spec` is a type syntax with arguments (`Vec<int>`); returns the instance type, cloning and registering the template when needed
    TypePtr instantiate_generic(const TypeSpec* spec, Unit* u);
    // `Name<args>` (from a call site, e.g. value construction of a generic struct `Pair<int,long>(...)`)
    TypePtr instantiate_generic_named(const std::string& name,
                                      const std::vector<TypeSpecPtr>& args, Unit* u);
    // Find a generic template by name (this unit first, then a unique suffix match); returns nullptr when none
    Decl* find_generic_template(const std::string& full, Unit* u);
    // Free function monomorphization: `c->type_args` holds the explicit type arguments; returns the instance function (check_func already run)
    FuncDecl* instantiate_func(Call* c, const std::string& base, Unit* u);
    // Structural constraint check (U34 M3b): whether `t` satisfies every bound of `p` (`&` requires all / `|` requires any)
    void check_bound(const TypePtr& t, const TypeParam& p, Unit* u, const Node* at);
    bool satisfies_one(const TypePtr& t, const TypeSpec* bound, Unit* u, std::string* why);

    // Find a type by name in the given unit; with require_pub only public declarations are returned.
    TypePtr find_type_named(const std::string& unit, const std::string& name,
                            bool require_pub);
    // Type alias: lazy resolution + cycle detection (`type A = B; type B = A;` is an error)
    TypePtr resolve_alias(TypeAliasDecl* a, Unit* owner);

    TypePtr resolve_named_type(const std::string& name, Unit* u, const TypeSpec* spec);

    // Type name resolution inside expressions (current unit → public types from use → std.string),
    // used for enum-qualified references such as `Color.Red`. Returns nullptr when not found.
    TypePtr find_type_in_expr(const std::string& name, const Node* at);

    // Find a member by name in the given enum (order follows the declaration; incrementing continues after an explicit value).
    EnumConst* find_enum_member(const TypePtr& et, const std::string& member);

    // Find a type-level compile-time constant by name in a struct / class (including the base class chain).
    ConstMember* find_const_member_in(const TypePtr& t, const std::string& name);

    // Visibility of a type-level constant: public, or in the same unit as the type declaring it (same rule as fields)
    bool const_member_visible(const ConstMember* cm) const;

    // Whether the type (including the base class chain) declares any type-level constant
    bool has_any_const_member(const TypePtr& t);

    // ------------------------------------------------------------ scopes
    // Collect the error and continue: a failure in one declaration/function must not stop the other errors in the same unit from being reported.
    // Temporary state such as scopes is reset afterwards so the next declaration is not polluted.
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

    // ------------------------------------------------------------ unit checks
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

    // Duplicate-name check + evaluation for type-level constants (the value is emitted as a literal constant in codegen)
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

    // Single initialization site: declaration initializers, aggregate members and nested braces all go through here
    void check_init_into(const TypePtr& ty, Init* init, const std::string& what,
                         const Node* node);

    void check_aggregate(InitAggregate* init, const TypePtr& ty, const std::string& what);

    static bool simple_seq(Expr* e);

    // Walk Index/Member down to the innermost binding and determine whether it is const
    bool root_is_const(Expr* e);

    void check_writable(Expr* t);

    bool is_weak_field_access(Expr* e);

    void check_assign_stmt(AssignStmt* s);

    void check_return(ReturnStmt* s);

    void check_cond(Expr* e, const std::string& what);

    // switch case pattern evaluation: only compile-time constants are accepted (literals / enum members)
    long long checked_int_pattern(long long v, const TypePtr& st, Expr* e);

    long long pattern_value(Expr* e, const TypePtr& st);

    std::vector<std::pair<std::string, long long>> enum_members_of(const TypePtr& et);

    void check_switch(SwitchStmt* s);

    // Index shape (SPEC 6.4.1): length() -> usize  +  operator[](C, usize) -> T
    void resolve_index_shape(ForInStmt* s, const TypePtr& st);

    void check_for(ForInStmt* s);
    // ------------------------------------------------------------ assignment compatibility
    static bool contains_reference_type(const TypePtr& t);

    static bool contains_slice_type(const TypePtr& t);

    static bool is_zeroable(const TypePtr& t);

    static bool is_char(const TypePtr& t);

    static bool noncopyable_struct(const TypePtr& t);

    TypePtr check_assign(Expr* e, const TypePtr& dst, const std::string& what);

    bool assignable(Expr* e, const TypePtr& src, const TypePtr& dst);

    // ------------------------------------------------------------ compile-time constant evaluation (U29)
    //
    // Lazy evaluation + memoization + visiting flag: supports forward references and any declaration order,
    // and reports cyclic dependencies with a clear diagnostic. A failed evaluation returns false (the caller falls back to is_const_expr).
    // Division by zero / cyclic dependencies are hard errors and throw SemaError directly.

    struct ConstVal {
        TypePtr ty;
        bool is_float = false;
        long long i = 0;
        double f = 0;
        bool lit = false;   // from an integer literal: may adapt to the declared type (consistent with check_assign)
        bool flit = false;  // from a floating-point literal: may adapt to float / double (consistent with check_expr)
        bool clit = false;  // from a character literal: may adapt bit-pattern-wise to an 8-bit integer (consistent with check_expr)
    };

    // Switch cur_ to the unit owning the constant while it is evaluated (so error locations are correct for cross-unit lazy evaluation)
    struct CurUnitGuard {
        Sema& sema;
        Unit* prev;
        CurUnitGuard(Sema& s, Unit* u) : sema(s), prev(s.cur_) {
            if (u) sema.cur_ = u;
        }
        ~CurUnitGuard() { sema.cur_ = prev; }
    };

    // "currently evaluating" flag: it must also be reset on the exception path, otherwise a later evaluation would falsely report a cyclic dependency
    struct EvalFlagGuard {
        bool& flag;
        explicit EvalFlagGuard(bool& f) : flag(f) { flag = true; }
        ~EvalFlagGuard() { flag = false; }
    };

    // Recursion depth limit for constant evaluation (keeps very long dependency chains from blowing the stack)
    static constexpr int kMaxConstDepth = 512;
    struct ConstDepthGuard {
        Sema& sema;
        explicit ConstDepthGuard(Sema& s) : sema(s) { ++sema.const_depth_; }
        ~ConstDepthGuard() { --sema.const_depth_; }
    };

    // Diagnostics must not treat the declaration marker `const` as part of the type name (`const int` → `int`)
    static std::string type_str_no_const(const TypePtr& t);

    static long long normalize_int(long long v, const TypePtr& t);

    // Sign-extend a two's-complement bit pattern of the given width to 64 bits (same semantics as LLVM AShr)
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

    // Commit the evaluation result to the declared type: the same assignment rules + integer literal adaptation (no extra implicit widening for const)
    void convert_const_to(ConstVal& v, const TypePtr& dst, const Node* at,
                          const std::string& what);

    bool eval_const_member(ConstMember* cm, ConstVal& out);

    bool eval_global_const(GlobalVar* g, const std::string& unit_name, ConstVal& out);

    bool eval_const_name(Name* e, const TypePtr& self_ty, ConstVal& out);

    // Base type of an expression of the form `Type` / `unit.Type` (constant evaluation only; returns nullptr when not found)
    TypePtr const_expr_base_type(Expr* base);

    bool eval_const_member_expr(Member* e, ConstVal& out);

    bool eval_const_binary(Binary* e, const TypePtr& self_ty, ConstVal& out);

    bool eval_const(Expr* e, const TypePtr& self_ty, ConstVal& out);

    // ------------------------------------------------------------ constant expressions
    bool is_const_expr(Init* init);

    bool const_ok(Expr* e);
    // ------------------------------------------------------------ expressions
    TypePtr check_expr(Expr* e, const TypePtr& expected = nullptr);

    TypePtr check_expr_impl(Expr* e, const TypePtr& expected);

    TypePtr check_name(Name* e);

    TypePtr check_member(Member* e);

    TypePtr check_unary(Unary* e);

    // Conditional expression `cond ? a : b` (SPEC §5.2):
    // the condition must be bool; the two arms resolve as "same type → implicit widening → null adaptation", and an explicit `as` is required otherwise.
    TypePtr check_ternary(Ternary* e, const TypePtr& expected = nullptr);

    TypePtr check_binary(Binary* e);

    TypePtr check_cond_type(Expr* e, const std::string& what);

    void adapt_lit(Expr* e, const TypePtr& self_ty, const TypePtr& other);

    bool can_cast(const TypePtr& src, const TypePtr& dst);
    // ------------------------------------------------------------ calls
    TypePtr check_call(Call* c);

    int rank_arg(Expr* e, const TypePtr& aty, const TypePtr& p);

    static bool dominates(const std::vector<int>& a, const std::vector<int>& b);

    static bool is_user_type(const TypePtr& t);

    // Whether it contains RC-managed reference fields (recursive over class/interface/slice/string/array);
    // only fields are considered, not user destructors (a struct with a destructor may still be addressed and used through a pointer).
    static bool has_ref_fields(const TypePtr& t);

    // Borrow-origin check for returned borrows (ref parameter / this / global / pointer·slice indirection / forwarded borrow)
    bool borrow_root_ok(Expr* e);
    // Whether the expression itself is a borrow value (a `ref T f()` call / an index returning a borrow)
    static bool is_borrow_expr(Expr* e);

    // Whether a type can be the referent of a borrow (`ref`): not reference counted, not void, not class/interface/string/slice.
    static bool borrowable_type(const TypePtr& t);

    // Resolve the return type of a method or operator (handles the `ref` prefix + borrow referent validation).
    void resolve_method_ret(MethodDecl* m, Unit* u);
    TypePtr make_borrow_ret(TypeSpec* spec, bool ret_ref, Node* at, Unit* u, const std::string& what);

    // Operator lookup scope: current unit + units brought in by use + units owning the operand types (ADL style).
    // Operators from units that were not imported are invisible, avoiding accidental clashes between unrelated units.
    std::vector<FuncDecl*> collect_operators(const std::string& sym,
                                             const std::vector<TypePtr>& operands = {});

    static std::vector<Expr*> raw_args(const std::vector<ExprPtr>& v);

    FuncDecl* choose_overload(const std::vector<FuncDecl*>& cands,
                              const std::vector<Expr*>& args, const std::string& what,
                              Node* at);

    // Only collect methods along the class chain (interface default methods do not participate in super calls)
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

    // ------------------------------------------------------------ misc
    bool is_lvalue(Expr* e);

    // Whether the expression is "rooted" at a class/interface reference (including its array/slice elements):
    // `c.x`, `h.items[0]` and `catArray[i]` all count. U21: such addresses cannot express the RC lifetime.
    bool rooted_at_reference(Expr* e);

    void check_entry();

};

}  // namespace muc_detail

