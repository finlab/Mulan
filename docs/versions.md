# Version history

## Rules

- Version format: `major.minor.build`, starting from **0.0.1**.
- **After each official build passes the full test suite, the build number +1**; recorded in this file by the maintainer.
- **Only changes to the compiler code raise the build number**: documentation-only, comment-only or config/CI-text changes are recorded inside the current entry's follow-up paragraph instead of bumping the version.
- **Major / minor version numbers are raised only on explicit request**, otherwise they stay unchanged.
- The single source of truth for the version number is the `VERSION` file at the repository root; CMake, the compiler `--version` and the generated C code all read from it.

## Releases

### 0.0.83 — 2026-11-12

**Conservative module-local may-throw optimization**: added `src/may_throw.cpp`, a whole-program fixpoint over visible function, constructor and struct-method bodies. A call whose callee is proven not to return with a pending exception skips the B2 pending-flag check; free functions, operators, constructors, local struct construction, `new`, `super(...)`, `for-in` `length()`/`operator[]`, and struct methods use the summary. Class/interface dispatch, external/precompiled/static-library/dynamic-library and otherwise unknown targets are conservatively may-throw, so the optimization does not constrain the future library ABI. Catch barriers are over-approximated; destructors and `finally` are nothrow (an escaping exception aborts). Added 3 runtime cases (nothrow free/recursive/struct-method code, may-throw struct methods, and a cleanup-free struct constructor that throws).

**Tests**: 418/418 (113 pass + 289 must-fail + 16 check)

### 0.0.82 — 2026-11-12

**Exceptions stage 2: `finally`**: `finally` now runs on every exit of the `try` statement — normal completion, a matched `catch`, `return`, `break`, `continue`, and an exception that is caught or propagated — exactly once per exit. Nested `try/finally` runs inner before outer. The finally body is emitted by duplicating it on each exit path, so no platform EH support is needed. A `finally` body must not use `return` / `break` / `continue` / `throw` directly (compile errors); a nested `try/catch` is allowed, but an exception escaping it aborts (cleanup must not throw). While the finally body runs, the pending exception is saved and temporarily cleared, so ordinary calls inside `finally` do not observe the in-flight exception; the original pending state is restored afterwards. Interface catch is explicitly not supported: `catch` accepts only classes derived from `Exception`. Added 10 runtime cases (normal/catch/no-match/return/catch-return/break/continue/nested/throw-from-catch/inner-catch) and 4 must-fail cases; removed the old "finally unsupported" error case.

**Tests**: 415/415 (110 pass + 289 must-fail + 16 check)

### 0.0.81 — 2026-11-12

**Exceptions stage 1 (B2 lowering)**: implemented `throw`, `try`, and `catch` (class matching in written order over the descriptor base chain), read-only catch bindings, RC cleanup on unwinding, constructor-throw cleanup, uncaught `abort` at `main`, and `std.exception.Exception` as an implicit standard-library unit. The function ABI is unchanged: a thread-local pending slot (`flag` + object pointer) is checked after every mu_lan call; a throwing function releases its active scopes and returns a dummy value with the flag set, and callers propagate it until a matching `catch`. `extern "C"` calls are not checked. No `.eh_frame`/`.pdata`/personality/unwind runtime is involved, so Windows/Linux/macOS share one lowering and the libc-only portability is preserved. `finally` was not yet implemented and an interface is not a valid `catch` type (catch accepts `Exception` subclasses only); may-throw analysis is stage 3. Added `libs/std/exception.mu`, 12 runtime cases (including a generic function carrying try/throw, a destructor that calls a mu_lan method while unwinding, a destructor that catches a local throw, and exception-object ownership across a catch), and 5 must-fail cases. Cleanup runs with the pending flag temporarily cleared, so ordinary calls inside destructors do not see the in-flight exception; a destructor may still catch locally, and a nested throw that escapes unwinding aborts.

**Tests**: 402/402 (100 pass + 286 must-fail + 16 check)

### 0.0.80 — 2026-11-12

**English comments + English docs**: translated all 874 Chinese comment lines across the 42 files in `src/` with one consistent terminology; added the English editions of the specification (1633 lines, section-for-section parity with the Chinese text) and of the version history (79 entries). String literals (user-visible diagnostics) are untouched, and the full suite verifies no behavioural change.

**English-only public tree (documentation follow-up, no version bump)**: removed the Chinese `README.md`, `docs/SPEC.md` and `docs/versions.md`; the English editions took over the plain names and are the only maintained copies from now on. Translated the remaining Chinese comments and prose in every other published file: 420 comment lines across 86 `.mu` files (`examples/`, `libs/std/` and `tests/`), plus `tests/README.md`, `tests/run_tests.py` (including its console output), `CMakeLists.txt`, `ci.yml`, `.gitignore`, `.vscode/settings.json` and `NOTICE`. `agents/` (internal, gitignored) keeps its Chinese notes, and the multilingual tests (`unicode_escape`, `char_nonascii`, `unicode_*`) deliberately keep their non-ASCII literals and expectations. Every `.mu` change was verified to be comment-only (code, string and char literals byte-identical).

**Stale statements corrected (same documentation follow-up)**: the README no longer lists generics and automatic `weak` nulling as unimplemented (both have shipped and are covered by tests), gains a generics row, narrows the SPDX claim to `src/`, and drops the reference to an internal cross-platform plan; the SPEC header no longer calls destructors a non-goal and states the version it is current through; the public changelog no longer points at an internal glossary; CMake now requires `LLVM ≥ 17`, matching the README.

**Generics showcase example (documentation follow-up, no version bump)**: added `examples/generics.mu`, a commented walkthrough of monomorphization — a generic `struct Vec<T>` with methods, constrained free functions (`max<T: IOrdered>`, `sum<T: IAddable>` taking a nested `Vec<T>`), and a user type (`Money`) that satisfies the constraints structurally with no `implements` clause. The test runner now auto-discovers every `examples/*.mu` that defines `main` (instead of hardcoding `examples/example.mu`) and compares it against `tests/expected/<name>.txt`, so the example is itself a regression case (`generics.txt`). No compiler changes.

**Tests**: 385/385 (88 pass + 281 must-fail + 16 check)

### 0.0.79 — 2026-11-12

**Version history switched to section format**: long entries were easily broken by pipes in a table (incomplete escaping shifted the columns), so the structure became `### version — date` + summary + `**Tests**`; the 78 records were re-sorted in descending version order (newest first). No code changes

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

### 0.0.78 — 2026-11-12

**Documentation cleanup wrap-up**: `versions.md` moved back into `docs/` (publicly released, readers need the changelog); fixed the unescaped pipe in the 0.0.57 entry (the diagnostic fragment `` `  |` `` used to break the table column, shifting the following columns). AGENTS and the README/README.en document tables synced. No code changes

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

### 0.0.77 — 2026-11-12

**Documentation boundary tightened**: `docs/` keeps only `SPEC.md` (the public specification); the code map, backend notes, object model plan, cross-platform plan, version history, visibility design and std.string layout all moved into `agents/` (gitignored). README and SPEC no longer link to unpublished files. No code changes

**Tests**: 384/384

### 0.0.76 — 2026-11-12

**Documentation directory cleanup (docs)**: `docs/` is designated the public documentation directory and `SPEC.md` moved to `docs/SPEC.md`; added `agents/` (**gitignored, not committed**) to hold local AI collaboration artifacts — `design-backlog.md`, `design-review.md`, `TODO.md`. README / README.en / AGENTS / SECURITY and the relative links between documents updated in sync (including the `examples/` and `visibility-design` paths inside SPEC), and the README version number refreshed to 0.0.75. `docs/design-review.md` is no longer publicly released. No code changes

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

### 0.0.75 — 2026-11-12

**Explicitly dropping "value type as interface value" (documentation decision)**: after evaluation, without borrow checking a value type → interface value is necessarily boxed, costing roughly a `new` of a class and losing the explicit lifetime; runtime polymorphism is covered by `class`, and heterogeneous value collections use sum types. Closed backlog U33 S3/S5 and U34 M5b; SPEC adds 3.15.8, while 3.15.7 and table 110 changed to "explicitly not doing" with the rationale and the condition "can be re-evaluated once borrow checking lands". No code changes

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

### 0.0.74 — 2026-11-12

**U33 S1+S2: `struct S : I` static satisfaction**: only interfaces may be listed after `:` (class/struct is an error); interface methods (including the inheritance chain and default methods) must be implemented explicitly with `override`, and a missing implementation or a missing `override` each have a dedicated diagnostic. `struct` methods support a leading `override` (previously only `class` did), and `MethodSig::is_override` is propagated faithfully. Static satisfaction produces no value type and no vtable, with zero codegen changes. Added the `struct_iface` end-to-end case + 2 error cases

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

### 0.0.73 — 2026-11-12

**SPEC generics chapter (docs)**: added **3.15 Generics** (monomorphization strategy, declaration/use syntax, instantiation rules, the constraint type set + member set model, `&`/`|` duality, structural satisfaction and exact matching, `Self`, current limitations); updated section 0 at a glance, 2.3 (`Self` contextual keyword), 3.10 (generic interfaces as constraints only), 7 (functions support generics), 10 (moved out of the non-goals), 12 tables 95/110/111, 14 implementation roadmap. Docs aligned with the implementation in 0.0.64–0.0.72

**Tests**: 381/381 (86 pass + 279 must-fail + 16 check)

### 0.0.72 — 2026-11-12

**Generic interfaces as constraints (M5a)**: `instantiate_generic` supports `InterfaceDecl` (`make_interface`, member signatures including `is_operator/ret_self/params_self`, `Self` resolution, default method body checking), and `T: ISeq<int>` takes effect via structural shape matching; interface instances produce no value type/itable. `interface ISeq<T>` + `sum_all<T: ISeq<int>>(T)` works end to end. Limitation: generic interface inheritance and "interface as value type" still pending M5b. Added the `generic_iface` case

**Tests**: 381/381 (86 pass + 279 must-fail + 16 check)

### 0.0.71 — 2026-11-12

**Generic inheritance (M3f)**: `class Derived<T> : Base<T>`. Added `base_specs` (`vector<TypeSpecPtr>`, parallel to the string list) next to `bases`, filled in by the parser at the same time and handled together on clone/replace; the class branch of `instantiate_generic` supports a single base class (after instantiation it sets `ty->base` and flattens fields), and constructor `super(...)`, method inheritance and vtable/desc all follow the ordinary class path. Generic interfaces and `const` members are still unsupported. Added the `generic_inherit` case

**Tests**: 380/380 (85 pass + 279 must-fail + 16 check)

### 0.0.70 — 2026-11-12

**Generics M3e: generic class instantiation**: `instantiate_generic` uniformly supports struct/class (fields per `field_visibility`, method/constructor/destructor signatures, `is_abstract`, vtable/desc following the ordinary class path); `check_class` wired in (context save/restore). `class Box<T>` and `Counter<T>` (with a default constructor) work end to end. Limitation: generic classes do not yet support base classes, interfaces or `const` members. Added the `generic_class` case

**Tests**: 379/379 (84 pass + 279 must-fail + 16 check)

### 0.0.69 — 2026-11-12

**Generic constructor call syntax**: `Pair<int,long>(3, 9)` / `Vec2<int>(3, 4)`; the `type_args` branch of `check_call` looks up generic functions first, otherwise instantiates a generic struct value construction (`instantiate_generic_named`) and reuses the existing `struct_ty` construction check. Added `find_generic_template` for unified template lookup. `Pair<A,B>` and `Vec2<T>` work end to end. Added the `generic_ctor` case

**Tests**: 378/378 (83 pass + 279 must-fail + 16 check)

### 0.0.68 — 2026-11-12

**Generics M3d: methods/constructors/destructors of generic structs**: lifted the "fields only" restriction; instantiation resolves method/constructor signatures, registers `ty->methods`/`ty->ctors`, generates symbols (`method_cname`/`ctor_cname`/`dtor_cname` run `ident_safe` on the owner, `Vec<int>` → `Vec_int_`), and runs `check_struct` to check method bodies (with checking-context save/restore, supporting nested instantiation). `Vec<T>`'s `length()/at()/fill()` and `Wrap<T>::twice()` work end to end. Limitations: generic structs still do not support `const` members or interface bases; the constructor call syntax `Pair<int,long>(...)` comes later. Added the `generic_methods` case

**Tests**: 377/377 (82 pass + 279 must-fail + 16 check)

### 0.0.67 — 2026-11-12

**Generics M3a+M3b: interface operator members + structural constraints**: interfaces allow `T operator+(T a, T b);` (`MethodDecl::is_operator`); `Self` is no longer rewritten to the interface name but marked via `TypeSpec::is_self` + `MethodSig::ret_self/params_self`, which sema substitutes correctly when resolving interface signatures. **Constraint checking became structural**: `T: I` matches by member shape, methods look up the method table (including the base class chain / interface defaults), and operators look up built-in or visible-unit `operator`s (exact signature, no implicit conversion); `&` requires all, `|` requires any. Built-in types satisfy at zero cost (`int: IAddable` thanks to the built-in `+`). Failure diagnostics name the missing items. Added the `generic_bounds` end-to-end and `generic_bound_missing` error cases

**Tests**: 376/376 (81 pass + 279 must-fail + 16 check)

### 0.0.66 — 2026-11-12

**Generics M2d: free function monomorphization**: call site `f<int>(x)` (`Name<` tentative parse in `parse_postfix_tail`, falling back to a comparison expression on failure); `Call::type_args`; `instantiate_func` (instance cache, signature resolution, checking-context save/restore to support nested calls, `func_cname` symbol sanitization); `mangle.cpp` applies `ident_safe` to instance names (`Box<int>` → `Box_int_`). `T max<T>(T,T)` and `T twice<T>(T)` work end to end. Added the `generic_func` end-to-end case and the `generic_arity` error case

**Tests**: 374/374 (80 pass + 278 must-fail + 16 check)

### 0.0.65 — 2026-11-12

**Generics M2: struct monomorphization end to end**: AST deep clone (`src/ast_clone.cpp`) + type parameter substitution (`src/ast_subst.cpp`) + instantiator (`src/sema_generic.cpp`, lazy instantiation + cache + recursion guard). `struct Box<T> { T v; }`, `Pair<K,V>` and `Box<Box<int>>` all instantiate and produce an executable; the output is an ordinary struct, so `same_type` / mangle / layout / codegen are unchanged. Current limitations: generic structs support fields only (methods/construction/interfaces/`align(N)` pending M3); free function generics pending M2d. Added the `generic_struct` end-to-end case and removed the M1 "not implemented" error case

**Tests**: 372/372 (79 pass + 277 must-fail + 16 check)

### 0.0.64 — 2026-11-12

**Generics (M1: syntax layer)**: `<T>` type parameter declarations (on free functions / structs / classes / interfaces); the type argument `Vec<int>` and the `>>` splitting of `Vec<Vec<int>>`; constraints `T: I` / `T: A & B` / `T: int | long` (mixing them errors, suggesting separate parameters); `Self` rewritten to an interface-name placeholder inside interface bodies; generic base classes `: ISeq<T>` as a placeholder; `struct S : I` syntax in place (backlog U33). Templates are moved into `Unit::templates` during the sema collection phase, neither checked nor emitted; use sites report "instantiation not implemented". The instantiation architecture (clone the AST + rename, zero codegen changes) and all decisions are in backlog U34

**Tests**: 372/372 (78 pass + 278 must-fail + 16 check)

### 0.0.63 — 2026-11-11

**Borrowed returns + borrowed receivers (ref semantics completed)**: ①**`ref T` return type**: `ref T f()` / `ref const T f()` (free functions, struct/class methods, interface methods, operators); the borrow source must be a `ref` parameter, `this`, a global/static, or a path through pointer/slice heap storage — borrowing a local, borrowing a by-value parameter (including inline fields) and borrowing a temporary are all compile errors; the result cannot be stored (`ref T r = f();` is an error) and can only be written through in place (`f() = v`, `f() += 1`, `f().field = v`), passed as a `ref` argument, or bound by `for ref`; a `ref const` location cannot be returned as a mutable `ref`. ②**`ref` parameters relaxed to scalars**: primitive types/enums/pointers can be borrowed too (`void inc(ref int x)`), and assigning to a scalar borrow writes through; struct borrows still require writing fields under "no rebinding". ③**Borrowed receivers**: operator parameters support `ref` (`ref int operator[](ref C c, usize i)`), fixing "the value receiver writes into the copy and silently drops the write"; method borrowed returns use the implicit `this` (`ref int at(usize i) { return v[i]; }`). ④**`[]` returns a borrow**: `c[i]` is directly a writable lvalue (`=`/`+=`/taking a value/passing `ref`/`for ref` all work), so `[]=` is no longer required. ⑤**`for ref` supports user types**: the index form requires `[]` to return `ref T`. ⑥Symbol names encode the parameter borrow bit (`f(T)` vs `f(ref T)` no longer collide); `ref` in variable/field/element position gives a "binding mode is not a type" diagnostic. New cases: ref_return (1 pass), ref_return_local / ref_return_param_copy / ref_return_const / ref_return_assign_const / ref_return_extern / ref_return_void / ref_var_decl / ref_loop_const / ref_const_to_mutable (9 errors); ref_param_class / forin_ref expected text updated

**Tests**: 370/370 (78 pass + 277 must-fail + 15 check); cold build with zero warnings

### 0.0.62 — 2026-11-11

**Language feature batch (all TODOs executed)**: ①**Type aliases** `type A = B;` (contextual keyword, parsed only when a declaration starts with `type Ident =`; one alias per declaration; transparent aliases, supporting alias-of-alias/forward references/cycle detection; unit-level + `public` across units; `TypeAliasDecl` + `UnitInfo::aliases` + lazy resolution). ②**`ref` parameters** (free functions only): `ref T p` mutable borrow, `ref const T p` read-only borrow; borrows pure value structs only (no RC fields/no destructor/not a union); the argument must be an addressable lvalue, temporaries are not accepted, and a `const` argument cannot bind a mutable borrow; no rebinding/storing/returning; on the C side a pointer is passed (the parameter slot holds a pointer, the call site passes the address); methods/constructors/`extern` explicitly error. ③**range extensions**: `..=` with an inclusive upper bound, `for i in a..b step n` (a non-zero compile-time constant; a negative step counts down; `a..b` does not reverse automatically); `i` in the index form becomes the iteration count; switch branch ranges support `..=` and `step` errors. ④**Brace range fill**: `int[10] a = {1, 2, 5..9, 10};` (endpoints must be compile-time constants, fixed-size integer arrays only, participating in zero-fill/over-length checks after expansion). ⑤**Bitfields explicitly rejected**: `int x : 3;` in struct/union/packed gives a dedicated diagnostic; U16 closed. ⑥**Docs**: `switch` extensions decided against (6.6), `ref` is a binding mode so no `T&` is introduced, `string` classified as library content (new `docs/lib-std-string.md`, SPEC keeps only the language-visible contract), class mechanics wrapped up (3.8 heading, tables 74/87 unified with 94, U9/U10/U23 closed). New cases: type_alias (1 pass + 4 errors + 1 check), ref_param (1 pass + 8 errors), range_ext / switch_incl (2 pass), bitfield (3 errors), init_range (4 errors)

**Tests**: 360/360 (77 pass + 268 must-fail + 15 check); cold build with zero warnings

### 0.0.61 — 2026-11-11

**`const` semantics tightened (U7 closed)**: explicitly **not doing** `const` class views and `const` methods; `const` keeps only two meanings — a **read-only binding marker** on named types (locals / parameters: cannot rebind, cannot write fields directly through it; a class handle copied out is still writable, and mutating methods can still be called through it), and **pointee read-only** for pointers / slices. **Fixed `const` being silently dropped at parameter positions** (previously inside `void f(const Cat c)` rebinding and direct field writes were allowed; now consistent with a local `const`), and parameter signatures do not contain `const`, so overloading is unaffected. SPEC 3.7 / 3.8 / 3.11 / table 98 and backlog U7 synced; added 4 error cases + 1 check case

**Tests**: 333/333 (73 pass + 246 must-fail + 14 check)

### 0.0.60 — 2026-11-11

**Source split (purely physical moves, no logic changes)**: every file ≤1000 lines. `parser.cpp`(1896) → `parser_impl.hpp` + `parser_unit/stmt/expr.cpp`; `codegen_llvm.cpp`(3912) → `codegen_impl.hpp` + `codegen_support/runtime/expr/expr2/stmt/class/func.cpp`; `sema.cpp`(4953) → `sema_impl.hpp` + `sema_decl/types/class1/class2/const/expr/call.cpp`. Approach: internal class declarations go into `*_impl.hpp` (`namespace muc_detail`), member functions are defined per TU by feature, and the public interfaces (`analyze` / `llvm_emit_*` / `parse_*`) and `main.cpp` stay completely unchanged; added `docs/code-map.md` (concept→file:function index) and file-header indexes; `AGENTS.md` gains a "code structure" convention (≤1000 lines/TU, helper function placement, default arguments only in declarations).

**Tests**: 328/328 (73 pass + 242 must-fail + 13 check); cold build with zero warnings

### 0.0.59 — 2026-11-11

**Type-level compile-time constants + constant expression evaluation (U28/U29 closed)**: `const T NAME = <const-expr>;` inside a `struct`/`class` body is a **type-level compile-time constant** — it must be initialized on the spot, must be a constant expression, and **occupies no instance storage** (not in the field table, does not affect `sizeof`/`alignof`/layout/ABI/construction/destruction/RC); it is accessed as `Type.NAME`, can be written bare inside methods of the declaring type, and is looked up along the base class chain; `obj.NAME` / `this.NAME` / assignment / taking the address / `ref` targets all error; visibility follows `public:`/`private:` (class defaults to private), duplicate names and conflicts with fields/methods/constructors error, and union/interface do not allow it; types are limited to scalars (bool/integer/float/usize/isize) and enums. Added a **compile-time constant evaluator** (lazy + memoized + cycle detection, supporting forward references): literals, unary `- + not !`, arithmetic/bit/shift/comparison/`and`/`or`, `as`, references to unit-level `const` / enum members / type-level constants, `sizeof(T)`/`alignof(T)`; division by zero and circular dependencies error; it also takes over unit-level `const` initialization (`const int B = A + 1;`, `const int C = sizeof(int) as int;`), while string/aggregate constants keep the old path. **Instance fields no longer accept `const`** (`const int x;` → ``const member `x` must be initialized``), fixing the previous half-working state where "`&s.x` was unexpectedly `const int*`, yet `s.x = 2` was legal"; no read-only fields and no `readonly` keyword are provided. Added the const_member success case and 23 error cases; **post-audit fixes**: unsigned `>>` in constant evaluation now matches runtime (arithmetic shift by bit width, `uint 0x80000000 >> 4` = `0xF8000000`), a `use`d same-named constant no longer silently takes the first (reports ambiguity), `char` literals ≥128 are accepted by bit pattern in constant contexts, the `evaluating` flag became RAII (exception paths no longer falsely report circular), constant evaluation depth is capped at 512, diagnostics no longer print qualified type names such as `const int`, and **non-const global initializers also go through the constant evaluator** (`int g = 1 + 2;` used to fail in the backend, now works); comparisons and floating-point arithmetic round by the `usual_arith` common type (consistent with runtime). Added the const_global_init success case and 6 error cases

**Tests**: 328/328 (73 pass + 242 must-fail + 13 check); cold build with zero warnings

### 0.0.58 — 2026-11-11

**`\u{...}` Unicode escapes + codepoint type decided**: character/string literals support `\u{` + 1–6 hex digits + `}` (the four-digit `\uXXXX` form is not supported), range `U+0000`–`U+10FFFF`, rejecting the surrogate range `U+D800`–`U+DFFF` and out-of-range values; inside a string it is UTF-8 encoded into 1–4 bytes (`\u{0}` errors just like `\0`), inside a character literal it is limited to `≤ U+FF` (beyond that suggests a string literal instead); the Unicode **codepoint type is fixed as `int`** (i32), with no `rune`/`unichar`/`char32` alias introduced; codepoint views such as `s.chars()` are left until the string class is implemented (backlog U3 deferred, strings stay byte-based: `s[i]`/`for c in s`/`len(s)` unchanged). Added the unicode_escape success case and 8 error cases (surrogate/out-of-range/non-hex/missing brace/empty hex/more than 6 digits/character beyond one byte/NUL)

**Tests**: 298/298 (71 pass + 214 must-fail + 13 check); cold build with zero warnings

### 0.0.57 — 2026-11-11

**Diagnostics and debugging experience**: ①diagnostics carry a source snippet and a caret (`  |` + line number + `^`; tabs expanded to 4 columns, CJK counted as 2 columns for display width); ②**multiple error reporting** — global/function/class/struct/interface checks of the same unit collect errors item by item and print them together (resetting scope, loop depth and auto inference state after an error), capped at 20 per run; ③`-g` / `--debug` generates **DWARF** (compile unit, function DISubprogram, statement-level line numbers, locals and struct members), so gdb can break, step and run `info locals`. Added the multi_errors (3 errors reported at once) and dbgvars cases

**Tests**: 289/289 (70 pass + 206 must-fail + 13 check); cold build with zero warnings

### 0.0.56 — 2026-11-11

**`main` with parameters**: supports `int main(int argc, char** argv)` (passed in by the C startup code, `argv` is null-terminated and `argv[argc] == null`); parameter types must be exactly `(int, char**)`, and 1 parameter / `char*` / a non-`int` return are all compile errors; `char**` rather than `string[]` (argv does not participate in RC). Added the main_args success case and the main_bad_argv error case, and updated the main_params expectation; SPEC 7 and §12 #15 synced

**Tests**: 287/287 (69 pass + 205 must-fail + 13 check); cold build with zero warnings

### 0.0.55 — 2026-11-11

**C variadic calls (`...` on `extern`) + `std.cstdio`**: `extern` declarations may carry `...` (must be last, with at least one named parameter), arguments follow C default promotions (`float→double`, integer types smaller than `int`→`int`, enum→underlying integer type), and `string` automatically passes the underlying `const char*`; arguments containing RC are compile errors; custom varargs are not supported inside Mulan. Codegen uses `FunctionType::isVarArg` + `promoteVararg()`, with the ABI handled by LLVM. Also: `extern` declarations are now implicitly `public` (previously a cross-unit call was diagnosed as not public); constant initializers support unary `+`/`-` (`const int EOF = -1;`); added `libs/std/cstdio.mu` (stdio function and constant bindings). Added the varargs / cstdio success cases and 5 error cases; SPEC adds 7.1, backlog U31

**Tests**: 285/285 (68 pass + 204 must-fail + 13 check); cold build with zero warnings

### 0.0.54 — 2026-11-10

**Unions `union`**: a POD value type, all members at offset 0, `sizeof = max(member)` rounded up to the largest member alignment; supports `packed` / `align(N)` (same rules as struct); members are limited to POD — `string`, dynamic arrays `T[]`, class/interface references, and structs with reference fields or a user destructor are forbidden; no methods/constructors/destructors/inheritance, members are always public; initialization supports only `{}` zero initialization and named `{ member = value }` (exactly one member), with no positional initialization and no `T(...)` construction; reading an inactive member reinterprets the bytes per C semantics. On the LLVM side it is represented as "alignment carrier + tail padding", and member access uniformly goes through offset 0. Added the union_basic success case and 11 error cases; SPEC adds 3.4.1, backlog U14 closed

**Tests**: 278/278 (66 pass + 199 must-fail + 13 check); cold build with zero warnings

### 0.0.53 — 2026-11-10

**Invalid character hints switched to backticks**: `quote_char()` output changed from `'x'` to `` `x` ``, matching the code-snippet style of the other diagnostics; backslash / backtick itself / control characters are printed as `` `\` ``, `` `\`` `` and `` `\x0b` `` respectively. Added the `invalid_char` error case (previously this diagnostic had no coverage)

**Tests**: 266/266 (65 pass + 188 must-fail + 13 check); cold build with zero warnings

### 0.0.52 — 2026-11-10

**All diagnostic messages and CLI output in English**: 787 string literals in `src/` (lexer/parser/sema/codegen/main/loader) changed to English, styled after clang/rustc — lowercase start, no trailing period, idiomatic `cannot`/`expected X but got Y`/`unknown`/`invalid`, with code identifiers still in backticks; `--help` rewritten into the three sections `Usage:` / `build options:` / `Environment:`; code comments stay in Chinese. The 187 expectation files `tests/errors/*.err` were synced to the corresponding English message fragments (mapped one-to-one by the original "Nth diagnostic"; success cases such as `strings` are unaffected)

**Tests**: 265/265 (65 pass + 187 must-fail + 13 check); cold build with zero warnings

### 0.0.51 — 2026-11-10

**Fixed garbled Chinese in the Windows console**: at startup, if the console output codepage is not UTF-8 (Simplified Chinese systems default to 936/GBK), `muc` temporarily switches to `CP_UTF8` and restores the original value on exit via `atexit`. The Chinese messages in the source have always been UTF-8 bytes; previously a 936 console interpreted them as GBK and displayed mojibake; when output is redirected to a file or pipe the bytes are unchanged (still UTF-8)

**Tests**: 265/265 (65 pass + 187 must-fail + 13 check); cold build with zero warnings

### 0.0.50 — 2026-11-10

**Compiler renamed `mulanc` → `muc`**: the executable target and the CMake `project()` name became `muc` (artifact `build/muc.exe`), with `--version` / `--help` output synced; environment variables `MULANC_CMD`/`MULANC_LIBS`/`MULANC_BACKEND`/`MULANC_LLVM_TRACE` became `MUC_CMD`/`MUC_LIBS`/`MUC_BACKEND`/`MUC_LLVM_TRACE`, and the compile macro `MULANC_VERSION` → `MUC_VERSION`; the test runner's default compiler became `muc`; command examples in README / SPEC / docs / AGENTS synced (historical entries in `versions.md` keep the old name). Also: added `LICENSE` (Apache-2.0) and `NOTICE`, named the language Mulan, and added SPDX headers to 17 source files; the `"mu-lan"` literal in examples and tests changed to `"Mulan"` (length/truncation expectations derived by the strings case updated to 5 / `Mu`)

**Tests**: 265/265 (65 pass + 187 must-fail + 13 check)

### 0.0.49 — 2026-11-10

**U24: forbid taking the address of structs with reference fields**: `&s` and `&arr[i]` (the element is a struct with references) are compile errors — a raw pointer could bypass per-field retain/release; scalar fields `&s.x` are still allowed; structs with a user destructor are exempt (they can only be used through pointers anyway). `void keep(S* out, ...)` in the `dynarray` case changed to return by value; added 3 address-taking error cases, and `addr_rules` covers the allowed forms (scalar fields of RC structs, address-of on destructor structs)

**Tests**: 265/265 (65 pass + 187 must-fail + 13 check)

### 0.0.48 — 2026-11-10

**for-in index form + user type iteration + U21 landed + lvalue fix**: ①`for i, x in seq` / `for i, ref x` (`i` is a usize, read-only inside the loop; for built-in sequences `i` is the real subscript, for ranges it is the offset relative to `lo`); a user type (struct/class) providing `length() -> usize` + `operator[](C, usize) -> T` (exact usize match) degrades to the index form, `length()` is evaluated only once, and elements are retained/released per round as owning locals; multidimensional arrays `T[m,n]`/`T[,]` explicitly disallow for-in (use an index loop + `len(a,k)` instead); `for ref` does not support user types; SPEC adds 6.4.1–6.4.4 and backlog U30. ②**U21**: forbid taking the address of class/interface references (`&c`, `&i`) and of members/elements accessed through a reference (`&c.x`, `&h.items[0]`, `&catArray[i]`, `&h.v[0]`), while pure value structs / fixed-size array elements / pointer variables are unaffected; layout2 now verifies class layout against a mirror struct model from the same source. ③Fixed `is_lvalue` misjudging local/global variables that share a name with a unit as non-lvalues (`unit t; S t = ...; t.x = 1;` reported "the left side of an assignment must be an assignable object"). Added the forin_index / unit_name_var / addr_rules success cases and 12 error cases

**Tests**: 262/262 (65 pass + 184 must-fail + 13 check)

### 0.0.47 — 2026-11-10

**Parameters become owning locals + `del` removed**: function/method/constructor/interface method parameters are retained on entry and released on exit (covering the return/break/continue paths), and parameters can be freely rebound inside the body; `for-in` value bindings are likewise treated as owning locals (retain/release each round, covering break/continue); this fixes the memory-safety defect where "assigning `c = null` / `c = new C()` to a parameter or loop variable released the caller's object early (use-after-free) or leaked"; the `del` statement is removed (`del x;` ≡ `x = null;`, no separate semantics), with the keyword kept only for migration diagnostics; migrated `del` in all tests/examples, removed the 6 del-specific error cases and class_del_null, added the del_removed error case and the param_ownership regression case; SPEC 3.8/4/8.3 and decision table #69/#70 synced

**Tests**: 247/247 (62 pass + 172 must-fail + 13 check)

### 0.0.46 — 2026-11-10

**Conditional expression `?:`**: `cond ? a : b`, right-associative, with lower precedence than `or`; the condition must be `bool`; only the selected branch is evaluated; the two sides converge by "same type → implicit widening → `null` adaptation", requiring `as` when none applies; the result is a prvalue (struct/string/fixed-size array retain per field, class/interface borrows from a branch get +1 while `new`/call results transfer ownership, dynamic arrays follow handle semantics), and the result slot is materialized in the entry block; the result is not an lvalue (`(c ? a : b) = x`, `&(c ? a : b)` are errors) and does not participate in compile-time constants; when both sides are `void` it can only be an expression statement; also fixed the backend `cast<IntegerType>` assertion crash on `double d = 1;` (an integer literal adapting to a floating-point type); added the ternary success case and 5 error cases; SPEC adds 5.2, #14/#104 decided

**Tests**: 252/252 (62 pass + 177 must-fail + 13 check)

### 0.0.45 — 2026-11-10

**`switch` (no fallthrough)**: `switch expr { pattern-list: { } }`, braces optional; branch bodies require braces and get their own scope; at most one `else`, which must come last; patterns are compile-time constants (integer literals / character literals / literals with unary `-` `+` / enum members), and `a, b` matches on any of several values; ranges `a..b` are half-open and must be non-empty; enums must be exhaustive (a missing member errors; `else` is mutually exclusive with it, and full coverage warns); enums do not support ranges or implicit integer conversion; duplicate/overlapping patterns (including overlapping ranges) are compile errors; the matched value is evaluated only once; `break`/`continue` still belong to the innermost loop; in codegen dense values go through LLVM `switch` (jump table) and ranges through comparison chains; added the switch_basic success case and 15 error cases; SPEC adds 6.6, and `match` drops its reserved word

**Tests**: 246/246 (61 pass + 172 must-fail + 13 check)

### 0.0.44 — 2026-11-10

**Explicit `this`**: `this` can be written explicitly inside class/struct methods, constructors and destructors; `this` is a reference to the current object (for structs an lvalue alias of the object itself, not a pointer), supporting `this.field` reads and writes, `this.method()`, passing as an argument and returning by value; `this` cannot be assigned, cannot have its address taken (`&this` errors) and cannot be passed to `del`; added the explicit_this success case and the this_addr / this_outside error cases, removed the struct_this error case

**Tests**: 230/230 (60 pass + 157 must-fail + 13 check)

### 0.0.43 — 2026-11-10

**Parameter declarations may not share a type (SPEC table 26 decided)**: every parameter must write `type name` separately, so `int f(int a, b)` is a compile error; the parser recognizes the C-style shared-type form early in `parse_params` (`Ident` directly followed by `,`/`)`) and gives a clear "declare a type for each parameter" diagnostic (previously it only reported a vague "expected parameter name" at `)`); a type suffix applies to a single parameter only; shared types for multiple names in variable/field declarations are unchanged (SPEC 4.1 gains a cross-reference); added the param_shared_type error case

**Tests**: 228/228 (59 pass + 156 must-fail + 13 check)

### 0.0.42 — 2026-11-10

**Layout modifiers `packed` / `align(N)`**: `packed` takes no argument (= `pack(1)`, no internal/tail padding, `alignof` 1), `align(N)` (a power-of-two constant, overriding overall alignment, `sizeof` rounded up to a multiple of N, field offsets unchanged, `N < natural alignment` errors), and the two are orthogonal and composable; layout has a single source of truth `type_align`/`type_size` (shared by sema and the backend); packed uses LLVM packed structs (the ordinary struct ABI is unchanged), `align(N)` inserts explicit padding and sets alignment on alloca/global/member access (packed chains always `align 1`); class objects switched to the same-source "model layout + padding" (object alignment = max(16, max field alignment), using `_aligned_malloc` when >16); slice allocation rejects elements with `align > 8` (the data area sits after the 24-byte header); added the layout / layout2 success cases and 9 error cases

**Tests**: 227/227 (59 pass + 155 must-fail + 13 check); cold build with zero warnings

### 0.0.41 — 2026-11-10

**Empty braces `{}` zero initialization**: aggregate types (struct / fixed-size array / slice / `string`) written as `{}` at the declaration mean the zero value, and `= 0` on an aggregate errors suggesting `{}`; scalars (numeric/bool/char/pointer/class reference) written as `{}` error, suggesting their own constants (`0`/`0.0`/`false`/`null`); in codegen empty aggregates use `zeroinitializer` / zeroing, and empty globals use zero constants; added the empty_brace success case and the brace_scalar / zero_aggregate error cases

**Tests**: 216/216 (57 pass + 146 must-fail + 13 check)

### 0.0.40 — 2026-11-10

**Qualified enum member references**: `Color.Red` is equivalent to `Red`, and cross-unit `unit.Color.Red` can be written; sema resolves the type name at the member access site (current unit → `use`d public → std.string), members are constants (cannot be assigned/taken address), and `T.m` on a non-enum type errors; codegen folds the node directly into a constant as `enum_const`; enum_cmp and vis_import_ok cover qualified references, and 4 error cases were added

**Tests**: 213/213 (56 pass + 144 must-fail + 13 check)

### 0.0.39 — 2026-11-10

**Enums become nominal types**: no implicit conversion to or from integers in either direction (both need `as`); the same enum supports `== != < <= > >=` (order = underlying integer value); comparing across enums, enum arithmetic/bitwise operations/unary `-+~`/compound assignment, and subscripts/range endpoints using enums all error; `usual_arith` rejects enums, and codegen emits `icmp` using signed int order for comparisons; added the enum_cmp end-to-end case and 8 error cases, migrated extras/enumfield/vis_import_ok

**Tests**: 209/209 (56 pass + 140 must-fail + 13 check)

### 0.0.38 — 2026-11-10

**Bitwise NOT changed to `~`**: consistent with C, and `operator~` overloading is supported (1 parameter); `!` is no longer used and gives a migration hint; migrated the bitwise case, added the op_neg end-to-end and bang_removed error cases

**Tests**: 200/200 (55 pass + 132 must-fail + 13 check)

### 0.0.37 — 2026-11-10

**++/-- uniformly error in expression contexts**: function arguments, if/while conditions, return, initializers, array subscripts, `new` arguments and parentheses all give a clear "no return value" hint; added the inc_call_expr / inc_return_expr error cases

**Tests**: 198/198 (54 pass + 131 must-fail + 13 check)

### 0.0.36 — 2026-11-10

**Forbid `*(p++)`**: `++`/`--` have no return value, so appearing inside parentheses also reports a clear error; `*p++` still parses as `(*p)++`; added the inc_paren_expr error case and covered `*p++` in inc

**Tests**: 196/196 (54 pass + 129 must-fail + 13 check)

### 0.0.35 — 2026-11-10

**++/-- statement support**: prefix/postfix `++`/`--` are only allowed as standalone statements (no return value, cannot appear in expressions); semantically lowered to `x += 1` / `x -= 1`; `i++; j++;` is legal; added the inc end-to-end and inc_expr error cases

**Tests**: 195/195 (54 pass + 128 must-fail + 13 check)

### 0.0.34 — 2026-11-10

**Explicitly no `T* const`**: `const` is only a base-type prefix (value/pointee read-only), and neither pointer-binding read-only nor per-level const is introduced; SPEC 3.7 and #25 changed to "not provided" with the rationale

**Tests**: 193/193 (53 pass + 127 must-fail + 13 check)

### 0.0.33 — 2026-11-10

**const changed to the C model**: removed `readonly`; `const` becomes a type qualifier (a prefix on the base type, `const int**` means the innermost level is read-only); `T* → const T*` is implicit while the reverse needs an explicit `as`; write checks and implicit conversions are decided per const level; migrated `readonly` → `const` across the whole repo; `T* const` is not supported for now

**Tests**: 193/193 (53 pass + 127 must-fail + 13 check)

### 0.0.32 — 2026-11-10

**Removed `static`**: unit-level variables are declared directly (private by default, `public` across units); `extern` is kept; global variable linkage is decided by public/private; using `static` at top level or inside functions gives a clear migration error; migrated all cases and examples, removed unit_var_bare

**Tests**: 193/193 (53 pass + 127 must-fail + 13 check)

### 0.0.31 — 2026-11-10

**string rewritten as an object**: `string` stays a value-type struct (canonical layout/literals/`s[i]`/`len`/`==`/for-in degradation unchanged) with private fields; added the constructor `string(readonly char*)` and the member methods `length/empty/is_empty/at/cstr/sub/concat/find/starts_with/ends_with/trim`; removed the `str_*` free functions, with `operator+` thinly wrapping `concat`; relaxed `= 0` to allow zero-value initialization of structs that have constructors; added the string_methods end-to-end case, removed struct_ctor_zero

**Tests**: 194/194 (53 pass + 128 must-fail + 13 check)

### 0.0.30 — 2026-11-10

**public/private replace pub**: private = inside the unit, public = across units; struct members default to public and class members to private; struct/class support `public:`/`private:` section labels and per-member prefixes; struct fields can be private, and cross-unit aggregate initialization requires all fields to be public; migrated the whole repo and added 4 cases

**Tests**: 194/194 (52 pass + 129 must-fail + 13 check)

### 0.0.29 — 2026-11-10

**C#-style import visibility**: after `use`, `pub` symbols can be used without a prefix (types/functions/global variables/enum constants/struct construction); local declarations shadow imports; the same name from multiple sources reports ambiguity at the use site and requires a qualified name; function overloads merge across units; the parser tentatively supports unqualified type names by declaration shape; added 6 import cases

**Tests**: 191/191 (52 pass + 128 must-fail + 11 check)

### 0.0.28 — 2026-11-10

**Multi-segment unit name qualification**: the expression `lib.a.b.f()` is folded into a single Name by the longest imported unit prefix; the type `lib.a.b.T` is matched by the longest prefix in sema; the parser's type parsing supports dots at any segment; `std.string` is treated as implicitly `use`d; added 2 check cases

**Tests**: 186/186 (52 pass + 125 must-fail + 9 check)

### 0.0.27 — 2026-11-10

**Visibility mechanism**: added the keyword `pub`; top-level declarations / class fields and methods / constructors are visible only inside the unit by default, and `pub` makes them cross-unit; `pub unit` exports across libraries; the library boundary = the first segment of the unit name (a cross-library `use` needs `pub unit`); `pub` signature propagation checks; non-`pub` symbols get LLVM `internal` linkage; migrated std.string/math/lib_* and added 11 visibility cases

**Tests**: 184/184 (52 pass + 125 must-fail + 7 check)

### 0.0.26 — 2026-11-09

**Standard library directory and type rename**: added the `libs` default library search path (entry directory, `-I`, `MULANC_LIBS`, `libs`/`libs/std` in the current directory and its ancestors); `libs/std/string.mu` (unit `std.string`) replaces the compiler-embedded prelude; `unit`/`use` support dotted names (`std.string` → `libs/std/string.mu`); the string struct renamed from `Str` to `string`; tests/examples/docs synced

**Tests**: 172/172 (52 pass + 115 must-fail + 5 check)

### 0.0.25 — 2026-11-09

**String changed to the std.string.string struct, built-in `string` removed**: the struct `string { byte* p; usize n; byte[] owner; }` in `libs/std/string.mu` (unit `std.string`) + the string library; the literal type is string (owner=null, rodata, zero allocation/zero RC); `s[i]`/`len`/`==`/for-in degrade using the built-in canonical layout; copies/fields/returns go through U24 per-field RC; std.string provides `str_from_cstr`/`str_cstr`/`str_sub`/`str_concat`/`operator+` and methods; removed the `string` keyword from lexer/parser/sema and migrated all tests and examples; added the `str_struct` case; runs the standard LLVM `-O2` pipeline after generating IR

**Tests**: 172/172 (52 pass + 115 must-fail + 5 check)

### 0.0.24 — 2026-11-09

**Dynamic arrays `T[]` + multidimensional arrays**: `new T[n]` creates a heap array with an RC header (an 8-byte handle, O(1) copy sharing); static rectangles `T[m,n]` (nested fixed-size arrays, `a[i,j]`/`a[i][j]`), dynamic rectangles `T[,]` + `new T[m,n]`, and jagged `T[][]` + `new T[m][]` (one dimension per `new`, rows filled in a loop); `len(a)`/`len(a,k)` (literal axes for static, runtime-capable for dynamic); `T[N]→T[]`, `T[m,n]→T[,]` allocating copies; elements may be class/interface and structs with references, released per element; a null handle has len=0; `extern` recursively rejects `T[]`; structs with destructors are still forbidden as elements; removed the class/interface slice prohibition and the old borrowed-view semantics

**Tests**: 171/171 (51 pass + 115 must-fail + 5 check)

### 0.0.23 — 2026-11-09

**U24: struct reference fields + per-field RC**: structs may contain class/interface fields (including arrays/nesting); copies/passing retain, assignment retains the new value before releasing the old, and destruction releases per field/per element; a returned value turns a borrow into owned; a temporary struct created by a call is released at the end of the statement; uninitialized local structs are zeroed automatically; extern/globals recursively rejected; structs with a user destructor still cannot be copied; added the `struct_rc_fields` success case + 2 error cases, removed the outdated "structs cannot contain class fields" case

**Tests**: 163/163 (49 pass + 109 must-fail + 5 check)

### 0.0.22 — 2026-11-09

**struct construction/destruction**: constructor overloading, `S s;` default construction, `S s = S(args)` in-place construction; destructors are called automatically on scope exit/return/break/continue; structs with destructors are temporarily forbidden to be copied/passed/returned by value/in arrays/fields/globals; added 1 success case + 11 error cases

**Tests**: 161/161 (48 pass + 108 must-fail + 5 check)

### 0.0.21 — 2026-11-09

**struct methods**: static dispatch, implicit `this`, method overloading, array/field receivers, implicit method calls; methods may not share a field's name; explicit `this` not supported yet; added 1 success case + 4 error cases

**Tests**: 149/149 (47 pass + 97 must-fail + 5 check)

### 0.0.20 — 2026-11-09

Converged 8-bit types per the final decision: `char`=i8 (C style), `byte`=u8 and the only unsigned 8-bit name (**removed the `ubyte`/`uchar` aliases**); `s[i]`/string for-in return `byte`; character literals default to `byte` and adapt to `char`/`byte` by context; added an alias-removal case

**Tests**: 144/144 (46 pass + 93 must-fail + 5 check)

### 0.0.19 — 2026-11-09

**`char` changed to signed 8-bit (C style); `byte` changed to unsigned 8-bit, with `ubyte`/`uchar` as its aliases (the same type)**; `'\xNN'` is interpreted by bit pattern; added char/byte range error cases

**Tests**: 143/143 (46 pass + 92 must-fail + 5 check)

### 0.0.18 — 2026-11-09

Clarified character literal semantics: `char` is an unsigned 8-bit single byte and non-ASCII literals are compile errors; SPEC gains item 101; added the `char_bytes` (UTF-8 byte view) and `char_nonascii` cases

**Tests**: 141/141 (46 pass + 90 must-fail + 5 check)

### 0.0.17 — 2026-11-09

Check that foreign C calls cannot pass class/interface; added a case of an interface returning an interface and a comprehensive case (interface arrays/for-in/downcast/weak references)

**Tests**: 139/139 (45 pass + 89 must-fail + 5 check)

### 0.0.16 — 2026-11-09

Added regression cases for single evaluation of comparisons/subscripts and temporary release (verifying the double-evaluation fix in 0.0.14)

**Tests**: 136/136 (43 pass + 88 must-fail + 5 check)

### 0.0.15 — 2026-11-09

SPEC formally downgraded: `readonly` class views, `ref` parameters and interface→derived-interface downcasts are listed as unsupported in phase one (tables 98–100); added checks that `extern` cannot be overloaded/cannot share a name with an ordinary function, and that `main` cannot be overloaded

**Tests**: 135/135 (42 pass + 88 must-fail + 5 check)

### 0.0.14 — 2026-11-09

**weak targets auto-null on destruction**: a global weak registry (target, field address) + spinlock; assignment registers, overwriting/destruction of the holder unregisters, and the target's destroy zeroes them; also fixed repeated evaluation in binary comparisons (a function call executed twice inside a comparison, leaking temporary references)

**Tests**: 132/132 (42 pass + 85 must-fail + 5 check)

### 0.0.13 — 2026-11-09

Cleared the last warning in a cold build (unused variable); cold build has zero warnings

**Tests**: 131/131 (41 pass + 85 must-fail + 5 check)

### 0.0.12 — 2026-11-09

`super.method` resolves only along the class chain (calling an interface default method through super errors); `del null` is a no-op; multi-unit interface/inheritance check cases (lib_ dependencies skipped); error case for compound assignment to a subscript

**Tests**: 131/131 (41 pass + 85 must-fail + 5 check)

### 0.0.11 — 2026-11-09

Class field resolution changed to a topological order based on base class dependencies (a base class may be declared later in the same unit and field layout is still correct); added forward base class and interface array cases

**Tests**: 127/127 (40 pass + 83 must-fail + 4 check)

### 0.0.10 — 2026-11-09

Per-element RC for arrays of references: copies retain, overwrites/scope exit release, and by-value returns and temporary arguments transfer per element (fixing early release/leaks of shared arrays); added 2 end-to-end cases

**Tests**: 125/125 (38 pass + 83 must-fail + 4 check)

### 0.0.9 — 2026-11-09

Fixed a leak where an operator/subscript overload returning a reference was treated as a borrow; override return types are compared against all base class/interface declarations (conflicts error); added 2 cases

**Tests**: 123/123 (36 pass + 83 must-fail + 4 check)

### 0.0.8 — 2026-11-09

Tightened `as`: pointers/slices no longer convert to each other, and slice conversion requires identical element types; added 2 error cases

**Tests**: 121/121 (35 pass + 82 must-fail + 4 check)

### 0.0.7 — 2026-11-09

When a constructor omits `super(...)`, the base class no-arg constructor is called implicitly (C++ semantics); added a linked list case (strong next + weak prev) and cases for nested `new`/base class arrays

**Tests**: 119/119 (35 pass + 80 must-fail + 4 check)

### 0.0.6 — 2026-11-09

`as` downcast: class→derived class and interface→class are checked at runtime along the desc base class chain, yielding null on failure; desc gains base_desc; added the downcast end-to-end case

**Tests**: 116/116 (32 pass + 80 must-fail + 4 check)

### 0.0.5 — 2026-11-09

`new`/`len`/`sizeof`/unary expressions can be followed by postfixes (`new C().f()`, `len(s) as int`); `sizeof` supports class/interface/enum; class pointers, `del` on operator subscripts and `return` inside destructors are forbidden; identity comparison between related reference types; added 4 end-to-end cases and 3 error cases

**Tests**: 115/115 (31 pass + 80 must-fail + 4 check)

### 0.0.4 — 2026-11-09

Local reference cleanup (block/return/break/continue), temporary reference release, interface default methods dispatched through class variables, interface array layout fixed (16 bytes), `del` restrictions on parameters/loop variables; added 4 end-to-end cases

**Tests**: 107/107 (26 pass + 77 must-fail + 4 check)

### 0.0.3 — 2026-11-09

Object model end to end: semantics and LLVM codegen for class/interfaces/inheritance/construction and destruction/virtual dispatch/reference counting/`weak`; overloads and operator dispatch; added 4 end-to-end class cases, 21 error cases and 4 check cases

**Tests**: 103/103 (24 pass + 75 must-fail + 4 check)

### 0.0.2 — 2026-11-08

Removed the C99 backend, making LLVM the only backend; cleaned up `--emit-c`/`-DMULANC_WITH_LLVM`; version plumbing wired into `--version` and the LLVM module flags

**Tests**: LLVM backend 58/58; cold build with zero warnings

### 0.0.1 — 2026-11-08

Baseline: C++20 compiler (lexer/parser/sema), C99 backend, LLVM backend (LLVM C++ API → COFF), 58 regression cases, reference/object model design discussion

**Tests**: C backend 58/58; LLVM backend 58/58; cold build with zero warnings
