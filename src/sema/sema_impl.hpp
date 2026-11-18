// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Semantic analysis internals: Sema class declaration and cross-file helper declarations (for use by sema_*.cpp only).
//
// Member functions are split by feature across sema_decl / sema_types / sema_class1 / sema_class2 /
// sema_const / sema_expr / sema_call; the public interface is in sema.hpp.
#pragma once

#include "sema.hpp"
#include "directive.hpp"
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
            return spec_text(s->elem.get()) + "[" +
                   (s->len ? std::string("N") : std::to_string(s->size)) + "]";
        case TypeSpec::K::Func:
        case TypeSpec::K::Delegate: {
            std::string out = s->kind == TypeSpec::K::Delegate ? "delegate (" : "(";
            for (size_t i = 0; i < s->params.size(); ++i) {
                if (i) out += ", ";
                out += spec_text(s->params[i].get());
            }
            out += ") => ";
            out += spec_text(s->ret.get());
            return out;
        }
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

// `a`, `a.b`, `a.b.c` → the dotted name; empty for anything else (a call, an index, ...).
std::string expr_dotted_name(const Expr& e);

// The constants visible to a `#if` condition at one point of the program (see `Sema::directive_scope`).
struct DirectiveScope {
    UserConstTable table;
    std::unordered_set<std::string> ambiguous;  // bare names coming from several `use`d units
    bool comptime = false;                      // `use std.comptime` is visible at this position
};

// Where an expanded type body's members go (a struct/class has all lists, an interface only methods).
struct MemberSink {
    std::vector<std::pair<std::string, TypeSpecPtr>>* fields = nullptr;
    std::unordered_map<std::string, bool>* field_visibility = nullptr;
    std::unordered_map<std::string, ExprPtr>* field_default = nullptr;
    std::vector<std::unique_ptr<ConstMember>>* const_members = nullptr;
    std::vector<std::unique_ptr<MethodDecl>>* methods = nullptr;
    std::vector<std::unique_ptr<CtorDecl>>* ctors = nullptr;
    std::unique_ptr<DtorDecl>* dtor = nullptr;
};

// ---------------------------------------------------------------- capacity limits (SPEC 9.4, C++ [implimits])
//
// The numbers are C++'s informative minima where they exist, and every one of them is far above real
// code. The hierarchy and generic limits are also the depth bounds of the walkers that compute
// subclass / interface relations (see sema_class1.cpp), so a program that compiles never keeps a
// relation computed by a truncated walk.

// Instantiations in one chain (`Box<Box<…>>`); C++ [implimits] §1.9 nested template instantiations: 1024.
constexpr int kGenericDepthLimit = 256;
// Total instances per program. Depth alone does not bound the work (`Pair<T, T>` doubles the count per
// level); real code uses a few hundred, and the standard library a few dozen.
constexpr size_t kGenericInstanceLimit = 4096;
// Class inheritance chain and interface inheritance chain (C++ sets no minimum for these).
constexpr int kMaxInheritanceDepth = 256;
constexpr int kMaxInterfaceDepth = 256;
// Enumeration constants in one enum: C++ [implimits] §1.22 sets 4096. The members share the unit's
// unqualified namespace and each one is interned for the whole process.
constexpr size_t kMaxEnumMembers = 4096;
// Non-static data members of one struct/class: C++ [implimits] §1.20 sets 16384 (and 4096 members in a
// single member-specification, §1.28; Mulan has no out-of-line members, so the smaller fits both).
constexpr size_t kMaxFields = 4096;
// Case labels of one switch: C++ [implimits] §1.19 sets 16384.
constexpr size_t kMaxSwitchLabels = 16384;

// ---------------------------------------------------------------- shared signature helpers
//
// `Self` denotes the implementor (SPEC 3.15.6) but a declared signature keeps the interface type in
// that slot, so every comparison against an implementation substitutes first. These live here because
// the declaration checks (sema_class1.cpp) and the interface materialization (sema_iface.cpp) both use
// them, and a second copy would drift.

    // The interface and base-class graphs are DAGs, so a plain recursion visits shared ancestors once per
    // path — exponentially many times in a diamond, which a generated program can exploit (`interface
    // A{i} : A{i-1}, B{i-1}` doubles the paths per level). Every walk below takes a visited set.
inline bool first_visit(std::unordered_set<const Type*>* seen, const TypePtr& t) {
        if (!seen) return true;
        return seen->insert(t.get()).second;
    }

inline bool mentions_self(const MethodSig& sig) {
        if (sig.ret_self) return true;
        for (bool s : sig.params_self)
            if (s) return true;
        return false;
    }

    // `name(T1, T2)` with the implementor already substituted: the diagnostic has to name the exact
    // signature the implementor is missing (SPEC 3.10), not just the method name.
inline std::string sig_text(const MethodSig& sig) {
        std::string out = sig.name + "(";
        for (size_t i = 0; i < sig.params.size(); ++i) {
            if (i) out += ", ";
            out += sig.params[i] ? sig.params[i]->str() : std::string("?");
        }
        out += ")";
        return out;
    }

    // `Self` in an interface member signature denotes the implementor (SPEC 3.15.6), but the declared
    // signature keeps the interface type in that slot. Comparing a declaration against an implementation
    // must therefore substitute the implementor first: the constraint check already substitutes
    // (sema_generic.cpp), and without the same substitution here the `:` check and the constraint check
    // disagree, so no type can satisfy both.
inline MethodSig impl_sig(const MethodSig& sig, const TypePtr& impl) {
        MethodSig out = sig;
        // Operator members keep their own rule (the receiver is dropped from the member form, and the
        // remaining operands are matched as declared — SPEC 3.11/3.15.5), so they are left untouched.
        if (out.is_operator) return out;
        if (out.ret_self) {
            out.ret = impl;
            out.ret_self = false;
        }
        for (size_t i = 0; i < out.params_self.size() && i < out.params.size(); ++i) {
            if (out.params_self[i]) {
                out.params[i] = impl;
                out.params_self[i] = false;
            }
        }
        return out;
    }

class Sema {
public:
    Sema(std::vector<std::unique_ptr<Unit>> units, std::string entry, bool single_thread = false,
         bool strict = false);

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
    // `--single-thread`: reference counting is non-atomic, so concurrency constructs are rejected.
    bool single_thread_ = false;
    std::unordered_map<std::string, UnitInfo> infos_;
    // `builtin` declarations seen so far, name -> declaring unit (SPEC 9.3: once per program).
    std::unordered_map<std::string, std::string> builtin_owner_;
    // Cache for `directive_const_table()` (one table per unit being checked).
    Unit* const_table_unit_ = nullptr;
    UserConstTable const_table_;
    // While `collect` walks a unit's declarations, the constants declared so far are the visible ones
    // (text order). A `sizeof(struct)` in a const initializer resolves that struct's fields right there,
    // and an array bound inside it (`struct S { int[N] v; }`) needs this table (#82).
    const UserConstTable* collect_consts_ = nullptr;
    // Every `const T` view created by `resolve_type`, grouped by its canonical type (`cv_views_[T]`).
    // `refresh_cv_views` copies the final shape of T into each view. Both sides are weak/shared on
    // purpose: a view is often a temporary (a discarded conversion probe), and the map keeps the
    // canonical type alive, so the refresh never writes into freed memory.
    std::unordered_map<const Type*, std::pair<TypePtr, std::vector<std::weak_ptr<Type>>>> cv_views_;
    Unit* cur_ = nullptr;
    FuncDecl* cur_func_ = nullptr;
    TypePtr cur_ret_;
    std::string cur_name_;
    TypePtr cur_class_;
    bool in_ctor_ = false;
    bool in_dtor_ = false;
    // `f() const`: writes through `this` are rejected (decided 2026-11-17)
    bool cur_method_const_ = false;
    bool in_finally_ = false;
    std::vector<std::unique_ptr<LocalVar>> this_local_;
    // Set while a method body is checked: the receiver `this` was read as a value (see
    // MethodDecl::body_self). Reset by begin_member, read by check_interface.
    bool member_used_this_ = false;
    // Receiver-dropped views of interface operator contracts, resolved inside an interface default body
    // (`this[0]`). Owned here so the pointer handed back to the expression stays valid (the AST keeps it).
    std::vector<std::unique_ptr<MethodSig>> op_sig_cache_;
    std::vector<Scope> scopes_;
    std::vector<std::string> errors_;  // collected errors (multiple-error reporting)
    static constexpr size_t kMaxErrors = 20;
    int loop_depth_ = 0;
    int const_depth_ = 0;  // constant evaluation recursion depth (see DepthGuard)
    std::unordered_map<int, TypePtr> auto_groups_;  // inference result shared by auto in one declaration
    Expr* direct_init_expr_ = nullptr;  // direct-initialization expression currently under check (in-place constructor construction)
    // Inside a `return` statement: an array-typed return still copies (array returns are a Mu
    // extension; C99 has no array prvalues), while assignment/copy-initialization of arrays is rejected.
    bool in_return_ = false;
    // 9.1: the unit-level variable whose initializer is being checked, and the variables of the
    // current unit that have already been checked (they are initialized before it).
    GlobalVar* cur_global_ = nullptr;
    std::vector<GlobalVar*> unit_globals_so_far_;
    // While an interface default body is re-checked for a struct, that struct's unit: the body may
    // call any interface method S implements, including a private one. The same body is also checked
    // strictly in its declaring unit (check_interface), so this relaxation opens no new access.
    std::string extra_visible_unit_;

    // ------------------------------------------------------------ lambdas (3.16)
    // One entry per lambda body currently being checked: the scope depth at entry (locals below it are captures)
    // plus the types seen at `return`.
    struct LambdaCtx {
        std::size_t scope_base = 0;
        std::vector<std::pair<ReturnStmt*, TypePtr>> rets;
        std::vector<ReturnStmt*> bare;
    };
    std::vector<LambdaCtx> lambda_stack_;
    int lambda_seq_ = 0;

    // ------------------------------------------------------------ para sections (3.17.2)
    // One entry per section currently being checked. `seen` collects the enclosing locals the section
    // reads; `declared` is the set of locals it declares itself (they are not captures).
    struct ParaCtx {
        std::size_t scope_base = 0;
        int loop_base = 0;
        std::size_t lambda_base = 0;
        std::vector<LocalVar*> seen;
        std::unordered_set<LocalVar*> declared;
    };
    std::vector<ParaCtx> para_stack_;

    void check_para(ParaStmt* p);
    void check_para_for(ParaForStmt* p);
    // Set while a `para for` body is being checked: `break`/`continue` at the body level are errors
    // (a region ends when its source ends, or by cancellation; the body is one task per item).
    bool pending_para_for_ = false;
    bool in_para_for_body_ = false;
    int para_for_body_depth_ = 0;

    // ------------------------------------------------------------ `move` (agents/value-semantics.md §8)
    // Variables whose ownership was transferred: every later use is an error until the variable is
    // reassigned. The analysis is per function body: `if`/`else` merge the two arms, and a `move` may not
    // target a variable declared outside the enclosing loop (iteration 2 would reuse a moved value).
    std::unordered_set<LocalVar*> moved_locals_;
    std::unordered_map<LocalVar*, int> local_loop_level_;
    int move_loop_count_ = 0;
    // Why a value cannot be copied, phrased as the rule rather than the verdict: a destructor is a
    // declaration that the type owns a resource, so the author has to say how it is duplicated
    // (agents/value-semantics.md §8.2).
    std::string noncopyable_reason(const TypePtr& t) const;

    TypePtr check_move(MoveExpr* mv);    // `x = ...` makes a moved variable usable again.
    void note_assigned_local(const std::string& name);
    // `swap(a, b)`: two writable places of the same type.
    TypePtr check_swap(SwapExpr* sw);    // Reset the per-body state when a function/method/constructor/destructor body starts.
    void begin_move_scope() {
        moved_locals_.clear();
        local_loop_level_.clear();
        move_loop_count_ = 0;
    }

    // True when the expression is rooted at an enclosing value-typed local: a `para` section may not
    // write it, take its address or pass it by `ref` (the storage is the suspended caller's frame).
    bool roots_at_outer_value_local(Expr* e) const;

    // ------------------------------------------------------------ generics (U34)
    std::unordered_map<std::string, Decl*> generic_templates_;  // "unit.Name" → template declaration
    std::unordered_map<std::string, TypePtr> generic_insts_;    // "unit.Vec<int>" → instantiated type
    std::unordered_map<std::string, FuncDecl*> generic_func_insts_;  // "unit.f<int>" → instantiated function
    std::unordered_set<std::string> generic_busy_;              // being instantiated (recursion guard)
    // Nesting depth of the instantiation being expanded. A template whose next instance is a strictly
    // larger type argument (`Deep<T>` → `Deep<Box<T>>` → ...) never repeats a key, so the exact-key
    // guard cannot see it; the depth limit turns that into a diagnostic instead of a stack overflow.
    int generic_depth_ = 0;
    // Contract checking (build 156): while the body of a constrained generic function is checked at
    // its definition, each type parameter is bound to a witness type that carries exactly the
    // members the constraint promises. `resolve_type` consults this map before normal lookup, so the
    // template body can be checked as written — no clone, no substitution, no emission.
    std::unordered_map<std::string, TypePtr> type_bind_;
    std::unordered_set<const FuncDecl*> contract_checked_;
    // Templates that actually produced an instance (used by `--strict` to report unchecked bodies).
    std::unordered_set<Decl*> instantiated_templates_;
    // `--strict`: warn about a generic that is never instantiated, so its body was never checked.
    bool strict_ = false;
    // Innermost-last stack of "while instantiating ..." lines; every diagnostic raised while the
    // stack is non-empty carries them (see `err`). Mutable because diagnostics are `const` members.
    mutable std::vector<std::string> instantiation_notes_;
    size_t generic_instance_count_ = 0;
    std::vector<std::unique_ptr<Decl>> generic_decls_;          // instantiation products (lifetime)
    std::vector<std::unique_ptr<Decl>> synth_decls_;            // synthesized lambda environment classes (lifetime)

    // ------------------------------------------------------------ utilities

    [[noreturn]] void err(const std::string& msg, const Node* node = nullptr) const;

    // Warnings do not abort compilation; they only go to stderr (same format as errors, prefixed with warning).
    void warn(const std::string& msg, const Node* node) const;

    // TypeSpec is not a Node (it carries its own line/col), so it gets a separate error entry point.
    [[noreturn]] void err_ts(const std::string& msg, const TypeSpec* spec) const;

    // Rendered instantiation chain for a diagnostic (innermost first); empty when none is active.
    std::string inst_notes_text() const;

    UnitInfo& info();

    UnitInfo* std_info();

    // std.exception.Exception: the root of every throwable/catchable class
    UnitInfo* exception_info();

    TypePtr exception_type();

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
    // A `const T` view is a copy of the type node (cv is part of the type). The copy is taken while T may
    // still be a placeholder (its fields/methods are filled later), so every view is refreshed from its
    // canonical type once all shapes are final.
    void refresh_cv_views();
    // `std.comptime` declares the compiler-provided constants; this cross-checks its target enums
    // against the compiler's table (see directive.hpp).
    void check_comptime_unit();

    // ------------------------------------------------------------ conditional lists (P3)
    // The constants visible to a `#if` at this point: the public constants of the `use`d units (and
    // the compiler builtins after `use std.comptime`), then `own` — this unit's constants — which wins.
    // `before_line` is the source line of the declaration being processed: a `use` written later in the
    // unit is not visible to it (SPEC 9.2, the source-position rule); 0 means "no limit" (a function
    // body, which is checked after the unit is collected).
    DirectiveScope directive_scope(Unit* u, const UserConstTable* own, int before_line = 0);
    // The compile-time constants of the unit being checked, with the `std.comptime` qualified forms
    // (`std.comptime.ptr_bits`). Cached per unit: `eval_const_int` is called for every `step`,
    // `align(N)`, `switch` range and array length, and the table does not change after `collect`.
    const UserConstTable* directive_const_table();
    // Picks the selected arm of a `#if` chain; null when nothing is selected (`#if` without a matching
    // arm and no `#else`).
    DirectiveDecl::Arm* choose_decl_arm(DirectiveDecl& dir, const DirectiveScope& scope);
    DirectiveStmt::Arm* choose_stmt_arm(DirectiveStmt& dir, const DirectiveScope& scope);
    DirectiveMembers::Arm* choose_member_arm(DirectiveMembers& dir, const DirectiveScope& scope);
    // Expands the conditional member groups of a struct/class/interface declaration (no-op if none).
    void expand_type_decl(Decl* d, Unit* u, const UserConstTable* own);
    // Expands the conditional member groups of a type body into `sink` (in source order).
    void expand_member_body(MemberGroup& body, const DirectiveScope& scope, const MemberSink& sink,
                            const Node* at);
    // Splices the selected arm of a statement-list `#if` into the block at `i`.
    void expand_directive_stmt(Block* b, size_t& i);
    // A `#if` condition may not name a local variable (SPEC 9.2); the statement path calls this before
    // evaluating the arms.
    void check_directive_locals(const Expr* e);
    // Splices every statement-list `#if` of a block (no statement checks): the constructor path needs the
    // list expanded before it looks for a leading `super(...)`.
    void expand_directive_stmts(Block* b);

    // ------------------------------------------------------------ public signature chain check
    //
    // Types exposed by public declarations must also be public, and their units must be visible; otherwise outsiders get a signature they cannot name types with.

    void check_exposed_type(const TypePtr& t, const std::string& what, const Node* at);

    void check_public_signatures();

    void place_structs();

    void resolve_class_fields(ClassDecl* c, Unit* u);
    // Struct field table (idempotent; collect may resolve it early for a `sizeof` constant).
    void resolve_struct_fields(StructDecl* sd, Unit* u);
    // Resolves the type arguments of `sizeof(T)` in a const initializer, so collect can fold it.
    void resolve_sizeof_types(const Expr* e, Unit* u);
    // Sum types: payload types + the synthesized `{ i32 tag; union payload }` layout.
    void resolve_sum(SumDecl* sd, Unit* u);

    // ------------------------------------------------------------ layout modifiers
    //
    // The final layout of packed / align(N) is computed by type_align/type_size in types.cpp;
    // here we only evaluate N, check N against the natural alignment, and enforce the packed field type restrictions.

    // `bits` / `sgn` report the width of the folded value (32 for an `int` operand, 64 for a `long`).
    bool eval_const_int(Expr* e, long long& out, int* bits = nullptr, bool* sgn = nullptr);

    static bool is_pow2(long long n);

    void check_layouts();
    void check_instance_layouts();
    // Drops the units that are no longer reachable from the entry through `use` after `collect` spliced
    // the selected `#if` arms: a `use` in an unselected arm must not pull its unit into the program.
    void prune_unreachable_units();

    // Type-level compile-time constants: resolve the type and check that "only scalars / enums are allowed"
    void resolve_const_members(std::vector<std::unique_ptr<ConstMember>>& list,
                               const TypePtr& owner, Unit* u, const std::string& kw,
                               const std::string& type_name);

    void resolve_unit_types(Unit* u);
    // ------------------------------------------------------------ type resolution
    TypePtr resolve_type(const TypeSpec* spec, Unit* u);

    // ------------------------------------------------------------ generic instantiation (U34)
    // Registers one template in `generic_templates_` (called as each template leaves the declaration
    // list, so an instantiation later in the same unit can find it)
    void index_template(Decl* d, const std::string& unit_name);
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
    // Contract checking at the definition site (build 156)
    TypePtr make_contract_witness(const TypeParam& tp, Unit* u, const std::string& owner);
    void add_contract_members(const TypePtr& witness, const TypePtr& iface);
    void check_generic_contracts();
    // `--strict`: report generic templates that were never instantiated (their bodies were not checked).
    void warn_uninstantiated_generics();

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
            in_finally_ = false;
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
    // SPEC 9.3: a `builtin` name is declared once per program and must match the compiler's table.
    void check_builtin_owner(const std::string& name, const Node* at);
    void check_builtin_func(FuncDecl* f);
    // 9.1: a unit-level initializer must not observe a variable that is initialized after it.
    void check_global_init_order(GlobalVar* g, Expr* at);

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

    // `impl` (when given) is the implementor whose `Self` slots replace the interface's placeholder before
    // comparing: `Self` denotes the implementor (SPEC 3.15.6), and the declared signature stores the
    // interface type in that slot.
    static const MethodSig* find_sig(const TypePtr& t, const MethodSig& sig, int depth = 0,
                                     std::unordered_set<const Type*>* seen = nullptr,
                                     const TypePtr& impl = nullptr);

    static void collect_sigs(const TypePtr& t, const MethodSig& sig,
                             std::vector<const MethodSig*>& out, int depth = 0,
                             std::unordered_set<const Type*>* seen = nullptr,
                             const TypePtr& impl = nullptr);

    static const MethodSig* find_method(const TypePtr& t, const std::string& name, size_t argc,
                                        int depth = 0,
                                        std::unordered_set<const Type*>* seen = nullptr);

    static bool has_method_named(const TypePtr& t, const std::string& name, int depth = 0,
                                 std::unordered_set<const Type*>* seen = nullptr);

    void check_method(ClassDecl* c, MethodDecl* m);

    void check_ctor(ClassDecl* c, CtorDecl* ctor);
    // Validate `C(...) : Base(a), x(v) { }` — the construction prologue (SPEC 3.12)
    void check_ctor_prologue(const TypePtr& ty, CtorDecl* ctor,
                             const std::unordered_map<std::string, ExprPtr>& defaults);
    // A type with default member initializers and no constructor of its own gets a zero-argument one.
    void synthesize_default_ctor(StructDecl* sd);
    void synthesize_default_ctor(ClassDecl* c);
    void check_ctor_delegate_cycles(const std::vector<std::unique_ptr<CtorDecl>>& ctors);
    // `const T x = expr;`: fields with a default member initializer join the constructor initializer
    // list when the constructor does not name them (SPEC 3.7). The expression is cloned per
    // constructor: it is checked in that constructor's scope and emitted into its prologue.
    void apply_field_defaults(const std::unordered_map<std::string, ExprPtr>& defaults,
                              const std::vector<Field>& fields, CtorDecl* ctor);

    void check_dtor(ClassDecl* c);

    void check_struct_method(StructDecl* sd, MethodDecl* m);

    void check_struct_ctor(StructDecl* sd, CtorDecl* ctor);

    void check_struct_dtor(StructDecl* sd);

    // Duplicate-name check + evaluation for type-level constants (the value is emitted as a literal constant in codegen)
    void check_const_members(std::vector<std::unique_ptr<ConstMember>>& list, const TypePtr& ty,
                             const std::string& kw, const std::string& type_name);

    void check_struct(StructDecl* sd);

    // U34 M4: the default implementation of `sig` declared by interface `itf` (walking the interface
    // inheritance chain, most derived first); `owner` receives the declaring interface. nullptr when
    // every declaration on the chain is abstract.
    MethodDecl* find_iface_default_decl(const TypePtr& itf, const MethodSig& sig,
                                        TypePtr* owner = nullptr, int depth = 0,
                                        std::unordered_set<const Type*>* seen = nullptr);

    // Materialize that default as an ordinary method of `sd`: the body is cloned and re-checked with
    // `this` = S, so its calls resolve statically. Layout, vtable and `super` are unaffected.
    void materialize_iface_default(StructDecl* sd, const TypePtr& itf, const MethodSig& sig);
    // Same, for any implementor: `ty`/`decls`/`type_name` describe the type that gains the method.
    void materialize_iface_default(const TypePtr& ty, std::vector<std::unique_ptr<MethodDecl>>& decls,
                                   const std::string& type_name, const TypePtr& itf,
                                   const MethodSig& sig);

    // U33/U34: check `struct S : I...` and materialize the missed defaults. Own pass, before any body
    // is checked, so that call sites elsewhere already see the materialized signatures.
    void apply_struct_iface_defaults(StructDecl* sd);
    void apply_class_iface_defaults(ClassDecl* c);

    void check_class(ClassDecl* c);
    // `const class`: an override of a mutating virtual of the base takes the slot's (non-const) signature;
    // runs after every shape is final and before any body is checked (SPEC 3.7).
    void fix_const_class_slots(ClassDecl* c);

    void check_interface(InterfaceDecl* itf);

    // Validate an interface operator declaration and record its receiver borrow mode on `sig`.
    // Shared by the regular pass and generic-interface instantiation (templates are not checked
    // until they are instantiated).
    void init_interface_operator(MethodDecl* m, MethodSig& sig, const std::string& iface_name);

    static bool class_has_concrete(const TypePtr& cls, const MethodSig& req);

    static bool iface_has_default(const TypePtr& t, const MethodSig& req, int depth = 0,
                                  std::unordered_set<const Type*>* seen = nullptr,
                                  const TypePtr& impl = nullptr);

    static void collect_abstract_reqs(const TypePtr& t, std::vector<MethodSig>& out,
                                      int depth = 0, std::unordered_set<const Type*>* seen = nullptr);

    static void collect_defaults(const TypePtr& t, std::vector<MethodSig>& out, int depth = 0,
                                 std::unordered_set<const Type*>* seen = nullptr);

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
    // A mutable `ref` binding hands out write access, so it needs a writable object (SPEC 3.7).
    bool ref_target_is_readonly(Expr* a);
    // A value subobject of the object being constructed/destroyed (the only writable phase).
    bool is_this_value(Expr* e);
    bool root_is_this(Expr* e);

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
    void resolve_cursor_shape(ForInStmt* s, const TypePtr& st);

    void check_for(ForInStmt* s);
    // ------------------------------------------------------------ assignment compatibility
    static bool contains_reference_type(const TypePtr& t);

    static bool contains_slice_type(const TypePtr& t);
    // Sum types are built-in and their layout is an implementation detail: never part of a C signature.
    static bool contains_sum_type(const TypePtr& t);
    // A delegate inside a struct has the same cross-C problem as a delegate parameter.
    static bool contains_delegate_type(const TypePtr& t);

    static bool is_zeroable(const TypePtr& t);

    static bool is_char(const TypePtr& t);

    static bool noncopyable_struct(const TypePtr& t);

    TypePtr check_assign(Expr* e, const TypePtr& dst, const std::string& what);

    bool assignable(Expr* e, const TypePtr& src, const TypePtr& dst);

    // Implicit struct construction (3.6): `dst` declares an `implicit` single-parameter constructor
    // that accepts `src`. At most one step (the parameter is matched by the built-in conversions only).
    const ImplicitConv* find_implicit_conv(const TypePtr& dst, const TypePtr& src);

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

    TypePtr check_name(Name* e, const TypePtr& expected = nullptr);

    TypePtr check_member(Member* e, const TypePtr& expected = nullptr);

    // Lambda expression (3.16): lowers to a hidden private function and yields its address.
    TypePtr check_lambda(Lambda* e);

    // The type of a function name used as a value (3.16): picks the overload that matches `expected`.
    TypePtr func_value_type(const std::vector<FuncDecl*>& cands, const std::string& what,
                            const TypePtr& expected, FuncDecl** chosen, Node* at);

    // Call through a function value: `fp(args)`
    TypePtr check_fnptr_args(Call* c, const TypePtr& ft);

    TypePtr check_unary(Unary* e);

    // Conditional expression `cond ? a : b` (SPEC §5.2):
    // the condition must be bool; the two arms resolve as "same type → implicit widening → null adaptation", and an explicit `as` is required otherwise.
    TypePtr check_ternary(Ternary* e, const TypePtr& expected = nullptr);

    TypePtr check_binary(Binary* e);

    TypePtr check_cond_type(Expr* e, const std::string& what);

    void adapt_lit(Expr* e, const TypePtr& self_ty, const TypePtr& other);

    bool can_cast(const TypePtr& src, const TypePtr& dst);
    // ------------------------------------------------------------ calls
    TypePtr check_call(Call* c, const TypePtr& expected = nullptr);

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
    // C99 6.7.5.3p7: a parameter declared as an array of T is adjusted to a pointer to T (the bound
    // is not part of the type, so `sizeof` inside the callee yields the pointer size). Only the
    // outermost dimension decays: `int[2][3] rows` becomes a pointer to `int[3]`. Arrays whose
    // element is a class/interface keep value semantics — this language has no pointer to a
    // reference (3.8), so the parameter copies the handles (retain per element).
    static TypePtr decay_param_type(const TypePtr& t);

    // Resolve the return type of a method or operator (handles the `ref` prefix + borrow referent validation).
    void resolve_method_ret(MethodDecl* m, Unit* u);
    TypePtr make_borrow_ret(TypeSpec* spec, bool ret_ref, Node* at, Unit* u, const std::string& what);

    // Operator lookup scope: current unit + units brought in by use + units owning the operand types (ADL style).
    // Operators from units that were not imported are invisible, avoiding accidental clashes between unrelated units.
    std::vector<FuncDecl*> collect_operators(const std::string& sym,
                                             const std::vector<TypePtr>& operands = {});

    // Member operator on a struct (`s[i]`, `s + t`): the receiver is `this` (a borrowed pointer), so the
    // operation costs no copy and no RC. Returns nullptr when the struct declares no such operator.
    const MethodSig* find_member_operator(const TypePtr& recv, const std::string& name,
                                          const std::vector<Expr*>& args, Node* at);

    // Arity / return-type validation for an operator declared inside a struct body (3.11).
    void check_member_operator_sig(MethodDecl* m, const TypePtr& owner);

    static std::vector<Expr*> raw_args(const std::vector<ExprPtr>& v);

    FuncDecl* choose_overload(const std::vector<FuncDecl*>& cands,
                              const std::vector<Expr*>& args, const std::string& what,
                              Node* at, bool check_ref = false);

    // Only collect methods along the class chain (interface default methods do not participate in super calls)
    void collect_class_methods(const TypePtr& t, const std::string& name,
                               std::vector<const MethodSig*>& out, int depth = 0);

    void collect_methods(const TypePtr& t, const std::string& name,
                         std::vector<const MethodSig*>& out, int depth = 0);

    // Keep only the overloads callable on a `const` receiver (or prefer the non-const ones on a
    // mutable receiver), C++ rules (decided 2026-11-17).
    void filter_methods_for_receiver(std::vector<const MethodSig*>& ms, bool recv_const);

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
    // `x is Alt(a, b)` / `x is Cat d` (SPEC 10.1.2)
    TypePtr check_is(IsExpr* e);

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


