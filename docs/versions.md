# Version history

## Rules

- The `VERSION` file at the repository root is the single source of truth and records **two independent
  numbers** on labelled lines:

  ```
  version = 0.0.0
  build   = 145
  ```

- **`version`** (`major.minor.patch`) is the release version. It changes **only on an explicit release
  request**; a release is recorded as a `###` section below with its date.
- **`build`** is a monotonic build counter, **+1 for every change to the compiler code** (`src/`, compiler
  behaviour, generated artifacts). Documentation-only, comment-only or config/CI-text changes are recorded
  inside the current build entry's follow-up paragraph instead of bumping it.
- A build number is only raised after that build passes the full test suite.
- Everything reads the same file: CMake (`MUC_VERSION` / `MUC_BUILD`), `muc --version`
  (`muc 0.0.0 (build 145)`), the DWARF `producer` string, and the `mulan.version` / `mulan.build`
  module flags.
- **Historical note**: builds up to `0.0.144` used a single `major.minor.build` number and keep their
  original headings under *Legacy numbering*. From build 145 on, entries are `#### build <n> — <date>`
  under the current release section.

## Releases

### 0.0.0 (unreleased)

#### build 179 — 2026-11-17

**Ambiguity diagnostics for cross-unit `#if` constants.**

- A bare constant name declared by two `use`d units is recorded as ambiguous; `#if` reports it and asks for
  a qualified reference (`unit.name`) instead of silently taking the first.
- Tests: `if_cross_unit_ambiguous` (must-fail) with `tests/errors/iflib/{lib,other}.mu`.

Full suite: 707/707 (262 run, 407 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 178 — 2026-11-17

**Cross-unit `#if` compile-time constants (first P3 step).**

- The loader pre-scans a unit's `use` list, finishes its dependencies first, then parses the unit; a
  dependency's public compile-time constants are registered and merged into the current unit's `#if`
  table when the `use` is parsed (bare names and `unit.name` forms).
- A `use` inside a dead `#if` arm is ignored by the pre-scan; a missing dependency is reported only if the
  live parse reaches it. Dependencies are parsed before the unit, so post-order initialization stays
  correct.
- Tests: `if_cross_unit` (run) with `tests/cases/iflib/lib.mu` providing `public const K` / `ENABLED`.
- Remaining P3 work: full constant evaluation inside the pass (`sizeof`/layout), ambiguity diagnostics for
  same-name constants from several units, and removing the parser-side splice.

Full suite: 706/706 (262 run, 406 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 177 — 2026-11-17

**`std.comptime` and compiler-provided compile-time functions.**

- `comptime` moved to `libs/std/comptime.mu` as `public unit std.comptime`; every `use comptime;` is now
  `use std.comptime;`, and the parser fills the constant table on that name.
- `builtin` compile-time functions: `is_windows()` / `is_linux()` / `is_macos()` are declared in
  `std.comptime`; a call folds to a `bool` constant in `#if` conditions and in ordinary code (no symbol).
- `const bool` vs `bool` comparisons use the signature comparison (top-level const ignored), fixing
  `IS_WIN != is_windows()`.
- Tests: `if_comptime_func` (run).

Full suite: 705/705 (261 run, 406 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 176 — 2026-11-17

**`#if` build constants require `use comptime;` (implicit visibility removed).**

- The directive evaluator has no built-in constant fallback: `#if os == OS.Windows` resolves `os` and
  `OS.Windows` from the visible constant table, which `use comptime;` fills. Without the use they are not
  visible (the diagnostic says so).
- The enum identity is kept in the constant table, so `os == Endian.Little` is still rejected as a
  cross-enum comparison.
- Existing `if_*` tests now write `use comptime;`; `if_target` uses the `comptime` unit instead of
  `std.target`.
- `std.target` is kept for compatibility (no test or library uses it now); it will be removed when the
  library migrates.

Full suite: 704/704 (260 run, 406 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 175 — 2026-11-17

**`builtin` declarations and the `comptime` unit.**

- `builtin const T name;` declares a compiler-provided compile-time constant (no initializer, no storage,
  no address); `builtin T f(...);` declares a compiler-provided compile-time function (no body). The
  compiler binds constants from its built-in table and reports unknown names.
- `libs/comptime.mu` declares `OS` / `Arch` / `Abi` / `Endian` and the eight build constants as
  `builtin const`; `use comptime;` exposes them as ordinary names, and `comptime.os` resolves across units
  by folding the value.
- Builtin references fold in codegen; builtin globals/functions are not emitted. `&comptime.os` is
  rejected (not addressable). Builtin function calls are rejected until a compile-time call model lands.
- Runtime platform symbols stay outside `comptime` (library/platform unit).
- Tests: `if_comptime` covers the unit; manual probes for builtin declarations and cross-unit folding.

Full suite: 704/704 (260 run, 406 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 174 — 2026-11-17

**`use comptime;` exposes the compiler-provided compile-time constants to `#if`.**

- `use comptime;` records `os` / `arch` / `abi` / `ptr_bits` / `endian` / `debug` / `bounds` /
  `single_thread` and the `OS` / `Arch` / `Abi` / `Endian` members as ordinary compile-time constants; a
  `#if` after the use resolves them like any other name (no special build-constant source). The constants
  are compile-time values: no storage, no address.
- `comptime` is supplied by the compiler: the loader and sema skip its file, and the parser injects the
  values on `use comptime;`.
- Runtime platform symbols/functions are not part of `comptime`; they belong to the library/platform unit.
- Tests: `if_comptime` (run). The pre-existing implicit visibility of the build constants is kept for
  compatibility until the remaining `if_*` tests and `std.target` are migrated (next step).

Full suite: 704/704 (260 run, 406 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 173 — 2026-11-17

**`for ref const` read-only iteration.**

- `for ref const x in seq` binds a read-only borrow: it can read the element but not write it, and it is
  the only `for ref` form accepted on a `const` sequence. A plain `for ref` on a const slice/binding now
  suggests `for ref const`.
- Works on slices and on user types whose `operator[]` returns `ref T` or `ref const T`.
- Tests: `for_ref_const` (run), `for_ref_const_write` (must-fail).

Full suite: 703/703 (259 run, 406 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 172 — 2026-11-17

**`#if` user constants inside type bodies and function bodies.**

- The same `user_consts_` table is visible in every `#if` position, so a constant declared earlier in the
  unit works inside a struct/class body and a function body, and `as` is supported in the initializer.
- Cross-unit constants are **not** available in the parse-time model: the loader parses the current unit
  before visiting its `use` dependencies (`loader.cpp`), so a dependency's constants are not evaluated yet.
  Cross-unit `#if` constants require the full Pass B (P3 in `agents/directive-pass-design.md`).
- Tests: `if_user_const_scope` (run).

Full suite: 701/701 (258 run, 405 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 171 — 2026-11-17

**`#if` can read unit-level compile-time constants (first step of the directive-pass plan).**

- A unit-level `const` whose initializer folds in the directive evaluator is recorded while parsing; a later
  `#if` in the same unit may use it (`const bool DEBUG = true; #if DEBUG { ... }`,
  `const int LEVEL = 2; #if LEVEL == 2 { ... }`). A constant declared later is not visible (the same rule as
  constant initializers).
- The directive evaluator now guards `MIN / -1` and `MIN % -1`; previously recording such a constant executed
  the UB in the parser and crashed `muc`.
- Tests: `if_user_const` (run), `if_user_const_later` (must-fail).
- Pending: cross-unit constants (`use` + prior declaration), user constants inside type-body/function-body
  conditions, the full Pass B (single DFS post-order), `comptime`/`builtin`, `for ref const`.

Full suite: 700/700 (257 run, 405 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 170 — 2026-11-17

**Const-field initialization enforcement and runtime-initialized unit-level `const`.**

- A constructor must initialize every `const` field in its member initializer list; a missing entry is a
  compile error, and writing a `const` field in a constructor body is rejected (use the initializer list).
- A unit-level `const` may be initialized at run time (9.1): `const string S = "hi";` and
  `const int x = f();` are accepted and initialized by the unit init sequence. They are `const` objects
  but not compile-time constants (3.7.2); compile-time folding still applies when possible. A `builtin`
  constant (9.3, not implemented yet) is exempt from the initializer rule.
- Runtime-initialized `const` globals participate in the 9.1 sequence (declaration order, cleanup
  reverse); codegen does not mark their storage as an LLVM constant, and the unit hooks no longer skip them.
- Tests: `const_field_uninit`, `const_global_runtime`, `const_global_runtime_write`; the obsolete
  `string_global` / `unit_init_const_nonconst` must-fail cases are removed.
- Still pending: the `#if` evaluation pass, `comptime`/`builtin`, and `for ref const`.

Full suite: 698/698 (256 run, 404 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 169 — 2026-11-17

**`const` fields, `const` methods and `const struct` / `const class`.**

- `const T name;` in a struct/class body is an instance field (not a type-level constant). It is initialized
  by a constructor member initializer list (`C(...) : x(v), y(w) { }`) and is read-only everywhere except
  inside a constructor body. Type-level constants are gone; a unit-level `const` replaces them.
- `T f() const` declares a read-only method: `this` is a const view, fields cannot be written, `f()` and
  `f() const` are distinct overloads, and the const is part of the signature (override/interface matching)
  and of the symbol name. A const object can only call const methods; a mutable object prefers the
  non-const overload.
- `const struct S` / `const class C` makes every field and method implicitly const; the type name carries
  no cv, so copy/move and whole-variable reassignment work. `const` cannot modify a union/enum/interface.
- `move` on a const value is rejected (`... copy it instead`).
- The standard library's read-only `String` methods are marked `const`; the old `const_member_*` tests are
  replaced by `const_field` / `const_method` / `const_struct` cases (and `move_const`,
  `const_field_assign`, `const_method_*`, `const_struct_field_write`).
- Still pending: unit-level dynamic initialization, `#if` evaluation changes, `comptime`/`builtin`, a
  general `move`/`&&` parameter form, and the `T** → const T**` diagnostic.

Full suite: 697/697 (255 run, 404 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 168 — 2026-11-17

**`const` enters the type system (first step of the C++ `const` redesign).**

- Type nodes carry cv: `const T` and `T` are different types. The top-level const is `is_const`; a pointee
  const lives on the element node (`const T*` is `Ptr(elem = const T)`, `T* const` is a const pointer).
  `same_type` is cv-sensitive; `same_type_sig` ignores the top-level const for signatures (overloads,
  duplicate definitions, interface matching), as in C++.
- Conversions: a top-level const is dropped on copy/assignment; a pointee const may only be added
  (`T* → const T*`), while `T** → const T**` still requires `as`. A `const S` value is copied out as a
  mutable `S`.
- The parser keeps `const` in the type specification for locals, parameters and globals (it used to strip
  it into a binding flag); `T* const` is parsed. Address-of now yields `const T*` for a const object.
- Symbol mangling encodes the low-level const (`f(int*)` and `f(const int*)` are distinct); the top-level
  const is dropped from signatures.
- Value-compatibility checks (initialization, arguments, `swap`, enum comparison, slice conversion) use the
  signature comparison; `readonly_drop` pins the new diagnostic wording.
- Still pending from the redesign: `const` fields and member initializer lists, `const` methods,
  `const struct`/`const class`, `move` vs `const`, unit-level constants, `#if` evaluation and the
  `comptime` unit.

Full suite: 716/716 (253 run, 425 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

#### build 167 — 2026-11-17

**Assignment fast path for the identical handle.**

- `b = a` when both already hold the same object is a no-op for the reference count: the lowering now
  compares the handles and keeps the retain/release pair on the different-pointer arm only (a class
  handle compares its pointer, an interface value its `{obj, desc}` object field). A loop that
  re-assigns the same value stops paying two atomics per iteration, and the assignment loop becomes
  optimizable at all.
- Regression: `tests/cases/assign_same_rc.mu` (a different source still releases the old value, the
  same source keeps the count, interface assignment works), `tests/ir/assign_same.(mu|txt)` (the
  comparison and the two arms are in the emitted IR). Also `tests/errors/reserved_ident.mu` pins the
  `where` / `typeof` reserved-word rejection that SPEC 2.5 already required.

Full suite: 716/716 (253 run, 425 must-fail, 24 check, 1 warning, 10 ir, 3 c abi).

Documentation (no build bump): SPEC 3.7/3.7.1/3.7.2 rewritten for the C++ `const` model (cv belongs to the
type, top-level and low-level const, `const` fields and methods, `const struct`/`const class`, `T* const`,
the `T** → const T**` rejection, `as` in place of `const_cast`); `move` defined as `&&` and made mutually
exclusive with `const` (3.4, 3.8, 4.4); `#if` evaluation tied to the single 9.1 DFS post-order sequence and
given the same name resolution as constant initializers; new 9.3 for the `comptime` unit and `builtin`
declarations (replacing `std.target`); 12 table entries 24/25/98/105/107/108/111 re-judged and 129–131
added. Review pass: `const struct` keeps its content fixed after construction (copy/move allowed, a variable
may be reassigned as a whole), a top-level `const` return type is accepted and ignored, and
`T** → const T**` has no implicit conversion but `as` can force it (C++ rules). Implementation of these
decisions is pending; compiler behaviour is unchanged in this build.

#### build 166 — 2026-11-17

**Second audit batch: sum/interface lowering, generic instantiation, contract spelling, diagnostics.**

- Sum equality with every alternative a unit value compares the tags directly; the lowering used to
  emit an empty switch and then a branch into the terminated block (invalid IR).
- `String(const char*)` treats a null source as the empty string, matching `cstr()`; `getenv` and
  other C APIs that may return null no longer crash through the implicit conversion.
- A generic type may refer to the instance being defined through a pointer or a borrow
  (`Node<T>* next`, `Box(const ref Box<T>)`): the instantiator registers the in-progress handle
  (`Type::in_progress`) before resolving the body, and a value payload that contains the sum itself
  is rejected as infinitely large (`sum_self_payload`) instead of killing the compiler in layout.
- Generic instantiation keeps the semantic flags the clone table used to drop: `virtual` on methods
  (a subclass `override` is accepted after instantiation), `is_implicit` / `is_move` / `is_copy` on
  constructors (the copy/move constructors get their own symbol and mark the type copyable/movable).
- A member operator satisfies an interface operator whose receiver is spelled `Self`
  (`int operator[](Self, isize)` ≡ `int operator[](isize)`) for classes too; a plain method's `Self`
  stays a parameter type (SPEC 3.15.6), so `less(Self other)` keeps matching `less(Box<T> other)`.
- A bare sum alternative wins over a same-named user function when the target type is that sum
  (`Option<int> o = Some(3);`), while the function still serves its non-sum call sites.
- `#if` checks enum identity like ordinary code: `os == Endian.Little` is now an error instead of
  matching on raw member values.
- The runtime settings reject values they cannot apply: `set_max_live_tasks(0)` is the documented
  kill switch, a negative count and an unusable `set_task_stack` pair throw.
- `-I` directories that shadow a `std.*` unit print a warning naming the file (the precedence is
  unchanged).

Tests: `sum_unit_eq`, `string_null_ctor`, `generic_selfref`, `generic_virtual`, `alt_shadow_both`,
`class_iface_index`, `contract_self_method`, `task_settings` (run); `sum_self_payload`,
`if_cross_enum` (must-fail); `tests/cli/std_shadow.py`.
Full suite: 713/713 (252 run, 424 must-fail, 24 check, 1 warning, 9 ir, 3 c abi).

#### build 165 — 2026-11-17

**审计 issue 批量修复（is 判别、复合赋值窄化、sum 载荷 RC、字符串边界、诊断与 CLI）。**

Language / compiler:

- `is` on a class or interface value is lowered through the `as` machinery: a null receiver matches
  nothing (used to dereference null and crash) and an interface aggregate takes its object pointer
  first (used to emit `getelementptr` on an aggregate, which is invalid IR, so interface
  discrimination never compiled). Binding forms bind only on a match.
- Compound assignment on numbers is checked as `x = x op y`: the arithmetic result must not narrow
  back into the target, so `byte b; b += 1000;` and `int i; i += 2^40;` are compile errors (unsigned
  wrap-around such as `uint u; u += 1;` stays legal).
- A sum value's assignment and copy-initialization now release the old payload and retain the new
  one (SPEC decision #55): `Holder h = ...; h = ...` used to leak one payload object per assignment,
  and `Holder h2 = h;` copied the tag without retaining the payload.
- Range `for-in` keeps its iteration counter in a hidden slot, so writing the loop variable is a
  per-iteration copy (it used to skip elements); generic bodies are checked in a clean para/lambda
  context, so the first instantiation inside a `para` section no longer rejects the body's `return`.
- Interface values accept `is`, and implicit construction applies to call results at coercion points
  (`string c = s.cstr();`), matching the variable form.
- Ternary arms now take the target type, so a bare sum alternative resolves there
  (`Option<int> v = c ? Some(7) : None;`).
- `sizeof` stays `isize` (SPEC decision 128): the Linux `cstd_posix` failure was a type mismatch in
  the case itself (`c_size` is `usize`), not a host-dependent compiler type; the case now casts at
  the `size_t` boundary and `sizeof_isize` pins the rule.
- `std.target.Abi` has a `Musl` member: a musl triple used to fall through to "the build
  configuration has no value for `abi`" while compiling `std.target`, which is the wrong diagnostic
  from the wrong stage. Such a target now compiles and is rejected at the link step with the
  cross-driver message (`is not the host; pass --cc`), as `tests/cli/target_platform.py` expects.
- `const` at unit level parses the function form (`const char* f() { ... }`), not just the variable
  form.
- Diagnostics: a binding `is` in an `and`/`or` operand names the "whole condition" rule instead of
  reporting `undefined identifier`; the multidimensional `for-in` hint names `a.length(k)` (the
  `len(a, k)` form was removed); the `override` diagnostic says "base class or interface method".
- `muc build` takes the entry file as the first non-option argument, wherever it appears, matching
  `muc check`; two entry files and a missing entry are reported explicitly.

Library:

- `String.operator[]` is bounds-checked and throws `BoundsError` (index / length / axis), as SPEC 3.7
  promises; `String.sub` clamps a negative start/count instead of reading before the buffer or
  aborting through an internal `buf[-1]`.
- Dynamic array handles share their buffer through the RC rules: README says so, and `slice_alias`
  pins the behaviour.

Runtime:

- The weak registry is sharded (64 lists + 64 locks, shard = `(target >> 4) & 63`), so
  `mu_rt_weak_unreg` / `mu_rt_weak_notify` walk `O(W/64)` instead of the whole global list and
  threads touching different targets stop sharing one cache line. Object layout is unchanged;
  measured per-object destruction at W = 100k went from 99 µs to 4.3 µs (`bench/weak_sweep.mu`).

Tests: `is_null_class`, `is_iface`, `forin_range_write_copy`, `compound_wrap_uint`,
`implicit_ctor_call_result`, `ternary_sum_context`, `sizeof_isize`, `string_index_bounds`,
`string_sub_clamp`, `const_return_fn`, `sum_assign_release`, `slice_alias` (run),
`compound_narrow_int`, `compound_narrow_byte`, `is_bind_compose`, `multidim_forin_len_hint`,
`long_chain_too_long`, `deep_nest_too_deep` (must-fail), `tests/cli/build_args.py`.
Full suite: 703/703 (244 run, 422 must-fail, 24 check, 1 warning, 9 ir, 3 c abi); the
check case para_mono_return pins the generic-instantiation-inside-para path.

#### build 164 — 2026-11-17

**A generic `ref T` return keeps the borrow at instantiation.**

- `instantiate_generic` resolved a method's return type with `resolve_type`, which silently drops
  `ret_ref`: the instantiated `MethodSig` promised a plain value, so every call was typed as a value
  (`by_ref == false`) and returned a copy — `cell.at().x = 5` wrote to that copy and the field never
  changed (a generic method also could not be used as a `ref` receiver). The signature now goes through
  `resolve_method_ret`, the path the non-generic declarations already use (the generic interface
  instantiation in the same function did the same).
- The instantiation-time check comes with it: `ref T` instantiated with a non-borrowable argument
  (a class reference, or a value that owns something) is now rejected instead of silently degrading to
  a value return. Regression cases: `tests/cases/generic_ref_ret`,
  `tests/errors/ref_generic_ret_rc`.
- Full suite: 684/684 (232 run, 416 must-fail, 23 check, 1 warning, 9 ir, 3 c abi).

#### build 163 — 2026-11-17

**`new T[n]` constructs its elements (`ctor_glue` for dynamic arrays).**

- `new T[n]` now runs the parameterless constructor of every element, in a runtime loop emitted in
  `emitSliceAlloc`. Zeroing already covered POD fields and the handles, so the loop only exists for
  element types that have a constructor to run. A throwing constructor destroys the elements built so
  far (reverse order), frees the block and resumes unwinding; the element that failed is released by the
  existing ctor-progress cleanup (`tests/cases/slice_ctor_throw`).
- An element type that declares constructors but **no parameterless one** is now rejected:
  `new T[n] default-constructs every element, but struct 'S' has no parameterless constructor`
  (C++ refuses the same way). `std.string.String` gained `String() {}` — the zero value, which is what
  `string s;` and `new string[n]` were missing.
- `emitCtorGlue` takes an optional throw-cleanup callback so the per-element check can release the
  prefix; `tests/cases/struct_dtor_elem` now also asserts that elements are constructed (`x == 7`).
- Full suite: 682/682 (231 run, 415 must-fail, 23 check, 1 warning, 9 ir, 3 c abi).

#### build 162 — 2026-11-17

**Destructible structs are dynamic array elements; a host that contains one must declare its own copy/move constructor.**

- A struct with a **user destructor is a valid `T[]` element**: destruction is per element (already
  implemented) and the block is zeroed. The stale gates are gone from `sema_types.cpp` (the `T[]` type
  form) and `sema_call.cpp` (the `new T[n]` form); `tests/errors/dyn_dtor_elem` became the run case
  `tests/cases/struct_dtor_elem`.
- **Fixed a silent shallow copy** (`Holder b = a;` where `Holder` contains a value with a user
  destructor): the host copy byte-copied the field without invoking its copy constructor, so two owners
  freed the same block (reproduced as a double `free`). `Type::copyable()` / `Type::movable()` now treat
  a nested value that declares a destructor (directly, or through a struct / fixed-size array field) as
  requiring the host's **own** copy/move constructor — the compiler's glue reproduces reference-counted
  handles but never invokes a nested user constructor. Without one it is a compile error, with the
  rule-of-three diagnostic. Regression cases: `tests/errors/struct_dtor_copy_ctor`,
  `tests/errors/struct_dtor_move_host`.
- `ptr()` and `uninit new T[n]` now reject an element whose destruction is observable **through
  nesting**, not only when the destructor is declared directly (`Type::needs_dtor()` instead of
  `has_dtor`) — that is what the raw-pointer and undefined-block paths actually bypass.
- **Same fix on the assignment path**: `b = a` used copy-and-swap only when the target type declared the
  destructor *itself*; a host whose field declares it fell through to the generic byte copy (another
  double free). The predicate is now `Type::needs_ctor_glue()` there too. Run case:
  `tests/cases/struct_dtor_assign_host`.
- Full suite: 681/681 (230 run, 415 must-fail, 23 check, 1 warning, 9 ir, 3 c abi).

#### build 161 — 2026-11-17

**`operator[]` must be a member of the type it indexes.**

- The free-function form `T operator[](C c, isize i)` is now a compile error:
  `operator[] must be declared inside the type it indexes (a member of the struct or class): the
  free-function form copies the receiver on every element access`. Build 160 made the member form
  available on classes, so nothing needs the free form any more — and it was a 50× trap: a class cannot
  take a `ref` parameter, so the free operator copied the handle per element (one retain/release pair,
  7.5 ns vs 0.15 ns; `Mulan-Agents/bench/value_vs_class/iteration.mu`).
- Resolution is now single-form: `for-in`'s index shape and `c[i]` both look only at the member operator,
  so `Index::get_func` and `ForInStmt::idx_fn` (and their lookup/codegen/may-throw plumbing) are gone.
  The diagnostics point at the member form: `for-in cannot iterate S: the indexed form requires a member
  'operator[](isize) -> T' in 'S'`, and a failed subscript says `declare 'operator[](…)' inside 'S'`.
- A member subscript now converts each index to the operator's **declared** parameter type (it used to
  force `isize`), which is what lets `int operator[](int i)` and multi-index `operator[](usize, usize)`
  work as members — previously only the free form could declare a non-`isize` index.
- Interface `IIndex<T>` / `IIndexMut<T>` are unchanged as contracts; satisfaction no longer has a
  free-function spelling.
- Tests: `free_index_op` (must-fail). Migrated to the member form: `class_array`, `forin_index`,
  `multi_index_op`, `para_for_throw_index`, `ref_return`, `objmodel_basic`, `iface_subscript`, and the
  must-fail cases `forin_badidx` / `forin_badlen` / `forin_nolen` / `forin_ref` / `index_compound` /
  `ref_loop_const`.
- Full suite: 678/678 (228 run, 414 must-fail, 23 check, 1 warning, 9 ir, 3 c abi).

#### build 160 — 2026-11-17

**Methods are static-dispatch by default: `virtual` is opt-in; a class may declare member operators.**

- **`virtual` (new keyword).** A base-class method takes a vtable slot only when declared `virtual` (or
  abstract, which is implicit). `override` now requires the base declaration to be virtual: replacing a
  non-virtual method is a compile error naming the declaration to mark
  (`'A::f' is not virtual: declare it 'virtual' in 'A' before overriding it`), not C++-style silent
  shadowing. An `override` stays virtual, so a further subclass can replace it again; constructors are
  never virtual; destructors keep dispatching through the class descriptor. Interfaces are unchanged —
  their members are reached through the itable, so implementing one needs no `virtual`.
- **Class member operators.** The parser no longer refuses `operator` inside a `class`. The receiver is
  the handle (a borrowed `this`), so `c[i]` costs no object copy, and `for x in c` over a class container
  no longer pays a retain/release pair per element: measured 152 ms → 3 ms for 20 M element accesses
  (`Mulan-Agents/bench/value_vs_class/iteration.mu`). `==` / `!=` remain identity comparison and are
  rejected as members. A member operator may be `virtual`; calls then dispatch like any other method.
- **Vtable layout**: only virtual/abstract methods occupy slots, so a class with no virtual method emits
  no vtable at all (the descriptor points at null) — `tests/ir/export_surface` had to mark its exported
  class's method `virtual` to keep asserting that a vtable survives internalization.
- Why: methods were virtual by default, so every call paid `desc → vtable → slot` plus an indirect call
  that LLVM could not devirtualize inside loops (build 159's closed-world analysis recovered the calls
  whose signature no subclass overrides; this makes the intent explicit *and* lets a container's
  subscript stay static). Container-selection consequences: `agents/stdlib/containers.md` §7/§8.
- Tests: `class_member_op` (run: member `[]`, member `+`, virtual operator override), `override_nonvirtual`
  (must-fail), `override_required` / `override_ret_mismatch` updated to declare `virtual` first,
  `member_op_class` now asserts the `==` rejection, `dispatch_override` / `devirt_dispatch` /
  `class_ctor_dispatch` / `export_surface` declare `virtual` where they rely on dynamic dispatch.
- Full suite: 677/677 (228 run, 413 must-fail, 23 check, 1 warning, 9 ir, 3 c abi).

#### build 159 — 2026-11-16

**Closed-world devirtualization; weak notification stops touching the lock when the registry is empty;
reserved words enforced.**

- **Class method calls no longer pay the vtable when the target is unambiguous.** Mulan is closed-world
  (no class object crosses the C ABI — `export` refuses class types — the whole program is one module and
  every vtable is internal), so a call can name its target statically when **no strict subclass of the
  receiver's static type declares a matching method**. `emitMethodCallVals` now asks
  `methodDispatchIsStatic` and falls back to the existing direct-call path instead of
  `load desc → load vtable → load slot → call %fn`. This is what lets LLVM inline the call: a loop of
  `c.get(i)` over a class container drops from 1.1 ns to 0.15 ns per element (measured,
  `Mulan-Agents/bench/value_vs_class/iteration.mu`), and whole loops collapse to closed form.
  Abstract methods and any signature that is overridden somewhere keep the vtable — dispatch semantics
  are unchanged, only the indirect call disappears.
- **Weak-registry notification**: `mu_rt_weak_notify` / `mu_rt_weak_unreg` used to jump to a single
  `done` block that unconditionally released the spin lock, so the "registry is empty" path executed a
  `xchg 0` on a global cache line — on **every** object destruction. The two exits are now separate
  (empty → return, locked → unlock → return), and under `--single-thread` the empty check is a plain
  load instead of an atomic acquire. A program without `weak` fields now emits a destroy function that is
  just `free` + the user destructor.
- **SPEC 2.5 reserved words are enforced**: `where` / `typeof` are rejected as identifiers at the lexer
  with a diagnostic naming the rule (they were documented as reserved but accepted). Tests:
  `reserved_word_where`, `reserved_word_typeof`.
- Tests: new `tests/ir/return_inplace` pins that a struct result is constructed in the caller's `sret`
  slot (`- llvm.memcpy`, `- %ret.struct`); `tests/cases/devirt_dispatch` + `tests/ir/dispatch_override`
  pin the other half of the devirtualization contract — a signature that some subclass overrides keeps its
  vtable slot and is never bound statically, so an override still runs through a base-class handle.
  SPEC 6.4.1 now documents that a **class** container cannot avoid the per-element receiver copy (no
  member operator, `ref` parameters refuse class types), with the measured numbers and the
  container-selection rule. SPEC 3.15.7 and 3.16.5 refreshed (generic interfaces as value types, generic
  `struct S<T> : I`, capturing closures are all implemented).
- Full suite: 675/675 (227 run, 412 must-fail, 23 check, 1 warning, 9 ir, 3 c abi).

#### build 158 — 2026-11-16

**Compiler directives: `#if` / `#elif` / `#else` / `#error` (SPEC 9.2).**

- `#if <cond> { … }` selects which code *exists* in a build. It is a compiler directive, not a
  preprocessor: no macros, no textual substitution, no `#include`, and no `#endif` — the arm is a
  braced group.
- The selected arm is **spliced** into the enclosing declaration or statement list: the braces delimit
  the directive and do **not** create a scope (`int h` declared in an arm stays visible after it).
  Dropped arms are still parsed, so syntax is checked in every configuration, but they are never
  name-resolved, type-checked or emitted; a nested `#if` inside a dropped arm is not evaluated, and a
  `use` there does not join the dependency graph.
- Conditions are evaluated while parsing (before name resolution), so they may only use the build
  constants `os` / `arch` / `abi` / `ptr_bits` / `endian` / `debug` / `bounds` / `single_thread`, the
  enums `OS` / `Arch` / `Abi` / `Endian` (declared in the new unit `std.target`, whose values the
  compiler fills and cross-checks), literals and the arithmetic / comparison / `and` / `or` / `not`
  operators. A runtime value, `sizeof` or a call is rejected with a message that explains why.
- Positions: unit top level, struct / class / interface bodies, function bodies and blocks.
  `#` must be the first non-blank character of its line.
- New: `muc dump-config` prints every build constant; `std.target` is loaded implicitly.
- Tests: `if_config`, `if_target`, `if_dead_use` (run) plus seven must-fail cases covering the
  rejected condition shapes, dead-branch syntax errors, live `use` of a missing unit and `#error`.

#### build 157 — 2026-11-16

**Contract operators with mixed operands and `ref` receivers; compound assignment on user types.**

- A contract operator no longer requires **every** operand to be `Self`. The receiver (the first
  operand) must be `Self`; the rest are matched exactly as declared. `Self operator<<(Self b, byte c)`
  (a stream), `Self operator*(Self a, int k)` (scaled arithmetic) and `bool operator<(Self a, int b)`
  are now expressible; the built-in block still only answers the all-`Self` form. The old rule was
  written for `Self op Self` and already had a subscript-shaped exception.
- A contract operator receiver may borrow: `ref Self` (mutate in place) or `ref const Self`
  (read-only). A mutable `ref Self` is satisfied by a free `operator` with a `ref` first parameter or
  by a struct member operator (its `this` is borrowed); a by-value implementation touches only a copy
  and is rejected. A `ref const Self` accepts a free `ref const` (or by-value) first parameter and
  rejects a member operator (there are no `const` methods). Only the receiver may be a `ref`
  parameter; interface **methods** still do not take `ref` parameters.
- Interface operator members are compile-time only in both directions now: they get no itable slot
  and do not make a class abstract, so an interface may mix ordinary methods with operators.
- `x op= y` and the statement forms `x++` / `x--` now work on user types by rewriting to
  `x = x op y` (the operator set is 3.11's; `x++` lowers to `x += 1` first). The rewrite spells the
  target twice, so the left side must be side-effect free; a numeric/pointer target keeps the
  in-place codegen path (evaluated once). `String +=` works as a consequence.
- Operator calls now check `ref` arguments during overload resolution (addressable lvalue, no
  conversion, no readonly-to-mutable binding). This fixes a latent crash: a chained `ref`-receiver
  operator used to accept the previous result (a temporary) and generate a dangling borrow.
- An operator may return `ref T`; the result is a borrow (chained `b << 1 << 2` works for borrowable
  types, and `Buf x = b << 1` copies the value). Generic contracts (`IOut<int>`) are validated the
  same way when they are instantiated: receiver first, `ref` only on the receiver, borrow returns only
  for `operator[]`.
- Instantiation-time diagnostics now carry the chain (`while instantiating \`pick<int>\` at
  file:line:col`) and, for an unconstrained type parameter, a hint that the body was not checked at its
  definition and how a constraint would move the check there. `--strict` (on `check` and `build`)
  reports generics that are never instantiated, whose bodies are therefore never checked.
- The instantiation guard is exception-safe: a failed body no longer leaves the recursion set or the
  instance cache populated, so the diagnostics that follow do not turn into a bogus "recursive
  generic instantiation".

#### build 156 — 2026-11-16

**A constrained generic body is checked at its definition, against a witness of its contract.**

- Until now a generic body was only checked when instantiated, i.e. one concrete type argument at a
  time. A body could therefore use more than its constraint promises: with `T: IRead<int>` (a
  value-returning subscript) the statement `c[0] = 5` compiled whenever the argument type happened to
  return a `ref` subscript, and the first type that returned a value hit a compile error inside
  library code. "It compiles" did not mean "it works for every `T`".
- The body of a generic function whose type parameters are all constrained by a conjunction of
  interfaces is now also checked once, at its definition, with each parameter bound to a **witness
  type** that carries exactly the constraint's members (methods and operators, `Self` mapped to the
  witness, a `ref` return kept as a borrow). The body is checked as written — no clone, no
  substitution, no emission — so a use the contract does not grant is reported at the definition site
  with the ordinary diagnostic (`cannot assign through a subscript: operator[] must return ref T`,
  `has no field v`).
- Unconstrained type parameters and `A | B` bounds are not witnessed (there is no single member set);
  those bodies keep the previous behaviour. A witness never reaches codegen: it is only a checking
  device.
- Fixed on the way: a **generic interface** dropped `ref` on a method return (`m->ret_ref` was ignored
  when the instance's signature was built), so `ref T operator[](Self c, isize i)` promised a value.
  It now goes through the same `make_borrow_ret` path as a non-generic interface.
- New cases: `contract_body_write` and `contract_body_field` (must-fail, both reported at the
  definition), `contract_body_generic_call` (run: a constrained generic calling another one).
  `member_op_iface` no longer reaches into a field of the type parameter — that was the very
  permissiveness this change removes. Full suite: 643/643 with `MUC_VERIFY_MEM=1`.

#### build 155 — 2026-11-16

**Short contract names, and the ordering family derived from `operator<`.**

- Renamed (breaking; nothing outside the library used them): `IEquatable` → `IEqual`,
  `IOrdered` → `IOrder`, `IAddable` … `IModulable` → `IAdd` / `ISub` / `IMul` / `IDiv` / `IMod`,
  `INegatable` → `INeg`, `IEnumerable` → `IIterable`, `IIndexable` → `IIndex`,
  `IMutableIndexable` → `IIndexMut`. The `-able` suffix survives only where the bare word would read
  as an action (`IIterable`); the rest are the operation or concept name itself, so there is nothing
  to memorize.
- Each comparison contract now asks only for the operator the compiler can derive the rest from:
  `IEqual` requires `==`, `IOrder : IEqual` requires `<`. A type that declares `!=` / `>` / `<=` / `>=`
  still satisfies them and its own operator wins.
- `a > b`, `a <= b` and `a >= b` are derived from `operator<` when no such operator exists
  (`b < a`, `!(b < a)`, `!(a < b)`), the same fallback `!=` already used against `==`. Derivation is
  a fallback, so a float keeps its native `<=` and NaN semantics. A derived `>` / `<=` puts the other
  operand in the receiver position, so its operands are evaluated right-to-left (documented).
  `IOrder` therefore promises a **total order**.
- A missing `>` / `<=` / `>=` now says whether there was an `operator<` to derive it from.
- New cases: `derive_compare` (run, covers equal values, a never-less `<`, and a declared `>` winning),
  `derive_compare_missing` (must-fail). Full suite: 640/640 with `MUC_VERIFY_MEM=1`.

#### build 154 — 2026-11-16

**Read and read-write index contracts; a built-in block is writable through the subscript.**

- A subscript carries a capability that one operator expresses only through its return type (`T` = read,
  `ref T` = read-write), so the library now ships it as a pair:
  `IIndexable<T>` (read; satisfied by a value or a `ref` implementation) and
  `IMutableIndexable<T> : IIndexable<T>` (read-write; satisfied only by a `ref` implementation).
  A generic body that writes through an element — including `for ref x in c` — has a contract to name.
  Generic interface inheritance substitutes its type argument, so `IMutableIndexable<Buf>` also checks
  the inherited `IIndexable<Buf>` members.
- `int[]` satisfies both (its block is mutable); a value-returning subscript satisfies only the read form
  and is rejected by the mutable form with the `for ref` diagnostic.
- New cases: `iface_mutable_indexable` (run), `iface_mutable_readonly` (must-fail). Full suite: 638/638
  with `MUC_VERIFY_MEM=1`.

#### build 153 — 2026-11-16

**A generic `ref` parameter is checked after substitution, and a `ref` subscript works in a constraint.**

- `ref`-parameter legality ("a borrow may not alias a value that owns something") was only checked for
  concrete types. A type parameter is resolved at instantiation, so `void f<T>(ref T c)` compiled for a
  `T` holding a managed sequence and then **corrupted the heap** at run time (the argument was treated
  as owned: the code emitted `atomicrmw add` against the address of the handle instead of the block, and
  the caller's handle was mangled). The check now runs in `instantiate_generic` with a diagnostic naming
  the parameter, the offending type and the reason (reference-counted field / union / managed sequence).
- A constraint can declare a **`ref` subscript** (`ref T operator[](Self c, isize i)`), a `ref`
  implementation satisfies it, and a generic body can therefore write through the elements:
  `c[i] = v` now type-checks and stores into the caller's block. The container is passed **by value**
  (the handle is shared), which is what a generic algorithm needs — no `ref` borrow of the container is
  required, and the language keeps refusing one for a type that owns a block.
- New cases: `ref_generic_rc` (must-fail), `iface_ref_subscript` (run). Full suite: 636/636 with
  `MUC_VERIFY_MEM=1`.

#### build 152 — 2026-11-16

**A subscript is declarable in a constraint, so `IIndexable<T>` can actually constrain generic code.**

- `operator[]` / `operator[]=` are asymmetric: only the **receiver** operand has to be `Self`, the
  operands after it (an index, a key) may be any type. The old rule demanded that every operand be
  `Self`, which is exactly wrong for a subscript — an interface could declare it, but **no type could
  ever satisfy it**, so `IIndexable` silently lacked its subscript half.
- An interface member has no `this`, so the signature must spell the receiver:
  `T operator[](Self c, isize i)`. A container satisfies it with either spelling — a member
  `T operator[](isize)` or a free `T operator[](C, isize)` — and a `ref T` implementation now
  satisfies the declared `T` (a borrow is usable as a value). The same relaxation applies to
  free `operator` functions.
- Built-in managed sequences count as satisfying the method half: `T[]` has no declared methods (its
  `length()` is compiler-lowered), so `T[]` now satisfies `IIndexable<T>` through the built-in
  subscript. Generic algorithms can therefore accept a block and a container alike.
- A member spelling inside an interface now gets a diagnostic that says to write the `Self` receiver.
- `std.interfaces.IIndexable<T>` declares `T operator[](Self c, isize i)`.
- New cases: `iface_subscript` (check), `iface_indexable` (run, covers a member container, the shipped
  contract and `int[]`). Full suite: 634/634 with `MUC_VERIFY_MEM=1`.
- Still open: a constraint cannot express a **`ref`-returning** subscript, so `for ref` is unavailable
  inside a generic body over `IIndexable` (recorded in the internal interfaces record).

#### build 151 — 2026-11-16

**Dynamic arrays are a built-in type with a fixed length; `isize` is the index/length type.**

- `T[]` / `T[,]` / `T[,,]` are now **built-in** (`{rc,len,data}`, no capacity field): the length is
  fixed for the lifetime of the block, so nothing reallocates and an element address stays valid
  until the handle is released. A library container (`Array<T>`) becomes a wrapper that adds methods.
  The `buffer<T>` name is removed (using it is an error pointing at `T[]`).
- Built-in members are only `length()` / `operator[]` / `ptr()`. `ptr` is now a **method** (`x.ptr()`,
  the field form is rejected), returns `const T*` for a `const` block, and requires a **trivial**
  element type: a raw write through it would bypass an element's retain/release.
- `new T[n]` still zeroes the element block, but it now goes through `calloc`, so libc can skip the
  zeroing for a fresh mapping. `uninit new T[n]` opts out of the zeroing and is accepted only for
  trivial element types; the header stays valid either way. The 8-byte `cap` field of rank-1 blocks
  is gone, and the data offset is padded to 16 bytes, so every supported element alignment is a
  multiple of the offset.
- **`isize` replaces `usize` for lengths, indices and byte sizes**: `length()`, subscripts, the
  `for i, x` ordinal, `find` offsets, `sizeof`/`alignof`/`offsetof` and the `alloc`/`realloc` byte
  counts. `usize` stays for unsigned arithmetic and the C-facing `cstd.*` sizes, where an explicit
  `as` is now required (e.g. `malloc(sizeof(T) as c_size)`). The bounds check is unchanged: a single
  unsigned compare catches a negative index on the cold path.
- `length(axis)` is accepted at rank 1 too (`length()` ≡ `length(0)`), and a runtime axis is checked.
- New cases: `slice_zero_init`, `slice_uninit`, `slice_ptr`; new must-fail cases
  `slice_uninit_ref`, `slice_ptr_nontrivial`, `slice_ptr_field`, `slice_ptr_const`,
  `slice_axis_rank1`, `buffer_removed`. Full suite: 632/632 with `MUC_VERIFY_MEM=1`.

#### build 150 — 2026-11-16

**Weak registry: empty-list fast path, and no spin lock under `--single-thread` (#18, partial).**

- `mu_rt_weak_unreg` and `mu_rt_weak_notify` now begin with an acquire load of `mu_weak_head` and
  return immediately when it is null. Every class destruction calls `mu_rt_weak_notify`, so a program
  with no live `weak` cells used to pay a lock/unlock pair per destruction; it now pays one load.
- `--single-thread` skips the registry spin lock entirely (no other thread exists), which removes two
  more atomics per destruction in that mode.
- The `#18` cost that remains — `unregister` walks the whole global list, so the per-object cost still
  grows with the number of live weak cells, and all threads serialise on one cache line — needs the
  per-target intrusive list (a design decision recorded in the issue); the issue stays open.
- `tests/bench/rc_new.mu` reworked to be escape-proof (the handle escapes into a global strong
  reference and the destructor is observable), because the optimizer legitimately deletes a
  `new` + drop sequence once reference counting stops being atomic. Measured with it, best of 3
  (Windows x64, MinGW LLVM, O2): **36.5 ns/object** default vs **19.3 ns/object** `--single-thread`.
- The IR cases `atomic_rc` / `single_thread_rc` now compile the *same* program (weak field, loop,
  observable destructor) in both modes: the default pins `atomicrmw add/sub/xchg` + `fence acquire`,
  the single-thread one pins that no `atomicrmw` and no fence are left at all.

**Tests**: 624/624 (211 run + 382 must-fail + 21 check + 7 ir + 3 c abi).

#### build 149 — 2026-11-16

**`--single-thread`: opt-in non-atomic reference counting (#19).**

- `muc build --single-thread` (accepted by `muc check` as well) emits the retain/release helpers and the
  slice-buffer release as plain loads and stores: no `atomicrmw`, no acquire fence. Default builds keep
  atomic RC (relaxed increments, release decrement + acquire fence).
- The mode is only sound if nothing is shared, so concurrency becomes a compile error instead of a
  warning: `para`, `para for`, and `use std.thread` / `use std.task` are rejected with
  *"needs atomic reference counting"* (the library's own `std.*` units are exempt, so `std.thread`
  itself still compiles).
- Explicitness rule: the flag never changes default semantics — a program compiled without it behaves
  exactly as before, which `tests/ir/atomic_rc` pins (`+ atomicrmw ... / + fence acquire`).
- Benchmark `tests/bench/rc_new.mu` (20M × `new Node` + drop at the end of the scope, best of 3,
  Windows x64 / MinGW LLVM O2): **32.9 ns/object** atomic vs **26.8 ns/object** `--single-thread`
  (≈18% faster, 6.1 ns saved per object), against 33.4 ns measured for 0.0.128 in the issue.
- Tests: `single_thread_rc` (run, semantics with weak + array release), `single_thread_para`,
  `single_thread_para_for`, `single_thread_std_thread` (must-fail), `single_thread_rc` / `atomic_rc`
  (ir, the atomic and non-atomic forms of the same program).

**Tests**: 624/624 (211 run + 382 must-fail + 21 check + 7 ir + 3 c abi).

#### build 148 — 2026-11-16

**Two follow-up reports (#40, #41).**

- **Imported declarations of different kinds are ambiguous (#40)**: a bare name that is a variable in
  one `use`d unit and an enum member (or a function) in another is now an error listing both qualified
  names, instead of silently resolving by kind priority.
- **POSIX `c_wchar` is signed (#41)**: `std.ctypes` on Linux and macOS declares `c_wchar` as `int`
  (glibc/Darwin `wchar_t` is a signed 32-bit type), not `uint`; Windows keeps `ushort`.

**Tests**: 618/618 (210 run + 379 must-fail + 21 check + 5 ir + 3 c abi).

#### build 147 — 2026-11-16

**Eleven review reports (issues #29–#39): symbolic-name soundness, C-ABI ownership, and library/CLI
correctness.**

- **Symbol mangling is injective (#35, ABI-breaking)**: components are now separated by `$`, every
  literal `_` is doubled and every other non-alphanumeric byte becomes `_XX` (uppercase hex), so
  `lib_u_a` + `b` and `lib_u` + `a_b`, or `Box<int>` and `Box_int_`, can no longer collapse into one
  symbol. `mu$<unit>$<name>$<param>...` replaces the old `mu_<unit>_<name>__<param>` form; `tests/abi/*.c`
  and the IR assertions were updated, and `tests/ir/mangle_injective.txt` pins the distinction.
- **Generic recursion terminates with a diagnostic (#31)**: an expansion whose next instance never
  repeats a key (`Deep<T>` → `Deep<Box<T>>` → …) used to overflow the stack; a 256-level nesting limit
  reports `recursive generic instantiation … does not terminate` instead.
- **`swap` rejects `weak` fields and values embedding one (#29)**: the weak registry is keyed by
  (target, field address), so a raw byte exchange would leave both registrations attached to the wrong
  holder.
- **A cleanup-needing struct cannot cross the C ABI by value (#30)**: even when movable inside the
  language, a caller that does not follow Mulan's ownership protocol cannot hand over a destructor-owned
  resource; `extern`/`export` parameters and results are rejected with a pointer/plain-struct hint.
- **`std.string` (#32–#34)**: `String.sub` copies into its own NUL-terminated buffer and clamps its
  range (a start past the end is the empty string); the zero value's `cstr()` is `""`, not null;
  `find` returns `long` so a valid offset can never be confused with `-1`.
- **`std.cstdio` is platform-neutral (#39)**: the implementation-defined `BUFSIZ` / `FILENAME_MAX`
  moved out of the portable declaration layer (the platform values live in `cstd.stdio`).
- **Enum constants (#36, #37)**: a value (explicit or incremented) outside the underlying `int` range
  is rejected, and two enums can no longer declare the same unqualified member name (write `E.MEMBER`).
- **Unit loading reports cycles (#38)**: the back-edge check now precedes the "already loaded" lookup,
  so `a` → `b` → `a` is an error instead of a silently half-built unit.

**Tests**: 617/617 (210 run + 378 must-fail + 21 check + 5 ir + 3 c abi); CLI 9/9.

#### build 146 — 2026-11-16

**Six correctness bugs and two CLI validation gaps found in a review pass (issues #21–#27).**

- **`move` cleanup is path-sensitive (#21, #23)**: every owning local that can be moved now carries a
  runtime moved flag (an `i1` slot): `move x` sets it, a new assignment clears it, and every release site
  (scope exit, `break`/`continue`, `return`, unwinding) tests it. The compile-time cleanup scopes are
  registered once at the declaration and never removed, so a `move` written in a branch no longer cancels
  the cleanup of the paths that did not execute it. A local revived by assignment is registered back into
  the scope that declared it, so reassigning a moved outer variable inside a nested block no longer
  destroys it at the inner block's exit (#21). The compile-time `movedLocals` answer remains only for
  locals without a flag (captures and synthetic owners).
- **Partial construction releases exactly what was built (#24, #26)**: constructor bodies and inline
  member/element sequences now open a construction sequence (`CtorProgress`). When an exception leaves
  one, the subobjects that finished are destroyed with their full destructor in reverse construction
  order while the one under construction gets only its partial destructor (its user destructor body must
  not run); each released subobject is zeroed so the enclosing level's partial release is a no-op.
  Covered for a throwing nested struct field (#24), a fixed-array prefix (#26, including an array of
  structs with no user constructor) and a class's value members; `emitDtorPartial` /
  `emitStructDtorPartial` now recurse with partial destruction, and the may-throw analysis also sees the
  synthesized member step.
- **`for ref` accepts a member `operator[]` returning `ref T` (#27)**: `resolve_index_shape` prefers the
  member overload, but the borrow check only looked at the free-function form; both spellings are now
  validated (mutable borrow required).
- **`build` rejects a platform whose architecture or ABI differs from the codegen target (#22)**, and a
  musl target on a glibc host is a cross target that needs `--cc` (#25). The triple is parsed by role
  (arch / OS / environment), the Windows CRT families stay interchangeable, a triple whose runtime
  cannot exist on its OS fails early, and a linked ELF's `PT_INTERP` is checked against the requested
  runtime. New CLI checks: `tests/cli/target_platform.py`, run by CI on both hosts.
- New run cases: `move_cleanup_nested_scope`, `move_cleanup_conditional`,
  `exception_partial_nested`, `exception_partial_array`, `for_ref_member_index`; new must-fail cases:
  `forin_ref_member`, `forin_ref_member_const`.

**Tests**: 606/606 (209 run + 370 must-fail + 20 check + 4 ir + 3 c abi); `MUC_VERIFY_MEM=1` 606/606;
`tests/cli/target_platform.py` 9/9 on Windows.

#### build 145 — 2026-11-16

**The release version and the build number are now two independent numbers.**

- `VERSION` changed from a single `major.minor.build` string to two labelled lines:
  `version = major.minor.patch` and `build = <n>`. CMake parses both and defines the `MUC_VERSION` and
  `MUC_BUILD` macros; a malformed file fails at configure time with an explicit message.
- `muc --version` now prints `muc 0.0.0 (build 145)`.
- `--emit-ir` now honours `-g`: `llvm_emit_text` used to build the module without debug info, so the
  textual IR was the one place where the producer string could not be inspected. Both paths now construct
  the emitter with the same `EmitOptions`.
- The DWARF `producer` string and the module flags carry the build number as well
  (`mulan.version` + `mulan.build`), so an object file can be traced to the exact build that produced it.
- Version history now groups build entries under a release section. Entries up to 0.0.144 keep the old
  single-number headings under *Legacy numbering*.
- **Documentation-only follow-up (no build bump)**: #20/#28 — `README.md` now states the real platform
  status (Windows and Linux hosts, cross compilation via `--target` + `--cc`), and `docs/SPEC.md` /
  `docs/StdLibSPEC.md` were reconciled with the implementation: the SPEC status line tracks build 145,
  §3.4 user-destructor rules now match the 0.0.137–0.0.140 value semantics (copy/move constructors,
  copy-and-swap assignment), §3.12 no longer says user destructors do not exist (they are 3.13),
  §12 #11 now allows `for ref` for user types whose `operator[]` returns `ref T` (6.4.1), and
  `std.thread`'s `Mutex` / `CondVar` are documented as the 64-byte opaque blobs they are since 0.0.128.

### Legacy numbering (0.0.1 – 0.0.144)

#### 0.0.144 — 2026-11-16

**Fixed: the slot of a `ref` / `move` parameter was sized for the referent, not for the pointer.**

- `Emitter::bindParams` allocated a parameter's slot with the *referent* type (`lt(pl->rty)`), but a `ref`,
  `const ref` or `move` parameter's slot holds the **address**. Storing the 8-byte pointer into, say, a
  4-byte alloca is an out-of-bounds write — undefined behaviour, which the optimizer is entitled to exploit.
  `slotFor` already handled this; the parameter path did not. Both now allocate `ptr` with 8-byte alignment.
- How it surfaced: the value-semantics integration test produced wrong results at `-O2` — a later
  `if live != 0` was folded to "always true" and a move constructor call was deleted. The first diagnosis
  (an LLVM 23 regression) was wrong: the pre-optimization IR was *valid* but contained UB, which is why
  `verifyModule`, `opt -verify-each`, `-O0`-correctness and reproducing it with the system `opt` all pointed
  the wrong way. Root cause and the debugging recipe are recorded in `agents/llvm-assume-notes.md`.
- Impact: every `ref` / `const ref` / `move` parameter whose referent is smaller than a pointer silently
  corrupted adjacent stack slots. New run case `ref_slot_ub` (ref struct, const ref, `for ref`, move
  constructor, destructor counts); `value_semantics_matrix` is back to its full form.
- New debugging aid: `MUC_VERIFY_MEM=1` makes codegen check every `store` / `memset` / `memcpy` whose
  destination provably starts at an alloca against that slot's size, and abort with the function name and
  the offset. It is not a language feature — the LLVM verifier cannot catch this (an over-wide store is UB,
  not invalid IR) — so it runs as a separate pass after `verifyModule`, and the whole suite passes with it
  enabled. Verified in both directions: re-introducing the slot bug produces
  `stack write out of bounds in 'mu_ref_slot_ub_move_Val': 8 bytes at offset 0 into a 4-byte slot`.

#### 0.0.143 — 2026-11-16

**Built-in `swap`, a value-semantics integration case, and a `-O2` miscompile found while testing.**

- `swap(a, b)` is a built-in that exchanges the values at two writable places (variable, field, element,
  dereference) with a pure byte exchange: no constructor, destructor or reference-count operation runs. It is
  built in rather than written in the library because it needs the addresses of both places even for types
  whose address must not be taken (`&` is refused for a struct with reference fields), and because a byte
  exchange is exactly right for a reference-counted handle (the counts do not change). Both sides stay
  initialized, so it never interacts with the moved tracking; `swap(a, a)` is a no-op. This is the primitive
  `List::remove` / `swap_remove` will need. A hand-written three-step swap, by contrast, runs two spurious
  destructors on the emptied shells.
- `value_semantics_matrix` exercises the whole area end to end: a resource-owning struct as a struct field /
  array element / class member, implicit member construction, copy and move construction, synthesized
  assignment, `swap`, and reference counting next to value semantics, with every counter back at zero.
- `swap` is a hard keyword now; two test fixtures that used it as a function name were renamed.
- **Found while testing (fixed in 0.0.144): `-O2` produced wrong code for one combination** — a "resource
  struct + RC member" block followed by a `nocopy` + `move` block folded `if cat_live != 0` to "always true".
  The first diagnosis blamed the optimizer; it was our own out-of-bounds write (see 0.0.144).

#### 0.0.142 — 2026-11-16

**Diagnostics point at `ref`, not at a pointer, and only suggest what the type actually supports.**

- A borrow (`const ref T`) is the language's way to use a value without copying it; a pointer is a lower-level
  tool for C interop and manual memory. The "cannot copy / pass / return by value" diagnostics now offer
  `const ref T` instead of a pointer.
- The advice is conditional on the type, so it never suggests something that would itself be rejected:
  `, or move it with \`move\`` only when the type is movable, `, or borrow it as \`const ref T\`` only when the
  type is borrowable, and a return position lists the copy/move constructor declarations (a struct with a
  destructor cannot be borrowed, so no `ref` return is offered).
- No behaviour change; the seven must-fail expectations were regenerated from the new messages.

#### 0.0.141 — 2026-11-16

**The "cannot copy" diagnostics state the rule, not just the verdict.**

- A destructor is a declaration that the type owns a resource, so the author has to say how that resource is
  duplicated — that is what the rule-of-three rule means here. The copy / pass-by-value / return-by-value
  diagnostics now say which of the three reasons applies and what to write, instead of one parenthetical:
  - "it declares a destructor, so it owns a resource; declare `T(const ref T other)`"
  - "it is declared `nocopy`; …"
  - "it contains a value that owns a resource; …" (a member with a destructor or a `nocopy` member)
- No behaviour change; seven must-fail expectations updated to the new wording.

#### 0.0.140 — 2026-11-16

**`const ref T` is the preferred spelling for a read-only borrow.**

- `const ref T p` parses to the same thing as the older `ref const T p` (which stays accepted, so no existing
  code changes meaning): a read-only borrow. `const ref` reads like C++'s `const&`, which is why it becomes
  the spelling used in the specification, the diagnostics and new code.
- The copy constructor is now written `T(const ref T other)`; the diagnostics for a mutable `ref`-taking copy
  constructor point at that form.

#### 0.0.139 — 2026-11-16

**The copy constructor borrows its source read-only.**

- `T(const ref T other)` is the copy constructor form (initially written `T(ref const T other)`; 0.0.140
  made `const ref` the preferred spelling). The source is read-only by contract, and the language relies on
  that: `b = b`, `f(x, x)` and `arr[i] = arr[j]` (with `i == j`) are correct precisely because the copy
  finishes before the swap, so the source must not be writable through the parameter. Writing `ref T` now
  reports "a copy constructor must not modify its source".
- `ref const` also accepts a `const` binding as the source, which a mutable `ref` cannot (the same
  `ref_param_const_to_mutable` rule); C++'s other reason for `const&` — binding a temporary — does not
  apply here, because `ref` parameters reject temporaries and rvalues take the in-place construction path.
- The move constructor keeps `move T` (it has to write through the source to empty it). SPEC 3.13 updated;
  new must-fail case `copy_ctor_mutable`.

#### 0.0.138 — 2026-11-16

**Copy construction and synthesized assignment (W5b).**

- `T(ref T other)` declares the copy constructor (the pair of `T(move T other)`); `ref` parameters are now
  accepted on a constructor **only** for that form. `copyable(T)` becomes true for a type that owns a
  destructor and declares one, so `S b = a;` performs a deep copy through it. `nocopy` still wins: a
  `nocopy` type cannot declare a copy constructor, and the duplicate-signature check now includes the
  parameter mode (`ref` / `move`), so the copy and move constructors no longer collide.
- **Assignment is synthesized as copy-and-swap** (`agents/value-semantics.md` §8.11): for a type with a
  user destructor, `b = a` builds the new value in a temporary through the copy constructor, swaps the two
  byte blocks, and destroys the temporary (which now holds the old value). `b = move a` does the same with
  the move constructor. Consequences: a throwing copy constructor leaves the destination untouched, and
  self-assignment (`b = b`, `f(x, x)`, `arr[i] = arr[j]` with `i == j`) is correct without an identity
  check, because the copy happens before the swap. Aliasing is real even though the language has no
  reference types — pointers, `ref` borrows and runtime-equal indices all alias.
- A value type must not store a pointer into itself, since the swap is a byte swap; such types belong to
  `nocopy` (and are usually immovable too).
- Codegen detail: the copy constructor takes its source **by pointer** (like the move constructor), so the
  body borrows the source without touching its reference counts.
- Tests: `copy_assign` (deep copy, copy assignment, self-assignment, balanced buffer count) and
  `move_assign` (old value destroyed, source emptied, buffer changes owner once).

#### 0.0.137 — 2026-11-16

**Move semantics for structs (W5a): `move x`, `T(move T other)`, and "a moved value is out of reach".**

- `move x` is a prefix expression that transfers ownership out of a local variable or parameter. Only a whole
  name may be moved (`move o.field` / `move a[i]` are rejected — a partial move would need field-level
  state, which is what an ownership checker is for); the value must be movable, and `const`/`ref` bindings
  cannot be moved. Positions: `S b = move a;`, `b = move a` (into an already constructed destination),
  `f(move x)`, `return move x;`.
- `T(move T other)` declares the move constructor. It is the only way to make a type with a user destructor
  movable: `movable(T)` is a new recursive predicate — a type with a destructor or with `nocopy` is movable
  only when it declares one, and everything else is movable by the compiler (copy the bytes, zero the
  source), which is exactly an ownership transfer for a handle. That keeps `struct S { ~S(){} }` immovable
  until it says `S(move S other)`, per the decided rule that a move constructor is opt-in.
- Codegen: the move constructor takes the source **by pointer** (like `ref`), so the body steals from it
  without the parameter retaining anything; the compiler then zeroes the source and drops the variable from
  the cleanup list, so it is not destroyed twice. A type without a move constructor transfers by copying the
  bytes and zeroing the source. `valueOwned()` treats `move x` as owned, so returning/passing it inserts no
  extra retain.
- **A moved value cannot be used**: any later read/write/borrow/pass/return of the variable is a compile
  error, until a new assignment re-arms it. The analysis is a per-body may-move pass (locals and parameters
  only): `if`/`else` union the two arms, and a `move` may not target a variable declared outside the
  enclosing loop (the next iteration would reuse it). Cross-function tracking is not needed: a moved value
  cannot escape except through `return`/by-value arguments, which are the transfer points themselves.
  A second visit of the same `move` node (argument coercion re-checks its arguments) does not report itself.
- Signatures: a parameter or return type that is uncopyable but **movable** is now accepted, so move-only
  types can be returned and passed (`return move b;`, `take(move c)`); passing a plain lvalue of such a type
  is still an error, which is the "write `move`" diagnostic.
- Tests: `move_ctor`, `move_field_transfer` (implicit transfer of a reference-counted field),
  `move_return` (return + argument), and must-fail `use_after_move`, `move_field`, `move_in_loop`,
  `move_not_movable`, `move_arg_copy`.

#### 0.0.136 — 2026-11-15

**`nocopy struct`: values that must never be copied.**

- New declaration modifier `nocopy`, only valid on `struct` (a `class` is a heap handle — copying it only
  retains a reference, so it needs no opt-out; `union` is copied by bits; `class`/`interface`/`enum`/
  `unit`/`extern` reject it). `Type::copyable()` returns false for the type and, because the predicate is
  recursive (0.0.133), for every struct that contains it: copying, passing or returning by value, a
  sum/union payload and copying an array of it are all compile errors, and the type cannot cross the C
  boundary (C copies by value). Borrowing, pointer access, field assignment and aggregate initialization
  are unaffected.
- Motivation: a struct that owns a resource (file descriptor, socket, lock) must not be duplicated by an
  implicit copy, and a struct *without* a user destructor is copyable by definition — before this there was
  no way to say "this one must not be".
- **Behaviour change**: `ref` / `ref const` parameters no longer reject an uncopyable struct. A borrow does
  not copy, so the blanket "not copyable → only by pointer" rule was over-strict for the borrowing form; it
  now only applies to by-value parameters. The borrow still has to satisfy the existing borrowable-type
  rule, so a struct with a destructor or with reference fields is still not borrowable.
- New run case `nocopy_use` (initialization, field assignment, borrowing, pointer access, field of a host)
  and 3 must-fail cases (`nocopy_copy`, `nocopy_class`, `nocopy_union`). SPEC 3.13 documents the modifier.

#### 0.0.135 — 2026-11-15

**A struct with a user destructor can be a field and a fixed-size array element (W3a).**

- The bans on "struct with a destructor as a struct field / class field / fixed-size array element" are
  gone. Both halves were already in place: construction recurses (W2, 0.0.134) and destruction recurses
  through fields and elements (`emitReleaseValue` → `dtor_cname`), so this only removes the sema refusals —
  plus the recursion that keeps it sound.
- Underneath, the type property carries the safety: `copyable()` is recursive (0.0.133), so a struct that
  contains such a value is itself uncopyable. Copying the host (`O b = a;` where `O { S s; }`) is refused by
  the same diagnostic that refuses copying `S`; likewise for a host holding an array of them. The
  extern/param/return bans and the dynamic-array-element ban still apply (they need copy construction and
  `new T[n]` element construction respectively).
- Diagnostics corrected: the copy/pass/return messages said the struct "has a destructor", which is
  misleading now that they also fire for a struct that merely *contains* one; they now say the value "is
  not copyable (it has a destructor, or contains a value that has one)".
- New run case `struct_dtor_nested` (field, static array, class field, nested static array, explicit
  `super(...)`, balanced constructor/destructor counts); `struct_dtor_field` / `struct_dtor_array` were
  re-pointed at what is still refused (copying the host) instead of the lifted bans. SPEC 3.4/3.13 updated.
- **Fixed in the same change**: with an explicit `super(...)` the member step ran twice (once inside the
  base-constructor statement, once at the end of the constructor), so every member with a constructor was
  built twice and the first one leaked. `ctor_nested` recorded it (18 constructors instead of 17).

#### 0.0.134 — 2026-11-15

**Implicit construction of members and array elements (W2 of the value-semantics plan).**

- The compiler never ran a member's or an element's constructor: `struct O { S s; }` with `S` having a
  default constructor left `o.s.x` **uninitialized** (measured: a garbage value), `S[3] a;` likewise, and a
  `class` field of such a struct stayed zero. Only a local declared directly as `S` ran `S()`.
- New `emitCtorGlue()` recurses through fields and array elements and calls the user's parameterless
  constructor wherever one exists; a value without a constructor emits nothing, so POD values stay
  free (`int[1000] a;` is still not touched). Zeroing remains the job of the `needs_cleanup` paths, so
  releasing a partially constructed value stays a no-op.
- Wiring: local declarations (also fixes class/delegate/slice locals, which used to keep a garbage
  handle when declared without an initializer), aggregate initializers (`{}` now value-initializes:
  zero then the member constructors; the elements an array initializer omits are constructed too),
  class constructors (C++ order: base constructor, members in declaration order, body - the explicit
  `super(...)` form runs the member step itself), classes with no constructor of their own (the step
  runs at the `new` site), and struct constructors.
- `Type::has_default_ctor()`/`needs_ctor()` were added; `needs_ctor()` is the constructor-side dual of
  `needs_dtor()`. A struct whose constructors are all parameterless-free is not implicitly constructible,
  which is why the glue checks for a parameterless overload instead of `!ctors.empty()`.
- Cleanup on a throwing member constructor still relies on the zero-safe release (W0/W1); per-prefix
  destruction becomes necessary only once a type with a user destructor can be a field or an element
  (W3). New run case `ctor_nested` (field, static 2-D array, aggregate, `{}`, class member ordering,
  class without a constructor, partially initialized array).

#### 0.0.133 — 2026-11-15

**Value-semantics predicate family on `Type` (foundation for struct/class/array nesting).**

- `Type` gains five recursive predicates: `needs_dtor()` (what `needs_cleanup()` meant: an RC handle or a
  user destructor anywhere in the value), `trivially_copyable()` (a bitwise copy is correct — the C
  boundary condition), `is_pod()` (trivially copyable **and** no user constructor, so construction is a
  `memset`), `copyable()` and `has_user_ctor()`. `needs_cleanup()` stays as the alias the codegen already
  calls. They replace four partially overlapping ad-hoc judgements (`needs_cleanup`, `has_ref_fields`,
  `borrowable_type`, `noncopyable_struct`), which is what forced a special case at every position a value
  could appear.
- `Sema::noncopyable_struct()` now reads `!t->copyable()`, so **uncopyability is recursive**: a struct
  that contains an uncopyable struct is uncopyable itself. Nothing observable changes yet, because a
  struct with a user destructor is still rejected before it can become a field or an array element; the
  recursion is what makes lifting those bans safe (backlog `agents/value-semantics.md` §2, W1).
- `POD` and "can cross the C boundary" are deliberately **two different predicates** (§1.1): a struct
  with a user constructor but POD fields is not POD yet is still trivially copyable, so `extern` /
  `export` keep accepting it — the same split C++ makes.
- Behaviour is unchanged: the full suite (576 cases) passes before and after, and no existing case or
  expected output was modified.

#### 0.0.132 — 2026-11-15

**A constructor that throws no longer runs its own destructor.**

- `new C(...)` / `new C` / `S(...)` / `S s;` freed a failed object through the ordinary RC destroy
  entry, which calls the class's full destructor chain — including the **user destructor body of an
  object that was never constructed**. C++ [except.ctor]/2 says the opposite: only the subobjects
  that are already fully constructed are destroyed. The observable damage was user code running on a
  half-built object (`exception_ctor` recorded it: `live` went back to 0), and, once member
  construction lands (W2), releases of fields that were never stored.
- New per-class/struct glue (internal linkage): the **partial destructor** releases every field in
  reverse order (own fields only for a class, `weak` fields are just unregistered) and **never runs
  the object's own body**; a class additionally calls its **base class's full destructor**, because
  reaching that level means the base subobject is complete. The **partial destroy** entry does
  `weak_notify` + partial destructor + `free` (aligned allocator for over-aligned classes), mirroring
  the ordinary destroy entry. Symbol names: `mu_<unit>_D_partial_<type>` and
  `mu_<unit>_destroy_partial_<type>`.
- The partial destroy entry dispatches on the object **descriptor**, which a constructor writes on
  entry and restores only after `super(...)` returns: it therefore names the deepest constructor
  frame that was running. Releasing exactly that level is what makes both directions correct — base
  constructed + derived body threw runs the base destructor exactly once, while a base constructor
  that throws itself runs no destructor at all (neither the base's nor the derived's). This is why
  the `super(...)` call sites need no cleanup of their own.
- Normal paths are untouched: `releaseLocal`, the scope/statement-temp releases, the class
  destructor and the destroy entry keep releasing through the full chain.
- New run case `ctor_throw_partial`: class ctor body throwing (100 rounds, storage really freed),
  `S s;` and `S s = S(...)` on a struct with a default ctor, base constructed + derived body throwing
  (base destructor exactly once), base ctor throwing (no destructor), and the normal path still
  running the destructor once. `exception_ctor` now expects `caught 1` — its old expectation encoded
  the bug (the destructor decrementing the live-object counter after a failed constructor).
- Tests: 576/576 Windows (UCRT64).

#### 0.0.131 — 2026-11-15

**A `weak` field survives generic instantiation.**

- `sema_generic.cpp` rebuilt an instantiated class/struct's field table with `fld.weak = false`, so a
  `weak` field declared in a generic template silently became an owning reference: the target was
  retained by the holder, was never automatically nulled on destruction, and the expected RC
  behaviour (`weak` counts nothing) was gone — no diagnostic, just wrong lifetimes. The instantiation
  now carries `TypeSpec::weak` over to `Field::weak` and applies the same two rules as the
  non-generic path (`resolve_class_fields`): `weak` on a struct field and `weak` on a non-class type
  are rejected with the existing messages.
- New run case `generic_weak`: `class Holder<T> { public weak Node peer; ... }` — the weak field reads
  back, does not keep the target alive, is nulled when the target dies, and its registration is
  dropped when the holder dies first. New must-fail case `generic_weak_struct` (`weak` in a generic
  `struct`).
- Tests: 575/575 Windows (UCRT64).

#### 0.0.130 — 2026-11-15

**Weak closure captures: `[weak this]` / `[weak x]`.**

- A callback a `class` keeps on itself (`this.cb = [this] …`) is an RC cycle: the field owns the
  environment, the environment owns the object, the count never reaches zero and **no destructor
  ever runs** — not only a leak, but a file descriptor / lock / socket that is never released.
  `[this]` stays owning by default (a delegate returned by a method is *supposed* to keep its
  receiver alive); the new `weak` capture is the opt-in that breaks the cycle, mirroring the rule
  for classes that hold each other.
- Semantics: the environment stores an uncounted reference (a `weak` field in the synthesized
  environment class, registered with the existing weak registry), so the target's destroy clears the
  slot. The lambda prologue tests every weak slot and, when one is null, **returns the zero value of
  the return type without running the body** — a dead receiver means there is nothing to notify
  (the Qt / event-system semantics), and it also means every weak capture is non-null inside the
  body, so no nullability or per-use checks are needed there. The test is a snapshot; a concurrent
  destruction between it and a later use is a data race, same as reading a `weak` field.
- Restricted to `class` handles: a `struct` `this` is a value copy (no cycle to break) and an
  `interface` value has no `weak` form yet (the weak registry writes one word). `[weak ref x]` is
  rejected (the modifiers contradict each other). The stack environment of an immediately-invoked
  lambda unregisters its weak slots when it dies, so the registry never keeps a pointer into a
  dead frame.
- New run cases `delegate_weak_self` (self-held callback: the destructor now runs), `delegate_weak_skip`
  (delegate outlives the receiver: the call yields the zero value and the body does not run),
  `delegate_weak_local` (`[weak x]` on a local handle and on a parameter, a null capture, and the
  snapshot rule — reassigning the variable does not retarget the capture), `delegate_weak_inline`
  (the stack environment's registration is dropped) and `delegate_weak_returns` (the skipped call
  yields a `void`, a null class handle and a zeroed struct); 5 must-fail cases
  (`[weak this]` in a struct, `[weak n]` on an `int`, `[weak i]` on an `interface`, and `[weak ref x]`
  in both modifier orders).
- Backlog U41 records the remaining step: a compile-time warning for a provable self-cycle
  (`capture == the assignment target's field-chain root`) and a debug-mode report of objects whose
  destructor never ran; automatic weakening and interprocedural ownership analysis are explicitly out.
- Tests: 573/573 Windows (UCRT64).

#### 0.0.129 — 2026-11-15

**`>>` on an unsigned type is a logical shift (issue #17).**

- `codegen_expr.cpp` lowered every `>>` to `AShr`, so any value with the top bit of its declared width
  set came back sign-extended (`(uint)2332836374 >> 16` gave `4294937356`). It now selects `LShr` when
  the result type is an unsigned prim; the shift stays at the declared width (`byte >> 1` is an i8
  shift), so the instruction follows the result type, not a promoted operand.
- `codegen_stmt.cpp`: `>>=` shared the same unconditional `AShr`; it now uses the signedness already
  computed for `/` and `%`.
- `sema_const.cpp`: the constant folder deliberately matched the wrong instruction (sign-extend, then
  shift arithmetically), so it was consistently wrong too; it now zero-extends for unsigned types, and
  compile-time and run-time agree again.
- **This is a language change, not only a bug fix**: SPEC §5 said "`>>` is always an arithmetic right
  shift ... unsigned types are also interpreted as two's complement" (`uint 0x80000000 >> 4` =
  `0xF8000000`); the rule now reads "`>>` follows the operand's signedness", which is C's behaviour.
  `unsigned_wrap` and `const_member` had pinned the old value and now assert `0x08000000` / `134217728`.
- New run case `shift_unsigned`: every unsigned width with the top bit set, `>>=` on `uint` / `ulong`
  and on a signed type, and the same expressions folded at compile time.
- Tests: 563/563 Windows (UCRT64), 561/561 Linux with LLVM 21 and LLVM 18.

#### 0.0.128 — 2026-11-15

**Linux is a supported host: the runtime platform layer, the glibc binding layer, and a Linux CI job.**

- `libs/std/platform/linux/std/thread/backend.mu` — the pthread/ucontext backend behind `std.task`:
  threads, sleep / monotonic clock, task fibers (`getcontext` / `makecontext` / `swapcontext`),
  `pthread_key` TLS, `get_nprocs`, and `pthread_mutex_t` / `pthread_cond_t` locks. `loader.cpp` loads
  `std.task` for every program, so without it not a single Linux program could even be checked.
- `std.thread`'s `Mutex` / `CondVar` are now 64-byte opaque blobs (SRWLOCK is 8 bytes, `pthread_mutex_t`
  40 and `pthread_cond_t` 48 on 64-bit Linux); both backends accept zeroed memory, so a lock field still
  needs no init call. `std.task` uses those types instead of raw `usize` globals — the old form silently
  overran the neighbouring globals on POSIX (`para` crashed at random).
- `libs/cstd/platform/linux/` — the glibc binding layer (stdio, stdlib, string, math, time, locale,
  fenv, errno, ctype, stdint, limits, floats, inttypes): LP64 widths, glibc's `FE_*` / `LC_*` / `errno`
  values, 64-bit `fpos_t` / `tm`, and the POSIX `_r` / `casecmp` variants in place of the MSVC
  extensions. glibc's standard streams are data symbols, so they live in `cstd.streams` while
  `cstd.stdio` keeps the portable `stdin()` / `stdout()` / `stderr()` accessors (a second `FILE` over
  fd 1 would split the output buffering).
- `muc build` appends `-lm -lpthread` when the target is not Windows (POSIX keeps the math and thread
  entry points in their own libraries), and `--run` decodes the POSIX wait status instead of forwarding
  the raw `system()` value.
- SysV integer aggregates: `abi_class` coerces a 9..16-byte non-float aggregate through two INTEGER
  eightbytes instead of handing C two field registers, which fixes calls like `c_eight(Two)` that
  silently read garbage. The coercion slot is sized for the register pair (a 12-byte aggregate is
  stored as two i64, and its alignment comes from the fields — a 6-byte aggregate has align 2, and
  `size` was never a valid alignment). Float-bearing aggregates and packed aggregates whose field is
  unaligned or straddles an eightbyte (class MEMORY) are refused on SysV, so `abi_agg` / `abi_float`
  are fenced to Windows; `abi_mix` gained 12- and 6-byte shapes round-tripped through a real C
  compiler both ways.
- The test runner gained `@platform <os>` (fences a case to hosts where its subject exists:
  `cstd_windows`, `cstd_posix`, `abi_agg`, `abi_float`) and per-OS expectations (`<name>.<os>.txt`,
  `tests/ir/abi_surface.linux.txt`) for values that really differ (`BUFSIZ`, the SysV IR shape).
  `cstd_core` / `cstd_stdio` / `cstdio` are portable now, with `cstd_windows` / `cstd_posix` covering
  the platform-only symbols.
- LLVM 18 builds: the DI size/offset operands and the `Triple`-typed target APIs are gated on
  `LLVM_VERSION_MAJOR` (19 changed both; the local toolchain is 21, MSYS2 ships 23), so the new
  `build-test-linux` job on `ubuntu-24.04` (llvm-dev 18) is green.
- Tests: 562/562 on Windows (UCRT64), 560/560 on Linux with LLVM 21 and with LLVM 18 (three
  Windows-only cases skipped, one POSIX-only case skipped on Windows).

#### 0.0.127 — 2026-11-15

**Cross-platform code generation: `--target`, all LLVM targets, and per-target runtime choices.**

- `--target <triple>` selects the LLVM triple (default `getDefaultTargetTriple()`); `InitializeAllTargets`
  / `AllTargetMCs` / `AllAsmPrinters` make every backend available. Verified object output:
  `x86_64-pc-linux-gnu` → ELF64 x86-64, `aarch64-unknown-linux-gnu` → ELF64 aarch64,
  `arm64-apple-darwin` → Mach-O arm64.
- Per-target runtime choices: over-aligned allocation uses `_aligned_malloc`/`_aligned_free` on Windows
  and `posix_memalign` + `free` elsewhere (checked in the emitted IR); the output suffix (`.exe`) and the
  default link driver (`gcc` / `cc` / `clang`) follow the target.
- Cross targets refuse to link without an explicit `--cc` (the object and IR are still written), and the
  produced file's magic is verified against the target so a host driver cannot silently "succeed" with
  the wrong format.
- SysV C-ABI aggregates containing `float`/`double` are refused at codegen with a clear message until
  the INTEGER/SSE eightbyte classification lands: the backend passes one register per field, which
  disagrees with C for e.g. `{float, float}`. (Windows coerces the whole aggregate through an integer,
  so the host path is unaffected.)
- `MUC_LIBS` now splits on `;` only on Windows (drive colons no longer truncate entries) and on `:` only
  on POSIX. macOS gets `_NSGetExecutablePath` for the library search (no `/proc/self/exe` there).
- Unit resolution verifies the on-disk spelling of every path segment: `unit foo.bar;` that only
  matches `libs/Foo/bar.mu` through Windows' case-insensitivity is now an error, instead of failing on
  Linux later.
- Tests: 561/561 on both the UCRT64 and the MSVCRT builds (the host path is unchanged).

#### 0.0.126 — 2026-11-15

**`muc run a.mu` — compile, link and run in one command.**

- `run` is `build … --run` without the flag: same pipeline, same options (`-o`, `--cc`, `-g`,
  `--link`, `--platform`, …), and the program's exit code is returned. `muc run` without an entry
  prints the usage and exits with 2.
- Also flushes the compiler's own output before spawning the linker and the program, so `+ gcc …`
  and `ok: …` appear in the right order instead of after the program's output.
- Tests: 561/561 on both the UCRT64 and the MSVCRT builds (`run` is sugar over the covered `build --run`).

#### 0.0.125 — 2026-11-15

**Loader: a platform shadow may drop the unit segments its directory already represents.**

- `libs/cstd/platform/windows/stdio.mu` now resolves as `cstd.stdio`: the shadow directory already
  carries the library segment, so repeating it in the file name was noise. The full name
  (`cstd.stdio.mu`) is still accepted, and having both is an error (`ambiguous unit`). Applies at
  every shadow level (`libs/std/platform/windows/thread/backend.mu` = `std.thread.backend`, etc.).
- The `cstd` units were renamed to the short form accordingly; no unit name changed.
- Also documents the `--platform <os>[-<arch>[-<abi>]]` key chain in SPEC 2.1 (most specific shadow
  first: `os-arch-abi`, `os-arch`, `os`).
- Tests: 561/561 on both the UCRT64 and the MSVCRT builds.

#### 0.0.124 — 2026-11-15

**`cstd`: the C standard library bindings land as a separate library (Windows/UCRT first).**

- New library `cstd` under `libs/cstd/platform/windows/`, unit names `cstd.<header>`: `stdio`, `stdlib`,
  `string`, `ctype`, `math`, `time`, `locale`, `errno`, `limits`, `floats`, `stdint`, `inttypes`, `fenv`.
  Raw declarations only (C semantics, no checks, no exceptions, no ownership).
- **Loader**: `--platform os[-arch[-abi]]` (e.g. `windows-x86_64-ucrt`), resolving shadow directories in
  the order `os-arch-abi → os-arch → os`, with flat file names (`cstd.stdio.mu`). `--os` still selects
  the OS axis alone; `build` still refuses a non-host OS.
- **Macro globals as functions**: `stdin()/stdout()/stderr()` call `__acrt_iob_func`, `errno` is
  `cstd.errno.current()` over `_errno()` — no data-symbol import, and `dllimport` is irrelevant for them.
- **Deliberately not bound**: `va_list` families, `setjmp`, the wide-character API, `complex` (needs a
  2×double convention), `uchar.h` (`mbstate_t` layout, UCRT-only), `signal.h`. `timespec_get` is kept
  out of the shared Windows unit so the same file links against both CRTs.
- **Language limits made explicit**: `realloc`/`free`/`alloc` are builtins and not redeclared; `strtold`
  is absent (no `long double`); `const char*` does not implicitly convert to `const void*`, so literals
  passed to `memcpy`/`fwrite` need `as const void*`.
- **Tests**: `cstd_stdio` and `cstd_core` take the address of **every** non-variadic declaration (link
  coverage) and exercise file I/O, strings, sorting/searching, math, the time round-trip, locale, errno,
  fenv and inttypes. 561/561 on both the UCRT64 and the MSVCRT builds.

#### 0.0.123 — 2026-11-15

**Windows development moves to the UCRT toolchain; absurd nesting is now rejected by a sane cap instead of a stack overflow.**

- **Why**: the standard-library plan targets the Universal CRT (it is the CRT MSVC/Rust/Python use, it is
  serviced with Windows, and it carries the C11 pieces our baseline whitelists). CI already built with
  MSYS2 UCRT64; local builds used the MinGW/MSVCRT toolchain. Switching exposed a latent bug:
  `deep_nesting` (3000 nested parentheses) crashed with a stack overflow before the parser's limit was
  reached — the old cap (3072 guard levels ≈ 1000 parentheses) was far above anything real code needs,
  and the UCRT/GCC 16.2 build spends ~2.7 KB of stack per level.
- **Change**: `kMaxNestDepth` goes from 3072 to **320 guard levels**, which is about **100 nested
  parentheses** (a parenthesised subexpression costs 3 levels: expression → unary → primary). That is
  ~0.9 MB of frames, so the executable keeps its 8 MB stack reserve unchanged. Hand-written code
  rarely exceeds ~10 levels; `deep_parens` now locks 100 nested parentheses as still accepted, and
  `deep_nesting` (3000) gets the diagnostic instead of a crash. The loop-built chain cap
  (`kMaxChainLinks = 2048`) is unchanged.
- **Also**: CMake probes `ucrt64`'s LLVM first; the README quick start installs the UCRT64 toolchain.
  Produced executables import `KERNEL32.dll` plus a handful of `api-ms-win-crt-*.dll` names — API sets
  resolved by the OS, so a program is still distributed as a single file.
- **Tests**: 559/559 on both the UCRT64 build and the MinGW/MSVCRT build.

#### 0.0.122 — 2026-11-14

**Correctness sweep from the review of 0.0.120: silent generic miscompiles, a heap out-of-bounds, a
compiler crash and a duplicated-output abort.**

- **The generic instantiator clones the whole AST** (`src/ast_clone.cpp`, `src/ast_subst.cpp`; issues
  #7, #8): `ParaStmt` / `ParaForStmt`, `IsExpr`, `SwitchArm::alt`/`binds`, `Lambda::captures` and
  `Member::type_args` were missing from the clone table, and the substitution pass skipped `IsExpr`,
  `para` and `Call::type_args`. A `para` inside a generic function **silently disappeared** (the
  program computed the wrong result and exited 0); `is`, sum-type `switch` and capture-list lambdas
  inside a generic failed with misleading errors. An unhandled node kind now throws `CloneError`
  (`error: internal: AST node kind not handled by the generic instantiator (<kind>)`) instead of
  returning null and having the node dropped.
- **Negative dynamic-array length** (#6): `new int[-1]` wrapped the byte count (`-1 * 4 + 24 = 20`)
  while storing `-1` as a huge `usize`, so no bounds check could ever fire and the program got a
  heap buffer far smaller than its advertised length. A constant negative length is now a compile
  error; a computed one aborts (SPEC 6.6) after a per-dimension sign check and a wrap check on the
  final byte count.
- **Deeply nested input** (#9): the parser had no recursion guard, so ~500 nested parentheses killed
  the compiler with a stack overflow and no diagnostic. It now counts nesting depth (parentheses,
  unary chains, blocks, brace initializers, type suffixes, `if`) and chain links, and reports
  `expression nesting too deep (limit 1024 levels)` or `... is too long (limit 2048 ...)`. `muc`
  also reserves 8 MB of main-thread stack, so 1000 levels of parentheses still compile; the measured
  overflow points are ~2000 levels and 5k-10k chain links, leaving ~2x margin.
- **Abnormal termination is deterministic** (#12, and the CI failure it exposed): generated code used
  C `abort()`, whose process status depends on the CRT — msvcrt exits 3, while UCRT reports
  `STATUS_STACK_BUFFER_OVERRUN` (0xC0000409) through a fail-fast — and whose path flushed the shared
  stdio buffer twice, printing buffered output a second time. Every "execution cannot continue" path
  (uncaught exception, failed allocation, an exception escaping a task body) now calls one internal
  `mu_abort` helper: `fflush(null)` then `_exit(3)`. Output appears exactly once and the process
  status is 3 on both the mingw64 and the ucrt64 CRT.
- **`Type::is_signed()`** (#16): documented as an integer-only predicate (floats and non-primitive
  kinds answer true, matching the prototype); the float → integer conversion in codegen is now an
  unconditional `FPToSI` instead of reading a sign off that fallback, and `widens_int`'s three
  identical branches collapsed to one.
- **Test runner** (#13): the expected file of a run case may carry `@exit <code>` (default 0, so an
  `abort()` case can assert 3) and `@ <args>` (extra compiler options, e.g. `--bounds=off`).
- **New tests**: `generic_para`, `generic_para_for`, `generic_is`, `generic_sum_switch`,
  `generic_capture_lambda` (each monomorphized with two type arguments), `neg_len_runtime`,
  `alloc_overflow`, `para_throw_section`, `para_for_throw_body`, `para_catch_inside`, `bounds_off`,
  `float_arith`, `unsigned_wrap`, `deep_recursion`, `deep_parens`, `string_compare`, must-fail
  `neg_len`, `deep_nesting`.
- **Docs**: the README status table lists explicit capture-list lambdas (#10); SPEC 3.17.2 now states
  the rule the implementation follows (a section / task-body exception aborts the process, a
  producer-side exception joins the region cleanly) instead of the stale "rethrown in the enclosing
  scope" (#11); SPEC 6.4 spells out the non-negative / non-overflowing length rule;
  `tests/README.md` documents all five case kinds and the marker lines (#15).

**Tests**: 559/559 (182 pass + 350 must-fail + 20 check + 4 ir + 3 c abi)

#### 0.0.121 — 2026-11-14

**C ABI for aggregates: `extern` and `export` follow the platform convention (B2).**

- **Why**: an `extern` C function taking a 16-byte struct by value crashed — Mu passed it in
  RCX/RDX while C expects RCX to be *a pointer to a caller-made copy*. `export` had the same problem
  from the other side, so an exported symbol was not callable from C at all.
- **Rule**: `extern` **and** `export` use the C ABI; every other (internal) call keeps the backend's
  own convention, which is faster and unaffected. A symbol has one convention, so the Mu-side call
  sites of an `export`ed function use the C ABI too (documented: do not `export` hot helpers).
- **Classifier** (`src/abi.cpp`): Win64 — a 1/2/4/8-byte aggregate is passed and returned *as an
  integer of the same size* (measured against gcc: `{float,float}`, `{double}` and `{int,int}` all
  go in integer registers, never XMM), everything else by reference (`byval`) with the result
  through a hidden pointer (`sret`); over-aligned aggregates always by reference. SysV keeps the
  16-byte register rule (per-eightbyte SSE classification is still open).
- **One signature builder** (`src/codegen_abi.cpp`): `cAbiSig`/`cAbiCallee` are used by the
  declaration *and* every call site, with `byval(%T) align N` / `sret(%T) noalias align N`
  attributes; the call site carries the same attributes because LLVM only lowers an aggregate per the
  C ABI when the *call* says so.
- **Packed eightbytes**: LLVM hands each aggregate field its own register (`{i32,i32}` came back in
  RAX *and* RDX, while C packs both halves into RAX), so such values are reinterpreted through a
  stack slot as an `i32`/`i64` (`CoerceInt`). The callee unpacks, the caller repacks.
- **`export` signature rules**: exported free functions now obey the same restrictions as `extern`
  (no class/interface handle, `delegate`, `T[]`, sum type — also when nested in a struct or array —
  and no `ref` parameter), because a DLL consumer is C.
- **Function values**: a `c_abi` function whose aggregates travel differently cannot be taken as a
  function value (a value carries only the Mu signature) — reported instead of miscompiled.
- **`muc build --link <obj>`** (repeatable): extra object files for the link step, so a program can be
  built against C objects compiled by the same driver.
- Tests: `tests/abi/{abi_agg,abi_mix,abi_float}` (a C shim compiled by gcc is linked in and calls
  back into the exported functions: 16/24/8/4/3-byte and packed structs, unions, array fields, nested
  structs, float-only and mixed aggregates, sret + byval + scalars in one call), `tests/ir/abi_surface`
  (the exported symbol shapes), `tests/cases/export_abi_mutual` (internal and exported functions calling
  each other in both directions, across units and within one), must-fail `export_slice`,
  `export_class_param`, `export_ref_param`, `extern_array_return` (C has no array results).

**Tests**: 541/541 (166 pass + 348 must-fail + 20 check + 4 ir + 3 c abi)

> **Documentation restructure (no code change, no version bump)**: the standard library was split out of
> the language documents. `docs/` now holds only user-facing files: `SPEC.md` (language) and
> `StdLibSPEC.md` (standard library: layers, unit inventory, error model, public contracts). All design
> material moved to `agents/stdlib/` (`policy.md`, `c-bindings.md`, `interfaces.md`, `style.md`,
> `task.md`, `backlog.md`, `string.md`, `task-runtime.md`), and `agents/design-backlog.md` keeps
> language items only. Section numbers in `docs/SPEC.md` were kept on purpose because code comments and
> tests cite them; paths named in older entries of this file are historical.

#### 0.0.120 — 2026-11-14

**Sum types never cross into C; `para`/`thread` treat them as ordinary values (U42 S5).**

- **`extern` rejection**: a sum type in a parameter or return position — directly, as an array/slice
  element, or as a field of a struct that appears there — is a compile error: "its tag+payload layout
  is an implementation detail, not a public interface (pass a plain `struct { tag; union }` and
  convert)". This follows the same rule as class/interface/`T[]`: representation details stay inside
  the language.
- **`para` / `thread`**: no special handling. A sum is an ordinary value type — a section may read and
  copy it, a delegate may capture it by value, and a reference-counted payload follows the usual RC
  rules. Documented in SPEC 10.1.1; verified by a test that reads sums inside a section and captures
  one in a delegate.
- Tests: `sum_para` (sum read in a `para` section, `is` inside a section, sum captured by a delegate),
  must-fail `sum_extern_param` and `sum_extern_field` (also covers a struct containing a sum).

**Tests**: 532/532 (165 pass + 344 must-fail + 20 check + 3 ir)

#### 0.0.119 — 2026-11-14

**Sum-type ergonomics: `is` tests, contextual construction, value equality (U42 S4).**

- **`is`**: `if o is Some { ... }`, `if o is Some(v) { ... }` (payload binding), and on a class or
  interface value `if a is Cat c { ... }` (subtype test with the downcast bound). `is` is a contextual
  keyword (an ordinary identifier elsewhere). The binding is a local of the then-branch only, and it
  is allowed only when the `is` is the whole condition (so `and`/`or` compose without bindings).
  A scrutinee with no runtime discrimination (`int`, `struct`, `enum`, pointer, slice) is a compile
  error — the test would be a constant.
- **Contextual construction**: with the target type known, the alternative name alone works —
  `Option<int> o = Some(3);`, `o = None;`, `if o == None { ... }`. A user function of the same name
  wins over the alternative, so nothing existing changes.
- **Value equality**: `==` / `!=` on a sum compare the tag, then the active payload field by field
  (floats with `fcmp oeq`). Only payload types with value equality participate; a class/interface/
  `String`/slice/struct payload reports "no value equality (read it with `switch`)".
- Implementation notes: the binding locals come from `FuncDecl::synthetic_locals` (they must outlive
  Sema); the sum comparison computes the payload addresses **before** creating the switch block — a
  GEP inserted after the switch left the block unterminated and produced invalid IR until fixed.
- Tests: `sum_ergonomics` (`is` with and without binding, class test with binding, contextual
  construction in declarations and comparisons, single- and multi-field equality, `!=`), must-fail
  `sum_is_static`.

**Tests**: 529/529 (164 pass + 342 must-fail + 20 check + 3 ir)

#### 0.0.118 — 2026-11-14

**`match` merged into `switch`: one dispatch construct.**

- `switch` now decides its label domain from the scrutinee's type: integers (literals, several values,
  `..=` ranges, `else` required), enums (member labels, exhaustive), and **sum types**
  (`Some(v):` / `None:`, exhaustive, payload bindings scoped to the arm). The separate `match` keyword
  is gone — it shipped one build earlier and was folded in before it spread, so there is exactly one
  construct for "dispatch on a value" (the same "one concept, one construct" rule that removed
  `length`/`size`/`count` synonyms).
- A bare name label is parsed as a value pattern and reinterpreted as an alternative when the
  scrutinee is a sum, so `Red:` (an enum member) and `Some:` (a unit alternative) stay unambiguous
  without lookahead into the scrutinee type.
- A sum `switch` counts as a terminating path when every arm returns; a missing alternative without
  `else` is still a compile error naming it.
- Tests migrated: `sum_match` (renamed behaviour: same cases, `switch` labels), `sum_match_missing`,
  `sum_match_arity`, `switch_float` / `switch_ptr_scrut` (the diagnostic now mentions sum types).

**Tests**: 527/527 (163 pass + 341 must-fail + 20 check + 3 ir)

#### 0.0.117 — 2026-11-14

**Generic sums (U42 S3): `Option<T>` / `Result<T, E>` instantiate like a generic struct.**

- `type Option<T> = Some(T) | None;` takes type parameters; `Option<int>` and `Option<const char*>`
  are distinct instances with their own layout and alternatives (`sizeof` is 8 for the `int` payload,
  16 for a pointer). Alternatives, construction (`Option<int>.Some(3)`, `Option<int>.None`) and `match`
  all use the familiar forms.
- Generic instantiations, construction and alternative access now go through the parser's existing
  speculative `<...>` probe: `Option<int>.Some(3)` parses as a member call carrying the instantiation's
  type arguments, and `Option<int>.None` as a member carrying them too.
- `SumDecl` joins the generic machinery end to end: `index_templates`, `clone_decl` (it had no clone
  branch, which crashed the instantiator), `subst_decl` (alternative payload types were not
  substituted, which made `T` unresolvable), and the instantiation path itself.
- **Fixed along the way**: the instance label for a type argument with no `name` (pointer, slice,
  array, function type) was empty, so `Result<int, const char*>` and `Result<int, char*>` would have
  collapsed into one instance. Labels now fall back to the argument's textual form.
- Tests: `sum_generic` (`Option<int>`, `Option<const char*>`, `Result<int, const char*>`, an RC
  payload released at scope exit, per-instantiation `sizeof`).

**Tests**: 527/527 (163 pass + 341 must-fail + 20 check + 3 ir)

#### 0.0.116 — 2026-11-14

**Sum types, stage 2: `match` with bindings and exhaustiveness (SPEC 10.1.2).**

- `match v { Int(n): { ... } Pair(a, b): { ... } None: { ... } }` dispatches on the tag and binds the
  payload fields as ordinary locals (one per field, in declaration order). A reference-typed payload is
  retained for the arm and released when it ends.
- **Exhaustiveness**: without `else` every alternative must be covered (a missing one is a compile
  error naming it); `else` may appear once. Unknown alternatives, duplicates and a binding count that
  does not match the payload are errors. `match` also counts as a terminating path for the
  return-value check.
- `match` is a **contextual keyword** (`match x { ... }` starts a statement; a call to a function named
  `match` is unaffected).
- **Fixed**: sum construction retained an owned payload argument (`Holder.Obj(new Box(1))`) while
  ownership was already transferred, leaking one reference; it now retains only borrowed arguments
  (`convOwns`), the same rule every other store uses. Caught by a live-object counter: the value was
  never released at scope exit.
- Tests: `sum_match` (four-alternative dispatch, `else`, reference payload alive inside the arm and
  released after, sum temporary released at block exit), must-fail `sum_match_missing`,
  `sum_match_arity`, `sum_alt_unknown`.

**Tests**: 526/526 (162 pass + 341 must-fail + 20 check + 3 ir)

#### 0.0.115 — 2026-11-14

**Sum types, stage 1 (SPEC 10.1.1): declaration, layout, construction, value semantics.**

- **Declaration**: `type Value = Int(int) | Pair(int, int) | Text(const char*) | None;` — a `type`
  declaration whose right-hand side is two or more `|`-separated alternatives. Every other
  `type X = ...` stays a transparent alias, so nothing existing changes. `|` is the same operator
  the constraint syntax uses (`T: int | long`), so a sum's alternatives are literally the value set.
- **Layout**: the payload? The sum is `{ i32 tag; union payload }`: the payload area behaves like a
  `union` (all alternatives at the same offset, size = the largest, alignment = the largest), so
  `sizeof` is `align_up(8 + payload, align)`. A multi-field alternative gets an anonymous struct
  payload with fields `f0..fN`; an all-unit sum keeps a one-byte payload so the union stays non-empty.
- **Construction**: `Value.Int(3)` (payload alternative) and `Value.None` (unit alternative); the
  argument count and types are checked against the declaration, unknown/duplicate alternatives are
  errors, and a payload that is `void` or a struct with a user destructor is rejected.
- **Value semantics**: assignment/copy copies; the active payload is retained on copy and released on
  overwrite or scope exit — `emitRetainValue`/`emitReleaseValue` gained a `Sum` case that dispatches on
  the tag, so `Holder h = Holder.Obj(box)` keeps the object alive and releases it exactly once.
- **Not yet**: `match`/pattern dispatch (a sum can be built, stored, copied and destroyed but not
  inspected), `==` between sums, sums in `extern` signatures, and generic sums (`Option<T>`) — the
  next step builds on this layout.
- Tests: `sum_basic` (btw-alternative construction, `sizeof`, copy, retain/release on a class payload,
  overwrite, scope exit).

**Tests**: 522/522 (161 pass + 338 must-fail + 20 check + 3 ir)

#### 0.0.114 — 2026-11-14

**`sizeof` / `alignof` accept generic instantiations and anonymous delegate types.**

- `sizeof(IIterator<int>)`, `sizeof(Box<int>)` and `sizeof(delegate (int) => int)` now parse. An
  instantiation imported from another unit is not in the parser's name table, so the parenthesized
  operand is probed as a type shape (`Name<args>` or `(T) => R`) and backtracked on failure — the same
  speculative-parse trick the declaration lookahead uses.
- Expression operands are unchanged: `sizeof(a[0])`, `sizeof(arr)` and `sizeof(x < y)` still parse as
  values (`sizeof(x < y)` is 1: the probe fails on the missing `>`, so it stays an expression).
- Tests: `sizeof_generic` (imported generic interface, local generic, anonymous delegate, function-type
  alias, array element, array, comparison).

**Tests**: 521/521 (160 pass + 338 must-fail + 20 check + 3 ir)

#### 0.0.113 — 2026-11-14

**Closure optimisation step (U41 D5): an immediately-invoked lambda keeps its environment on the stack.**

- `([x] (int a) => a + x)(3)` no longer allocates: the environment is a stack object (header + zeroed
  fields), the captures are stored into it, the call passes its address as `env`, and the generated
  destructor runs right after the call to release the captured RC fields — so no allocation, no
  reference count traffic, and no leak. The capture-store loop is shared with the escaping path
  (`Emitter::emitCaptureInit`).
- **Fixed along the way**: a capture list could name a variable of an *outer function* (skipping the
  enclosing lambda). The name resolved, but codegen read a freshly allocated slot in the inner lambda,
  i.e. zero — `[x, y] () => ([y] …)()` silently lost `y`. A capture must now name a variable of the
  immediately enclosing function; deeper ones have to be captured by that lambda first.
- Still deferred (documented in U41 D5): stack allocation for a *stored* delegate whose only uses are
  calls (needs a scope-exit mechanism that destroys the stack environment's fields without freeing),
  and single-field environment specialisation.
- Tests: `delegate_inline` (scalar, RC string, class reference released after the call, two captures,
  nested immediate invocation), must-fail `delegate_capture_outer_fn`.

**Tests**: 520/520 (159 pass + 338 must-fail + 20 check + 3 ir)

#### 0.0.112 — 2026-11-14

**Closure finishing touches (U41 D4): per-iteration captures, `delegate == null`, `para` boundaries.**

- **`delegate == null` / `!= null`**: an empty delegate is the one whose entry point is null; the
  comparison reads the first word (`{code, env}`), so it costs a pointer compare. Two delegates are
  still not comparable with each other (like `fn`).
- **`for` bodies**: `[x]` captures a fresh copy per iteration (verified: three delegates from one loop
  see 0/1/2), while `[ref x]` shares the variable and every delegate sees the final value — documented
  as the classic loop-closure hazard instead of being special-cased.
- **`para` regions** (SPEC 3.17.2): a section may call a delegate captured by value before the region
  (the environment is immutable for it), but `[ref …]` and `[this]` captures **inside** a section are
  compile errors — a section must not write enclosing locals, and `this` is not usable there in v1.
- Tests: `delegate_loop` (per-iteration vs shared, null comparison, calling a captured delegate inside
  a `para` section through a shared class field), must-fail `delegate_para_ref`.

**Tests**: 518/518 (158 pass + 337 must-fail + 20 check + 3 ir)

#### 0.0.111 — 2026-11-14

**`[this]` capture (U41 D4): a class keeps the object alive, a struct copies it, `[ref this]` writes
through.**

- **Semantics**: `[this]` captures the receiver of the enclosing method. A **class** contributes its
  handle (the environment retains it, so the object outlives the delegate); a **struct** contributes a
  copy, so later changes to the original are invisible (value semantics). `[ref this]` on a **struct**
  stores the object's address — writes from the body reach the object the delegate was taken from
  (the out-parameter pattern, caller-owned lifetime); `[ref this]` on a **class** is rejected (the
  handle already is a reference).
- **The body keeps working as usual**: `this`, implicit field access and method calls resolve through
  the environment — sema rebinds the lambda's `this` to the captured type and codegen points
  `thisSlot` at the environment field (the handle for a class, the object address for a struct).
- **Also**: the "returning a `[ref ...]` lambda" guard now distinguishes frame-owned from
  caller-owned storage — `this` of a struct method, `ref` parameters and `for ref` bindings may
  escape (their lifetime is the caller's responsibility, like `&x`), plain locals may not.
- **Latent hazard found while implementing**: `Name::sym` for `this` points at a `LocalVar` owned by
  `Sema::this_local_`, which the next `begin_member` frees — codegen must never dereference it (it
  kept crashing with `std::bad_alloc` on garbage). `addrOf("this")` therefore relies on `thisSlot`
  alone, which the lambda path already points at the environment field.

Tests: `delegate_this` (class keep-alive, struct copy, struct `[ref this]` write-back, capture list
mixing `[ref this, by]`), must-fail `delegate_this_class_ref` and `delegate_this_outside`.

**Tests**: 516/516 (157 pass + 336 must-fail + 20 check + 3 ir)

#### 0.0.110 — 2026-11-14

**Capturing lambdas (U41 D2/D3): `[x]` copies into a synthesized environment, `[ref x]` writes back.**

- **Capture list** `[x, ref y] (…) => …`, parsed before the parameter list. **Explicit only**: a name
  the body uses from an enclosing scope must be listed, otherwise the diagnostic tells you to write
  `[x]` (copy) or `[ref x]` (writes reach the original). `[this]` is rejected for now (the struct-copy
  vs class-handle decision is still open); a capturing lambda's type is a `delegate`, a
  non-capturing one remains a plain `fn`.
- **Environment object**: one synthesized `class` per capturing lambda, one field per capture, built by
  sema (fields + a shell `ClassDecl` appended to the unit) and handed to codegen, which allocates it
  (`rc = 1`, descriptor, zeroed fields), copies (or address-takes) each capture and stores `{code, env}`.
  Layout, descriptor and destructor come from the ordinary class machinery, so an RC capture is
  retained by the environment and released when the delegate dies.
- **`[ref x]`**: stores the captured variable's address and binds the name as a `by_ref` local inside
  the body, so reads and writes go straight through the pointer — the out-parameter pattern
  (`void fill(ref int out) { Acc f = [ref out] (int v) => { out = v; }; … }`) works with caller-owned
  storage. Only borrowable types qualify (same rule as `ref` parameters); the caller owns the
  lifetime and an escaping delegate is UB (documented).
- **Statically rejected**: duplicate captures, a non-local name, `[ref …]` of a reference-counted type,
  by-value capture of a struct with a destructor, and `return [ref x] …` (the variable always dies
  with the call — the one escape the compiler can see).
- **Codegen**: `Emitter::emitClassAlloc` (shared with `new`), `slotFor` resolves a captured local to
  `env->field` while a lambda body is emitted (`lambdaEnvIndex` / `lambdaEnvArg` / `lambdaEnvTy`),
  `valueOwned` treats a lambda as an owned value, and delegate assignments now release the old
  environment (the value-assignment cleanup path learned `TypeKind::Delegate`) — that omission was
  caught by a live-object counter in the new test.
- **Destructor fix**: the generated class destructor derived "own vs base fields" from the AST field
  list, which is empty for a synthesized environment, so `baseCount` swallowed every field and RC
  captures leaked. It now uses the base class's field count (0 when there is no base).

Tests: `delegate_capture` (copy vs write-back, RC-string capture, out-parameter, escaping by value,
nested capture, class capture keeping the object alive then releasing it, cursor implementing
`IEnumerable<T>`), must-fail `delegate_capture_ref_rc` (`[ref]` of a `String`) and
`delegate_capture_return_ref` (returning a `[ref]` lambda); `fn_capture`'s expected message follows
the new wording.

**Tests**: 513/513 (156 pass + 334 must-fail + 20 check + 3 ir)

#### 0.0.109 — 2026-11-14

**`delegate`: the two-word callable type (closures step D1; capture analysis still to come).**

- **New type** `delegate (T1, T2) => R`: two words `{code, env}`, `sizeof`/`alignof` = 16/8, distinct from
  `fn` (one word, C ABI — unchanged). `delegate` is a **contextual keyword**: `delegate (` starts a type,
  everywhere else it remains an identifier, so existing code that uses the word is unaffected.
- **Calling convention**: `d(args...)` calls `code(env, args...)`. The environment is the first hidden
  argument, so C code can be handed `{code, env}` as two `void*` and call `ret (*)(void*, ...)`.
- **Conversions**: `fn → delegate` is implicit and materializes `{thunk, null}` — the thunk drops the
  unused environment, so free functions and non-capturing lambdas work as delegates today.
  `delegate → fn` is a compile error; `null` converts to the empty delegate.
- **RC plumbing**: the environment is an RC object; copying a delegate retains it, destroying the value
  releases it, so delegates are ordinary values in fields, arrays, parameters and return values. With a
  null environment every retain/release is a no-op — the same code path a capturing closure will use.
- **`extern`**: a delegate is rejected in `extern` parameter and return position (C has no ABI for a pair
  whose environment must be released); pass `.code`/`.env` explicitly as `void*`.
- **What is still missing** (backlog U41 D2–D4): lambdas cannot capture yet — the capture list `[x]`,
  the capture analysis and the synthesized environment class are the next steps, so every environment is
  `null` in this build and `for-in`/`auto` inference still produce `fn` values.
- Files: `src/types.{hpp,cpp}` (kind, layout, `str`, `same_type`, conversion, cleanup), `src/mangle.cpp`
  (`dlg_…`), `src/parser_expr.cpp` (type syntax, `is_type_start`), `src/{parser_unit,sema_generic,
  sema_impl}.hpp/cpp` (canonical spelling), `src/sema_call.cpp` (`is_callable_value`, delegate callee),
  `src/sema_decl.cpp` (`extern` rejection), `src/codegen_{expr,expr2,support,func,runtime,class,impl}`
  (layout, thunk, indirect call, retain/release).

Tests: `delegate_basic` (fn→delegate, lambda→delegate, field/argument/return, `sizeof`), must-fail
`delegate_to_fn` and `delegate_extern`.

**Tests**: 510/510 (155 pass + 332 must-fail + 20 check + 3 ir)

#### 0.0.108 — 2026-11-14

**Generic interfaces are real interfaces: `IEnumerable<T>` works end to end; structs implement contracts
without `override`.**

- **An instantiated generic interface is an ordinary interface type** (`IIterator<int>`, `IEnumerable<int>`):
  it can be implemented by a `class` (itable), implemented by a **generic class**
  (`class Cursor<T> : IIterator<T>`, `class Src<T> : IEnumerable<T>`), used as a value type (16-byte
  `{obj, desc}`), a return type, a field/local type, a structural constraint (`T: IEnumerable<int>`)
  and an `as` downcast target. Slots are keyed by the concrete method signature, so `IIterator<int>` and
  `IIterator<string>` coexist. Nothing is boxed. This supersedes backlog U34 M5b ("closed") and the
  0.0.75 decision *only* for the generic-interface-as-value-type half; the value-type → interface half
  (boxing a struct) stays closed.
- **Bases are resolved from their structured spec, not the display string** (`src/sema_decl.cpp`,
  `src/sema_generic.cpp`): `resolve_named_type("IIterator<int>")` could never succeed, which is why
  `class C : IIterator<int>` reported "unknown type". Class bases, interface bases and instantiated
  generic bases now all go through `resolve_type`. `class Cursor<T> : IIterator<T>` also needed the
  generic-class guard removed (it rejected >1 base, i.e. class + interface) and the interface branch of
  `instantiate_generic` to resolve its own substituted base specs.
- **Generic interface inheritance** (`interface Derived<T> : Base<T>`) now works at instantiation:
  `subst_decl` substitutes interface base specs (it used to skip them) and the instantiated interface
  records its base interfaces, so a class implementing `Derived<int>` satisfies `Base<int>` methods too.
- **`for-in` accepts an interface cursor** (`src/sema_class2.cpp`, `src/codegen_stmt.cpp`): the cursor
  may be a struct (static dispatch, unchanged) or an interface value (`IIterator<T>`, the shape
  `IEnumerable<T>` returns), which dispatches `next()`/`current()` through the itable; the loop owns
  one RC-managed 16-byte local. `std.interfaces.IEnumerable<T>` is now the C# shape
  `IIterator<T> iterator()` instead of the earlier cursor-typed `IEnumerable<It>`.
- **`override` is optional for a struct** implementing an interface: the `:` list is the contract and a
  value type cannot be subclassed, so the marker distinguishes nothing. A `class` still requires it on
  every interface method (the must-fail case moved from
  `tests/errors/struct_iface_no_override` to `tests/check/struct_iface_no_override`).
- Docs: SPEC 3.10 (generic interfaces as real interfaces, struct/class `override` rule), 3.15.8 (which
  half of the 0.0.75 decision still stands), 3.15.9 (`IEnumerable<T>` row), 6.4.2 (interface cursors),
  §12 #110/#111 rewritten; backlog U34 M5b marked implemented.

Tests: `iface_generic` (class + generic class + generic interface inheritance + struct container with a
class cursor + constraint + downcast + `sizeof(interface)`), `struct_iface_no_override` (check), must-fail
`iface_generic_no_override` (a class still needs `override`).

**Tests**: 507/507 (154 pass + 330 must-fail + 20 check + 3 ir)

#### 0.0.107 — 2026-11-14

**`libs/core` folded into `std`: `core.iter` → `std.interfaces`, which also declares the built-in
capability contracts.**

- `libs/core/` held exactly one declarations-only file (`iter.mu`) and bought nothing: the standard
  library already has compiler-facing units (`std.string`, `std.exception`, `std.task`,
  `std.thread`). It is now `libs/std/interfaces.mu`, unit `std.interfaces`.
- **The unit is also the home of the built-in capability interfaces**: `IEquatable`, `IOrdered`
  (ordering + equality), `IAddable` / `ISubtractable` / `IMultipliable` / `IDivisible` /
  `IModulable`, `INegatable`, and the bundle `INumber : IAddable, ISubtractable, IMultipliable,
  IDivisible, INegatable, IOrdered`, next to the existing `IIterator<T>` / `IIndexable<T>` and the
  new `IEnumerable<It>` (the container half of the cursor form).
  They are ordinary `interface` declarations: constraints (`T: INumber`) are checked structurally
  and built-in numbers satisfy them through their operators, so generic numeric code costs nothing
  at runtime and needs no special-casing in the compiler.
  - Split per operator instead of one arithmetic interface: the constraint checker recognises only
    `+ - * / %` and the comparison operators as built-ins, so a bitwise contract would be
    unsatisfiable today.
  - `IEquatable` requires both `==` and `!=` (a user value type must declare both — the language
    does not derive one from the other); `class` equality stays identity, so a class does not
    satisfy it.
- Implicit loading (`src/loader.cpp`) resolves `std.interfaces`; the cursor-form diagnostics in
  `src/sema_class2.cpp` name `std.interfaces.IIterator`, and a user type may
  `use std.interfaces;` + `: IIterator<T>` exactly as before.
- No semantic change to `for-in`: the contracts are still checked structurally (no vtable, no
  boxing); the interface declarations document them and give the definition site a check.
- **Fixed along the way — cross-unit generic instantiation** (`src/sema_generic.cpp`,
  `src/ast_subst.*`, `src/ast.hpp`, `src/sema_types.cpp`): a generic type or function declared in one
  unit could not be instantiated with a type from another unit — the substituted type argument was
  re-resolved **in the template's unit**, which cannot see the client's names
  (`error: unknown type 'C'`). Reproduced with a generic interface in a library and
  `struct S : E<Counter>` at the use site; it would equally have broken `Array<T>` / `Vec<T>` for
  user element types. The argument type is now resolved at the **use site** and travels with the
  substitution (`TypeSpec::resolved`, honoured by `resolve_type`), and an instance name qualifies
  arguments that come from another unit (`Box<genx.lib.Item>`) so two units that each declare
  `Item` no longer collapse into one instance — previously the second unit silently got the first
  unit's type.
- Docs: SPEC 3.15.5 / new 3.15.9 (table of the predeclared contracts) / 6.4.2 (how `for-in` locates
  the iterator, and why `IEnumerable<T>` is not expressible) / 6.4.3, `docs/style.md` §6,
  `agents/task-runtime.md`, backlog U39's implicit-unit list, `agents/TODO.md` and the
  `cursor_basic` header comment.

Tests: `std_interfaces` (int / double / a user `Money` value type satisfy the arithmetic and ordering
contracts; a `Counter : IIterator<int>` / `Counts : IEnumerable<Counter>` pair iterates), `generic_xunit`
(a generic type from another unit instantiated with a client-side type, alongside the library's own
same-named type), must-fail `std_iface_missing` (a struct without `operator+`) and
`std_iface_enum_number` (an enum cannot satisfy the arithmetic bundle).

**Tests**: 505/505 (153 pass + 330 must-fail + 19 check + 3 ir)

#### 0.0.106 — 2026-11-14

**Static arrays follow C99: array-to-pointer decay, non-assignable array objects, `T[N]` parameters are `T*`.**

- **Decay** (`src/types.cpp:implicitly_convertible`, `src/codegen_expr.cpp:coerce`): an array in value
  position converts to a pointer to its first element — declaration initializer, assignment, argument,
  struct field (`int* q = s.v;`) and a row of a static multi-dimensional array (`int* r = m[1];`).
  The conversion is the address of the lvalue, so the pointer shares storage. `sizeof`/`alignof` never
  decay (`sizeof(a)` is still the array size). A pointer obtained from a **prvalue** array (a function
  result) is only valid until the end of the statement (SPEC 8.2).
- **Arrays are not assignable** (`src/sema_const.cpp:assignable`): `a = b;`, `int[N] b = a;` and
  `s.v = a;` are errors — "arrays are not assignable (C99); copy the elements or use `memcpy`".
  Two Mu extensions stay: an array-typed `return` still copies (array returns are a Mu value feature),
  and an array can be initialized from a **prvalue** array (there is no lvalue to alias).
- **Parameter adjustment** (C99 6.7.5.3p7, `Sema::decay_param_type`): `int[5] xs` declares `int* xs`
  — the bound is dropped, `sizeof(xs)` is the pointer size, writes reach the caller, and only the
  outermost dimension decays (`int[2][3] rows` is a pointer to `int[3]`). Applied to free functions,
  methods, constructors, interface methods, function types (`(int[3]) => void` is `(int*) => void`),
  lambdas and generic instantiations, so `int f(int[3])` and `int f(int*)` are the same signature.
  Arrays whose element is a `class`/`interface` keep value semantics (this language has no pointer to
  a reference, 3.8): the parameter still copies the handles, retaining per element. `ref T[N]` stays
  rejected with the existing diagnostic.
- **Migrated cases**: `arrays` (decay, `sizeof` without decay, write-through parameter), `multidim`
  (row decay; element-wise copies of static rectangles), `class_array_copy` and `class_iface_array`
  (element-wise copies with per-element retain/release). New: `array_decay` (run) plus must-fail
  `array_assign`, `array_copy_init`, `array_field_assign`.
- **Varargs decay**: an array passed to an `extern` `...` function decays to `T*` before the C default
  argument promotions (it used to be passed as an aggregate, which the C side could not consume).
- **Docs**: SPEC 3.3 / 3.6 / 7 / 7.1 / 8.2 / 11 and §12 row 5 rewritten (the old "no decay" decision is
  superseded); backlog U22 records the change. Variadic arguments decay to `T*` as well
  (C99 6.3.2.1), so `printf("%p", a)` passes the array address instead of copying the aggregate.

**Tests**: 501/501 (151 pass + 328 must-fail + 19 check + 3 ir)

> **Doc-only change (2026-11-14, no version bump)**: the dynamic-array spelling is settled —
> `T[]` / `T[,]` / `T[,,]` **stay** and are **syntax sugar for `Array<T>` / `Array2d<T>` /
> `Array3d<T>`** (this supersedes the earlier "no sugar / consider deleting `T[]`" decision in
> backlog U38), while `buffer<T>` is the compiler-provided **low-level safe block** the `Array`
> family is built on, not a user-facing container. Until `Array*` is written both spellings still
> resolve to the same built-in RC sequence, so no code changes. Written into SPEC §3.3 / §12 #6,
> `docs/style.md` §1 and backlog U38.

#### 0.0.105 — 2026-11-14

**GitHub issue batch: ODR duplicate, `buffer<T>.ptr`, multi-index `operator[]`, constant axis
check, doc reconciliation.**

- **#1 duplicate `is_void`**: the non-inline definition in `codegen_support.cpp` and the inline one in
  `sema_impl.hpp` violated the ODR (and broke the link on GCC 16). `is_void` now lives once, inline, in
  `types.hpp`; both old definitions are gone, so a clean build links with any conforming toolchain.
- **#2 `buffer<T>.ptr`** (and `T[].ptr`, the same type): the element data pointer for C interop, null
  for a null handle. The operand must be a stable object — `.ptr` on a temporary is a compile error
  because the pointer would dangle.
- **#3 multi-index `operator[]`**: the member form takes 1–3 index parameters, the free form takes the
  container plus 1–3 indices; every index must be an integer. The call path already forwarded every
  index, so only the declaration arity checks had to change.
- **#5 constant out-of-range axis**: `x.length(axis)` with a compile-time axis outside `0..rank-1` is
  rejected at the call site (`axis 2 is out of range for a rank-2 sequence (valid axes: 0..1)`)
  instead of reading header data; a runtime axis still aborts.
- **#4 docs**: SPEC status is current through 0.0.104; §2.4/§3.7.2/§5.1 now state that a string literal
  is `const char*`, that `String` has an implicit `String(const char*)` constructor and `.cstr()` goes
  back, and that a `const` unit-level `string` is the only forbidden form; `tests/README.md` no longer
  claims `len` or iteration over static arrays.

Tests: `buffer_ptr`, `multi_index_op`; must-fail `length_axis_const`, `slice_ptr_temp`.

**Tests**: 497/497 (150 pass + 325 must-fail + 19 check + 3 ir)

#### 0.0.104 — 2026-11-14

**`export` declarations + export-surface switch; self-registering unit init/cleanup.**

- **`export` is a top-level declaration modifier.** It implies `public` (nameable across units) and
  marks the declaration as part of the **binary export surface**. Combining it with `private`,
  `extern` or `use`, or writing it inside a type/interface body, is an error (export the type
  instead: that exports its public member functions plus the class descriptor, vtable and RC destroy
  entry). Implemented in the lexer (`export` keyword), the parser modifier loop, `Decl::is_export`,
  the export-surface collector (`codegen_func.cpp`), and SPEC §2.3 / §9 / §12 #127.
- **`--exports=export|public`** (default `export`) selects the surface: `export` keeps only
  declarations marked `export` externally visible, `public` keeps every `public` declaration (bulk
  export, including the implicitly loaded standard library). `--export <name>` still adds single
  names. In an executable everything outside the surface is internalized and dead-code eliminated:
  the default hello world stays at ~35 B `.text`, while `--exports=public` restores the full public
  API (and the ~8 KB of standard-library code).
- **Unit init/cleanup is now self-registering** (toolchain plan P11-2): each unit gets a guard global,
  runs its `use` dependencies first, initializes its own variables, then registers its `fini` with
  `atexit`; LIFO gives reverse cleanup without the global `fini_all` table, and the same hooks can be
  reused by a future library entry. The `main` wrapper calls every unit hook in loader order (guards
  make repeats free) and no longer owns the cleanup sequence.
- Tests: `export_basic` (export across units), must-fail `export_in_class`, `export_private`,
  `export_extern`; IR cases `export_surface` and `export_surface_public` (default vs
  `--exports=public`). The `tests/ir/` format gained `@ <args>` to pass compiler options per case.

**Tests**: 493/493 (148 pass + 323 must-fail + 19 check + 3 ir)

#### 0.0.103 — 2026-11-14

**`struct` implements interfaces fully: the interface default implementation is materialized as an
ordinary method (backlog U33 / U34 M4).**

- **What changes**: `struct S : I` no longer requires `S` to re-declare every interface method. When
  `I` (or a base interface of `I`) supplies a body, the compiler clones that body, re-checks it with
  `this` = `S`, and adds it to `S` as an ordinary method. It is **monomorphization, not
  inheritance**: no layout change, no vtable slot, no `itable`, no `super`, no extra allocation.
  Codegen is untouched — `emitStructMethod` emits the new `MethodDecl` like a hand-written one.
- **Resolution context**: the cloned body is checked in the unit that declares the default (its type
  names, including unit-private ones, resolve there), while `S`'s own members stay reachable — a
  default may call any interface method `S` implements, private or not. The strict check of the same
  body already ran in `check_interface`, so this relaxation opens no new access.
- **Conflicts**: each interface in the `:` list contributes at most one provider, most derived first
  (a default on a derived interface refines the base one and wins, matching class behavior). Two
  unrelated interfaces providing defaults for the same signature is a compile error — the struct
  must implement it explicitly.
- **Not materialized**: methods whose signature mentions `Self` (a value type cannot spell itself in
  that position); they still require an explicit implementation. Operators are compile-time only and
  never materialize.
- **Fixed along the way**: the satisfaction check compared signatures with `same_sig`, which
  deliberately ignores the return type, so `int f()` was silently satisfied by `long f()`. The
  return type is now compared explicitly (same wording as the class diagnostic: "override return
  type does not match the declaration in `I`").
- **New pass**: `Sema::apply_struct_iface_defaults` runs after `resolve_unit_types` and before
  `check_unit`, because function bodies are checked first; a call `s.m()` in another unit must already
  see the materialized signature. `MethodDecl` gained `from_iface_default` / `iface_unit`;
  `decl_visible` honors `extra_visible_unit_` during that re-check.

Tests: `struct_iface_default` (default calls the struct's implementation, default chains, explicit
override wins), `struct_iface_default_xunit` (cross-unit: the body uses a unit-private struct of the
interface's unit and calls a private implementation of the struct); must-fail
`struct_iface_default_conflict` and `struct_iface_ret_mismatch`.

**Tests**: 486/486 (147 pass + 320 must-fail + 19 check)

#### 0.0.102 — 2026-11-14

**Unit-level initialization and cleanup (SPEC 9.1).**

- **Runtime initializers**: a unit-level variable may have an arbitrary initializer expression; the
  non-constant ones are evaluated by a compiler-generated init sequence before the user `main`. The
  constant path is unchanged: a foldable initializer still lands in the data area and runs no code.
- **Exit-time destruction**: every unit-level variable whose type needs cleanup (class/interface
  handle, `String`, managed sequence, struct with reference fields or a user destructor) is released
  at exit in the exact reverse order. This lifts the three "no exit-time destruction" restrictions:
  a global `String`, a global struct with reference fields and a global struct with a user destructor
  are all legal. `const` still requires a compile-time constant, so `const string G = "hi";` stays an
  error (new diagnostic: a `const` unit-level variable cannot hold a type that needs runtime cleanup).
- **Deterministic order**: the loader's DFS post-order over `use` edges *is* the initialization
  order (declaration order inside a unit, implicitly loaded std units first, entry unit last), and
  cleanup is the exact reverse of the same sequence. Cleanup is registered with `atexit`, so
  `exit()` also cleans up; an exception escaping an initializer aborts without cleanup.
- **Ordering rule**: an initializer may observe only its own unit's earlier declarations and
  upstream units; a direct reference to a variable declared later in the same unit is a compile
  error. Indirect dependencies (function pointer, interface dispatch, `extern` C symbol, a later
  variable reached through a function body) are not tracked and are documented as undefined.
- **Entry wrapper**: the user `main` is emitted as `mu_main`; a generated C `main` runs the init
  hooks, registers the cleanup hook with `atexit`, then calls `mu_main` and returns its value. Each
  unit's hook is a separate symbol, so a future split-compilation mode can assemble the same
  sequence at link time.
- `S g;` for a struct with a parameterless constructor now calls that constructor from the init
  sequence (the same lowering as a local `S s;`) instead of leaving a zero-filled object.
- Implementation: new `src/codegen_unit.cpp`; `sema_types.cpp` (`check_global`,
  `check_global_init_order`), `ast.hpp` (`GlobalVar::runtime_init`, `FuncDecl::is_entry`),
  `mangle.cpp` (`unit_init_cname` / `unit_fini_cname`), `codegen_func.cpp` (entry renaming, runtime
  globals emitted as the zero value).

Tests: `unit_init_order` (A→B→C through `use`, init and reverse cleanup order), `unit_init_string`,
`unit_init_dtor` (struct with constructor+destructor, struct with a reference field),
`unit_init_assign` (zero-initialized global assigned in `main` and released at exit),
`unit_init_exit` (`exit()` still runs the cleanup), `unit_init_catch` (an initializer that catches
its own exception does not abort); must-fail `unit_init_forward_ref` and
`unit_init_const_nonconst`; `string_global` updated to the new diagnostic; `struct_dtor_global` and
`struct_ref_global` removed because both constructs are now legal.

**Tests**: 482/482 (145 pass + 318 must-fail + 19 check)

**Target policy — executable builds prune dead code (same release).** An executable has no external
consumers, so the backend now **internalizes every definition except `main`** before the `-O2`
pipeline; GlobalDCE then removes whatever the entry cannot reach — unused functions, whole unused
classes (descriptor + vtable + methods), unused library units and the runtime helpers they need.
`--export <name>` (repeatable) keeps a symbol externally visible for a C shim or a debugger. Measured
`.text`: hello world 8 115 B → 35 B, `arith` 8 572 → 487, `class_basic` 8 938 → 683,
`exception_basic` 8 604 → 956, `para_basic` 8 716 → 3 690, `strings` 9 052 → 1 839. The test runner
gained a fourth category, `tests/ir/*.mu` + `.txt` (`+ regex` must appear in the emitted IR,
`- regex` must not), so the pruning policy itself is regression-tested (`dead_code`). The design of
the other two targets (static library, shared library) and their different pruning rules is recorded
in `agents/toolchain-plan.md` §11; only the executable policy is implemented.
**Tests**: 483/483 (145 pass + 318 must-fail + 19 check + 1 ir)

**Documentation-only follow-up (no build bump)**: design decision recorded — **member promotion
(Go-style embedding) and mixin-style member injection are explicitly not done** in v1. Vetoed
variants (stateful trait mixin, offset-thunk interface satisfaction, textual `mixin template`),
the reopen threshold and the "adding a method to another type silently changes remote code"
argument are in `agents/design-backlog.md` U40; the normative entry is `docs/SPEC.md` §12 row 127;
the general criterion gained a row in `agents/agent-era-language-design.md` §2 (criterion 6).

#### 0.0.101 — 2026-11-14

**Sequence surface rework: `len` removed, static arrays become C storage, `String` renamed,
`operator[]=` removed, subscript bounds checks.**

- **`len` is gone** (`lexer` keeps the word only to give a migration diagnostic). Managed sequences
  answer `x.length()` (multidimensional: `x.length(axis)`, axis 0 when omitted); the compiler lowers
  it to the slice header for `T[]` / `buffer<T>` and to the library method for user types.
- **Static arrays `T[N]` / `T[m,n]` are C storage**: no `length()`, no `for-in`, no `len`. The element
  count is `sizeof(a) / sizeof(a[0])`; iteration is an index loop. `for-in` on a static array now
  reports the fix instead of silently iterating.
- **`buffer<T>` is the public safe memory block**: same representation as `T[]`
  (`{rc, len, cap, data}`), `buffer<T>` accepted as a type name, zero-initialized, RC-shared,
  element-wise RC for class/interface elements, `b.ptr` for C interop.
- **`operator[]=` is removed**: `ref T operator[]` covers reads, writes and compound assignment
  (`c[i] = v`, `c[i] += v`). Side-effecting computed setters are no longer expressible.
- **Multi-index `operator[]`** on user types (`operator[](usize, usize)`), so a future `Array2d`
  can be a library type; the AST already carried the index list, the sema arity check was relaxed.
- **`String` is the canonical type name**, `string` is a `type string = String;` alias kept for
  input convenience (existing sources compile unchanged). Diagnostics now print `std.string.String`.
- **Subscript bounds checks** for managed sequences: emitted once at the `T[]` / `buffer<T>`
  subscript, controlled by `--bounds=on|off` (default `on`). A violation calls
  `std.exception.bounds_fail`, which throws `BoundsError { index, length, axis }` — more debug
  information than an abort, and catchable. Static arrays and `T*` stay unchecked (C semantics).
- **Deferred**: the `Array<T>` / `Array2d<T>` / `Array3d<T>` library containers and their struct-vs-class
  choice are queued in `agents/design-backlog.md` (U38); `T[]` / `T[,]` / `T[,,]` remain the builtin
  managed-sequence syntax until then.

Tests: `bounds_error` (1-D and 2-D `BoundsError` with index/length/axis), plus the migrated
`arrays`, `multidim`, `dynarray`, `forin_*`, `member_op`, `objmodel_basic` and the
`std.string.String` error messages. `--bounds=off` is exercised by the same case.

**Tests**: 476/476 (139 pass + 318 must-fail + 19 check)

#### 0.0.100 — 2026-11-13

**C-style gaps closed: `->`, value `sizeof`, `offsetof`, literal const, C ABI types.** A probe of ~30
C idioms drove this batch (`muc check` / `build --run` on each); the remaining gaps (array argument
decay, `va_list`, user TLS, static locals) are recorded in `agents/TODO.md` and
`agents/toolchain-plan.md` §7.

- **`p->f`** is exactly `(*p).f` (SPEC 3.2, precedence 5.1). Implemented as **parser sugar** — no new AST
  node, no sema/codegen path to keep in sync; it chains (`n.next->v`, `p->arr[i]`) and one level only, as
  in C. `s->x` on a value reports "`*` requires a pointer".
- **`sizeof` / `alignof` take a value as well as a type** (SPEC 8.1): `sizeof x`, `sizeof *p`,
  `sizeof(a[0])`, `sizeof(p->x)` work and the operand is **never evaluated**; `sizeof(T)` still parses as a
  type first. A trailing `as` binds to the result (`sizeof *p as int` = `(sizeof *p) as int`). A lone name
  that is not a value falls back to a type, so `sizeof(c_long)` (an alias from another unit) resolves.
  The parser now also registers `type` alias names, so `sizeof(Alias)` and `Alias x;` parse as types.
- **`offsetof(T, field)`** (new builtin keyword) folds to a compile-time constant from
  `type_member_offset()` — a single layout definition now shared by codegen and the constant folder, so
  field addressing and `offsetof` cannot drift.
- **String literals are `const char*` for real** (`src/sema_expr.cpp`): the const now sits on the
  *referent* (elem level) like a written `const char*`, instead of on the pointer. Previously `char* p =
  "abc";` compiled and `p[0] = 'x'` wrote into `.rdata` (silently dropped as UB by the optimizer). Writing
  through a literal now needs an explicit `as char*`. Test `cstr_literal_const`.
- **`std.ctypes`** (`libs/std/platform/<os>/std/ctypes.mu`, SPEC 7.2): `c_char`/`c_int`/`c_size`/
  `c_long`/`c_wchar`… as transparent aliases, so an `extern` declaration is ABI-correct on every target.
  `c_long` is 32-bit on Windows (LLP64) and 64-bit elsewhere; `c_wchar` is 16-bit on Windows. `std.cstdio`
  now uses them: **`fseek`/`ftell` were passing a 64-bit `long` for C's 32-bit Windows `long`** (a real
  Win64 ABI bug, portability-plan #11) — fixed — and `fread`/`fwrite` are bound.

Tests: `arrow_basic`, `sizeof_expr`, `ctypes_aliases` (check) plus must-fail `arrow_nonptr`,
`cstr_literal_const`.

**Tests**: 475/475 (138 pass + 318 must-fail + 19 check)

> **Doc-only change (2026-11-14, no version bump)**: the standard-library naming conventions are
> fixed in `docs/style.md`. Decisions: built-in types are lowercase (`buffer<T>`) and library types
> are PascalCase (`Array<T>`, `Array2d<T>`, `Array3d<T>`, `Thread`); `String` is the canonical type
> name and `string` is a lowercase `type` alias kept for input convenience. Methods are snake_case
> with `length()` as the only element-count name and `is_empty()` as the only emptiness name;
> parameters are snake_case with full words (`dst` / `src` are the only abbreviations, destination
> first); constants are UPPER_SNAKE, enum members PascalCase, interfaces `I`-prefixed. `at()` is
> dropped — `operator[]` is the only element access, so out-of-range has exactly one behaviour;
> `find()` returns `isize` with `-1` for "not found". The built-in `len` is removed: managed
> sequences answer `length()`, while static arrays are C storage with no `length()` and no `for-in`
> (count with `sizeof(a) / sizeof(a[0])`). `buffer<T>` becomes the public safe memory block under
> the `Array` family; the `Array` family is written explicitly (no `T[]` → `Array<T>` sugar), and
> whether `T[]` survives at all as a `buffer<T>` shorthand is still open. `operator[]=` is removed:
> `ref T operator[]` covers reads, writes and compound assignment. Bounds checking is a compiler
> option (`--bounds=on|off`, default on), emitted once in `buffer<T>[]` so upper layers do not
> repeat it, and a violation throws `BoundsError` with the index and length. Whether `Array` is a
> struct or a class is deferred until the other containers are designed. The language-mechanism
> work is queued as U38, the existing `std.string` / `std.thread` deviations as U37 in
> `agents/design-backlog.md`.

#### 0.0.99 — 2026-11-13

**Per-library platform directories + cross-platform check (portability stage 0)**: units are no longer
selected by a global `platform/<os>` search root. The loader now resolves every unit **relative to
itself**: for `a.b.c` it tries `<root>/a/b/platform/<os>/`, `<root>/a/platform/<os>/`,
`<root>/platform/<os>/` (nearest shadow first) and finally the portable unit (`src/loader.cpp`). That
makes a platform implementation **belong to the library that owns it** —
`ms/xxlib/platform/linux/ms.xxlib.io.mu` travels with the library, so a third-party tree can be vendored
as one directory instead of merging into a shared `platform/` tree; `libs/std/platform/windows/…` keeps
the std backends and needed no file moves.

- **Both file names are accepted everywhere**: the flat dotted name (`std.thread.backend.mu`, which the
  loader already tried before the slashed form) and the slashed path. Inside a shadow directory the file
  keeps the full unit name, so one `ls` lists every OS-specific unit of that tree.
- **`platform` is reserved**: a unit name may not start with `platform`, and a `platform.<os>` segment
  pair is rejected; two files for the same unit in one directory is an **ambiguity error** (it used to
  silently pick the flat one).
- **`--os windows|linux|macos`** on `check` (and on `build` for the host platform only) resolves the
  program against another platform's units, on any host: this is the substitute for `#ifdef`'s
  ability to compile "the other branch", and it is stronger, because the other branch is fully
  type-checked. `-v` prints the file every unit resolved to. The missing-unit diagnostic lists every
  candidate path, platform shadows first.
- The loader also stopped adding `libs/std` as a search root (nothing resolved through it; it only
  produced noise in the diagnostics).

Tests: `shadow_platform` (each OS ships its own `shadowlib.probe`, the portable fallback returns 0 and
must lose — the three platform files exercise both accepted file names), `unit_platform_reserved`
(`unit platform.foo;` rejected) and `unit_ambiguous` (both file forms in one directory). Docs: SPEC
§2.1 now specifies unit discovery + platform shadows; `agents/portability-plan.md` gained stage 0 and
its status moved to "implementing".

**Tests**: 470/470 (136 pass + 316 must-fail + 18 check)

> **Doc-only change (2026-11-13, no version bump)**: decided **no `unsafe` mechanism, permanently**
> (`docs/SPEC.md` principle 7; `agents/toolchain-plan.md` D13/§10). Every facility — raw pointers and
> arithmetic, `alloc`/`free`, `union`, uninitialized locals, `extern`, future intrinsics — stays
> available to every programmer, and `unsafe` is **released from the reserved-word list**. Rationale: a
> marker only means "break a guarantee the compiler enforces elsewhere"; mu's memory layer has no such
> guarantee (no borrow checker), so the marker would have to cover the whole language and lose its grep
> signal. The three jobs it would have done are carried instead by explicit, greppable intrinsic names
> (`atomic_*`, future `mmio_*`), optional runtime checks (bounds / null / div-zero / RC underflow; TODO
> entry added) and the documented UB list (SPEC 8.2). No compiler code changed; the test baseline is
> unchanged.

#### 0.0.98 — 2026-11-13

**M:N task runtime for `para` / `para for` (concurrency P3)**: the OS-thread-per-unit implementation is
gone. The new `libs/std/task.mu` scheduler runs **one task per concurrent unit on its own fiber**, and a
bounded set of **carriers** (OS threads, one per core) executes them:

- **Task = fiber**: a parked task keeps its stack; switching is `SwitchToFiber` (Win32 backend:
  `ConvertThreadToFiberEx` / `CreateFiberEx` / `SwitchToFiber` / `DeleteFiber`). Stacks are fixed
  (default 64 KiB commit / 256 KiB reserve, `set_task_stack`) and recycled through a pool, so
  `para for` over thousands of items costs thousands of small records instead of thousands of threads.
- **Carrier loop**: pop from the global run queue → record the task in carrier-local TLS → switch into
  the task's fiber → on return save the exception slot. An idle carrier sleeps on a condition variable;
  every `push_run` signals it (the lost wakeup that made the first version hang at ~1500 tasks).
- **Join**: a task parks (its carrier keeps running, so nested `para` cannot starve); `main`, a
  `std.thread` thread or a C thread waits on a condition variable. `finish_locked` wakes the waiters,
  recycles the fiber and publishes `T_DONE` — the fiber only sets an `exited` flag, because publishing
  the state itself let a joiner free the task while its carrier was still saving state
  (write-after-free, found with the CRT heap validator).
- **Handoff (design 6.4)**: a monitor thread adds a carrier (up to 4x cores) when the queue is
  non-empty and every busy carrier has missed its heartbeat for 10 ms. Blocking calls pin a carrier in
  this version; `para_handoff` checks the carrier count grows.
- **Exception slot migration (design 6.2)**: the compiler now emits `muc_eh_slot()` when the runtime
  references it (`src/codegen_func.cpp`), and the runtime saves/loads the 16-byte pending slot around
  every switch, so a task may resume on a different carrier.
- **Cap and settings**: `std.task.TaskLimitError` with `set_max_live_tasks` (default 10000 tasks),
  `set_task_stack`, `set_carrier_limit`, `live_task_count`, `carrier_count`; the lowering entry points
  moved from `std.thread` to `std.task` (`src/codegen_stmt.cpp:runtimeFn`), and `std.task` is loaded
  implicitly.

Two bugs found by the review pass and fixed before this entry shipped: (1) a failed `muc_para_start`
recorded its (undefined) handle in the region's task list before the pending check — the cleanup then
joined a garbage pointer; the check now comes before `muc_para_group_add`; (2) the fiber published
`T_DONE` before switching back, so a joiner could free the task under the carrier.

Two more fixed after the second review pass: (3) `started` was published in a different critical
section than the TLS keys, so a concurrent `muc_para_join` could read `tls_next == 0` and take the task
path on a plain thread (wild waiter write + `SwitchToFiber` on garbage); the keys, the failure check and
`started` are now one critical section, and `join` also guards on `started`. (4) `CreateFiberEx` /
`ConvertThreadToFiberEx` were called without `FIBER_FLAG_FLOAT_SWITCH`, so the FP state had no
guarantee across a switch even though task bodies may use `float`/`double`; both now pass the flag.
Also added `set_carrier_limit` (a test/embedder knob: it caps the initial carrier set and handoff
growth), a null-handle guard in `muc_para_join`, and an abort on `TLS_OUT_OF_INDEXES`.

**Docs**: SPEC 3.17.1/3.17.6/3.17.7 rewritten for the M:N runtime (fiber stacks, join parking, handoff,
cap settings), `agents/task-runtime.md` P3 marked done, and the library contract documented in
`docs/lib-std-task.md`. Tests: `para_many_tasks` (2000 items through the scheduler → 1999000),
`para_handoff` (the carrier set grows while tasks block → `grew`), `para_join_chain` (a nested fan-out
that parks on every level with **one** carrier → `8 1`), `para_limit_sections` (a failed section start
joins what already started → `limit 0`) and `para_task_fp_eh` (a task catches, parks and resumes with
`double` work → `5.50 0.25`), plus the existing `para_*` suite (including the cursor, `for ref`, cap and
mid-stream-throw cases) on the new runtime.

**Tests**: 467/467 (135 pass + 314 must-fail + 18 check)

#### 0.0.97 — 2026-11-13

**`para for` is one task per item, streamed from a single puller (concurrency P2b-4)**: the automatic
8-way chunking is gone. The producer is now the ordinary for-in lowering itself — the source expression
is evaluated **once** (so the old "must be a variable or member access" restriction is lifted), a
`next()`/`operator[]` pull happens between item starts (a blocking `next()` is where backpressure
lives), and the loop body is replaced by "start one task for the current item"
(`src/codegen_stmt.cpp:emitParaFor`, `emitParaForSpawnItem`, `emitParaForBody`). Each item gets a heap
context `{ frame*, item }` (the element address for `for ref`) that is retained on the producer side and
released by the task body, then freed by the task wrapper; the region records every handle in a task
list (`std.thread.muc_para_group_new/add/join_free`) and joins them when the source ends. The cursor
form of `for-in` is now a valid `para for` source (`sema` no longer rejects it), and `emitFor` lost the
`forChunkStart`/`forChunkStride` overrides.

**Task cap**: `std.thread.muc_para_start` now accounts live execution units (1000, a dedicated SRWLOCK
in the Win32 backend) and **throws `std.thread.TaskLimitError`** when a start is refused, as §3.17.7
requires. A refused spawn is an ordinary exception: the region releases the failed item's context
(including its retained element copy), joins the tasks that already started (the same rule for a failed
`para` section start), and propagates at the region's join point. `emitPara`/`emitParaFor` emit the
pending check because the runtime entry can now throw.

**Unwinding out of the source joins the region**: every check emitted while the producer runs (source
expression, range bounds, `iterator()`/`next()`/`current()`, `length()`, `operator[]`) now runs the
region's cleanup (`codegen_impl.hpp:paraUnwindCleanup`, invoked from `emitUnwindCurrent` *before* the
scopes are released), so a cursor that throws mid-stream cannot leave tasks running against freed
captured slots. `backend_thread_start` returns null instead of aborting when `CreateThread` fails, so
the refusal surfaces as a `TaskLimitError` (`std.thread.thread_start` still aborts, as before).

**Docs**: SPEC 3.17.3/3.17.7 rewritten (single puller, cursor sources, one task per item with its real
cost, `TaskLimitError`, source-throw join), `agents/task-runtime.md` P2b-4 marked done. Tests:
`para_for_cursor` (cursor source → 91), `para_for_ref` (`for ref` over a local array → 56),
`para_for_limit` (4000 sleeping items → the cap throws and is caught → `limit`), `para_for_throw` (a
throwing `next()` mid-stream: 30 rounds × 44 started tasks, no join means the cap is exhausted inside
the test → `30 0`), `para_for_throw_index` (a throwing `operator[]` mid-stream, index form → `20`), and
the existing `para_for_basic`/`para_for_array` keep passing with per-item tasks.

**Tests**: 462/462 (130 pass + 314 must-fail + 18 check)

#### 0.0.96 — 2026-11-13

**Cursor form of `for-in` (concurrency P2b-3)**: a user type that provides `iterator() -> It` is now
iterated through the cursor lowering — `auto it = seq.iterator(); while it.next() { v = it.current(); … }`
(`src/sema_class2.cpp:resolve_cursor_shape`, `src/codegen_stmt.cpp:emitFor`). `It` must be a struct
returned by value (no allocation, static dispatch) and satisfy the contract declared in
`libs/core/iter.mu` (`core.iter.IIterator<T>`: `bool next()`, `T current()`); the compiler checks it
structurally and the diagnostics name the interface. Form resolution changed accordingly (SPEC 6.4.3):
a type with `iterator()` uses the cursor form, otherwise the index form. `for ref` is rejected for
cursor sources, elements are bound by value (retain/release for class elements), and `next()` may block
(a stream). `para for` over a cursor is explicitly rejected for now (it would give each chunk its own
iterator and consume the stream 8×), so a plain `for` is the only cursor consumer until the stream
lowering lands. Test: `cursor_basic` (cursor over 3 elements → 60).

**Tests**: 457/457 (125 pass + 314 must-fail + 18 check)

#### 0.0.95 — 2026-11-13

**Compiler-facing contracts are explicit interfaces (`libs/core`)**: a new `core.iter` unit (`libs/core/iter.mu`, loaded implicitly like `std.thread`) declares the `for-in` contracts as real interfaces — `IIterator<T>` (`bool next()`, `T current()`) and `IIndexable<T>` (`usize length()`; the subscript half stays structural because an interface cannot declare `operator[]`). Nothing changes semantically: the compiler keeps checking the protocol structurally (no vtable, no boxing), but the contract now has a definition site, so an implementer can `use core.iter;` and declare `: IIterator<T>` to have their cursor checked against the interface. SPEC 6.4.2 now names the contract and records the stream clauses (`next()` may block, it is a park/cancellation point in a task, one consumer per cursor, value elements).

**Tests**: 456/456 (124 pass + 314 must-fail + 18 check)

#### 0.0.94 — 2026-11-13

**`para for` over arrays/slices/strings (concurrency P2b, part 2)**: non-range sources now work by reusing the ordinary `for-in` lowering once per chunk with a **stride override** (two new Emitter fields, `forChunkStart`/`forChunkStride`): chunk k visits indices `k, k+8, …`, so fixed arrays, slices, strings and user types that satisfy the index form all work without a second code path, and the loop variable keeps the ordinary per-iteration binding (value or `ref`, with element retain/release). The sequence must be a simple variable or member access, because each chunk evaluates it (explicit diagnostic otherwise); `for i, x` is still rejected, and the cursor form of `for-in` remains unimplemented, so a blocking stream cannot be a `para for` source yet. Tests: `para_for_array` (Σ 3·0..7 = 84); the range test and the `break` must-fail case are unchanged.

**Tests**: 456/456 (124 pass + 314 must-fail + 18 check)

#### 0.0.93 — 2026-11-13

**`para for` over ranges (concurrency P2b, part 1)**: `para for i in a..b { ... }` (including `..=` and `step`) is implemented by splitting the range into a fixed number of **strided chunks (8)** and running one OS thread per chunk; each chunk is a normal loop over a private copy of the loop variable, so the body keeps the same capture rules as `para` (captures travel through the shared read-only frame) and `break`/`continue` at the body level are compile errors (`para_for_break`). The chunk body and its exception wrapper are queued like `para` sections and emitted after the enclosing function; the queue is drained until empty because a body may itself contain a parallel region. Not yet: the non-range sources (arrays/slices/strings) and the cursor form of `for-in`, so `para for` currently rejects anything that is not a range (`para_for_array`). Tests: `para_for_basic` (sum of squares 0..15 = 1240) plus two must-fail cases.

**Tests**: 456/456 (123 pass + 315 must-fail + 18 check)

#### 0.0.92 — 2026-11-13

**`para` (concurrency P2a)**: `para { { ... } { ... } }` is implemented with one OS thread per section (`src/parser_stmt.cpp`, `src/sema_class1.cpp`, `src/codegen_stmt.cpp`). Each section becomes a hidden function that receives a frame of **pointers to the caller's captured locals** — the caller is suspended at the join, so nothing is copied and no reference count is touched; a nested `para` forwards captures through every enclosing frame (a capture is recorded in all active sections, so the inner frame can reach the outer function's slot through the outer frame). The compile-time rules of SPEC 3.17.2 are enforced: writing an enclosing local, taking its address, passing it by `ref`, `return`, and `break`/`continue` that would leave the section are errors, while writes through a class handle, a slice buffer or a pointer are allowed (explicit sharing). v1 limitations are recorded in SPEC 3.17.7: sections are joined in source order with no cancellation, an exception that escapes a section aborts the process (the B2 thread-boundary rule) instead of being rethrown, and `this` / implicit field access inside a section is a compile error. `std.thread` is now loaded implicitly because it provides the two lowering entry points (`muc_para_start` / `muc_para_join`); a program without `para` never starts the runtime. Tests: `para_basic` (two sections, captured local, results through a class), `para_nested` (nested sections) and five must-fail cases (`para_write_outer`, `para_return`, `para_addr_outer`, `para_ref_outer`, `para_this`).

**Tests**: 453/453 (122 pass + 313 must-fail + 18 check)

#### 0.0.91 — 2026-11-13

**Thread primitives: `std.thread` + platform search path (concurrency P1)**: the loader now puts `libs/std/platform/<os>` ahead of `libs` (`src/loader.cpp`), so a unit can have a platform-specific implementation chosen at load time without a preprocessor. The first user is `libs/std/thread.mu` — a platform-neutral API (`Thread`, `thread_start/join/detach/sleep`, `monotonic_ms`, `Mutex`, `CondVar`, `WAIT_INFINITE`) — with the Win32 implementation isolated in `libs/std/platform/windows/std/thread/backend.mu`: `CreateThread` with a `(void*) => uint` trampoline over a heap context, `WaitForSingleObject`, `Sleep`, `GetTickCount64`, and SRWLOCK / CONDITION_VARIABLE (both pointer-sized and zero-initialized, so a zeroed `Mutex`/`CondVar` is immediately usable — no init/destroy call). The API is deliberately C-shaped (function values plus `void*` userdata, no captures) and is the layer the task runtime will be built on; the front-end only knows the portable names. Two new tests: `thread_smoke` (4 threads × 1000 mutex-protected increments, deterministic 4000) and `rc_stress` (50k producer/consumer handoffs through a size-1 slot: every object is created on the producer and destroyed on the consumer, count 50000, which is exactly the cross-thread RC path the 0.0.90 ordering fix addresses). Both tests use only the portable API, so they keep working once a POSIX/macOS backend exists; Win32 is the only backend today.

**Tests**: 446/446 (120 pass + 308 must-fail + 18 check)

#### 0.0.90 — 2026-11-13

**RC memory ordering corrected for cross-thread sharing (concurrency P0)**: the strong reference count used `monotonic` for both the increment and the decrement and destroyed the object as soon as the decrement observed 1 — the classic relaxed-RC bug: the thread that releases the last reference need not see what the other threads wrote before dropping theirs. The count now follows the Arc pattern: increments stay **relaxed**, the decrement is **release**, and the destroy path executes an **acquire fence** before loading the class descriptor (`src/codegen_support.cpp:ensureRelease`). The dynamic-array/slice header release got the same treatment (`src/codegen_runtime.cpp:emitSliceRelease`), and the global weak-registry spin lock now uses **acquire** on lock and **release** on unlock instead of a relaxed exchange (harmless on x86-TSO, wrong on ARM). Verified by inspecting the emitted IR: `atomicrmw sub ... release` + `fence acquire` inside the destroy block, `atomicrmw add ... monotonic` for retains, `atomicrmw xchg ... acquire/release` for the weak lock. SPEC 3.8/8.3 and questions 74/87/94 updated from "relaxed" to the precise ordering. The test count does not change: a single-threaded program cannot observe the difference, and the cross-thread stress test arrives with the task runtime (P1), because the language has no thread or mutex primitives yet.

**Tests**: 444/444 (118 pass + 308 must-fail + 18 check)

#### 0.0.89 — 2026-11-13

**`string` is now a pure library type (no compiler knowledge at all)**: a string literal is a static, NUL-terminated UTF-8 buffer whose type is **`const char*`** (as in C), and `string s = "abc";` goes through the library's `implicit string(const char*)` (0.0.88). Every operation moved into `libs/std/string.mu` as ordinary code: byte length is `s.length()`, indexing is the **member** `operator[]` (borrowed receiver: no copy, no RC, 0.0.87), `for c in s` uses the index form, `==`/`!=` are library operators with overloads for `string` and `const char*` (`"abc" == s` works too), `+` is a member operator, and `s.cstr()` replaces the implicit `string → const char*` conversion. `TypeKind::String` was deleted, so codegen no longer knows the `{p, n, owner}` layout: `sizeof(string)` now comes from the resolved type (it used to answer 8 in expressions and 24 in constants) and `stringConst` only emits a `const char*` to the literal bytes. Consequences, all documented in SPEC 3.3: `string` cannot be a unit-level variable any more (reference-counted data has no exit-time destruction — use `const char*`), `printf("%s", s)` needs `s.cstr()`, and `len(s)` is a compile error (`len` serves built-in sequences only). A `char[N]` may still be initialized from a literal (byte copy + NUL padding, now with a length check). Added 2 run cases (`string_library` plus the migrated string cases), 2 must-fail cases (`string_global`, `string_no_implicit_cstr`).

**Tests**: 444/444 (118 pass + 308 must-fail + 18 check)

> **Doc-only change (2026-11-13, no version bump)**: added SPEC 3.17 (concurrency: `para` / `para for` /
> `thread`) together with the keyword table (2.3) and reserved-word list (2.5) updates, the UB
> entry for data races (8.2), the concurrency non-goals (10), open questions #120–125 (12) and a roadmap
> note (14). The decision record, invariants, library contracts and phasing live in
> `agents/task-runtime.md`. No compiler code changed and the test baseline is unchanged.

#### 0.0.88 — 2026-11-13

**Implicit struct construction (`implicit T(one_param)`)**: a `struct` constructor marked `implicit` is now the conversion path used at the coercion points (initialization, assignment, argument passing, `return`). It is struct-only (a class conversion would allocate implicitly), must be public, exactly one parameter and not the struct itself; at most one step and matched by the built-in conversions, so `string s = cstr;` will resolve to `implicit string(const char*)` while `f(int)` still beats `f(Wrap)` for an `int` argument (rank: exact 0 > widening 1 > implicit construction 2). It does not apply inside expressions, and it costs exactly what the constructor body costs — this is the missing piece for moving `string` out of the compiler (0.0.89). Codegen materializes the value with the constructor call (`src/codegen_expr.cpp:coerce`); `implicit` is a contextual keyword (a modifier only in front of a constructor name), and writing it in a class body is a compile error. Added 1 run case (`implicit_ctor`), 3 must-fail cases.

**Tests**: 441/441 (117 pass + 306 must-fail + 18 check)

#### 0.0.87 — 2026-11-13

**Member operators on `struct` (borrowed receiver)**: a struct may now declare `T operator+(B other)`, `T operator[](usize i)`, `void operator[]=(usize i, V v)`, unary `-`/`+`/`~`, and the comparison/bitwise set as **member** functions (`src/parser_unit.cpp`, `src/sema_types.cpp:check_member_operator_sig`, `src/sema_call.cpp:find_member_operator`). The receiver is the implicit `this`, i.e. a borrowed pointer, so `s[i]` / `s == t` copy nothing and touch no reference count — which is the prerequisite for moving `string`'s operators into the library at built-in cost. Member operators are static-dispatch only and **forbidden in classes** (a class operator would be virtual and would break `==` identity); a member operator wins over a free function with the same signature, and falls back when no overload accepts the arguments (`-s` vs `s - t`). The index-form `for-in` protocol now also accepts a member `operator[]` (and prefers it), `a != b` converts through a member/free `operator==` with the result negated, and the `T: I` constraint check accepts operator members as well. Added 1 run case (`member_op`), 3 must-fail cases and 1 check case.

**Tests**: 437/437 (116 pass + 303 must-fail + 18 check)

#### 0.0.86 — 2026-11-12

**Out-of-range shift counts in constant expressions + stale status text**: a shift by ≥ the bit width (and a negative shift count) inside a constant expression is now reported directly (`shift count 40 is out of range for `int` (0..31)`) instead of returning "not evaluable", which let `check` pass and then failed in the backend with "global initializer must be a constant" pointing at a standard-library line. Valid shifts (`1 << 31`, `(1 as long) << 40`) still evaluate. Documentation: the non-goal list no longer claims dynamic arrays are unimplemented (they are built in; only library container classes are missing), the two duplicated open-question numbers (110/111) were renumbered so the table runs 110–119, the `deinit` row now points at the implemented `~T()` destructor, and backlog U33 is marked implemented (S1 static satisfaction and S4 constraint dispatch, with the S3/S5 dynamic channel closed). Added 2 must-fail cases (`shift_count_overflow`, `shift_count_negative`).

**Tests**: 432/432 (115 pass + 300 must-fail + 17 check)

#### 0.0.85 — 2026-11-12

**Signed division overflow in constant expressions, test-runner hardening, documentation drift**: `MIN / -1` and `MIN % -1` are now rejected in a constant expression (both 32-bit and 64-bit) with an explicit diagnostic instead of silently wrapping for `int` and reporting "not a compile-time constant" for `long`. The specification records division/remainder overflow as UB (like division by zero) and keeps the two's-complement wraparound promise for `+`/`-`/`*` only — the CPU traps on the division, so a runtime `MIN / -1` is UB by contract; `-x` wraps. `tests/run_tests.py` now discovers `main` in `examples/*.mu` by scanning code with comments and string/char literals stripped (comments or formatting no longer hide or fake an entry point) and deletes only the artifacts of the current invocation (`build/<case>.o`, `build/tests/<case>.exe`) instead of globbing `build/`, so a developer's own `.o`/`.ll` files survive a test run. Documentation: dropped the stale `!` operator from the constant-expression lists, corrected `tests/README.md` (`~` is the complement, `const` instead of `readonly`, no `++` expression), narrowed the `const`-on-function-type rule to named aliases (with a parser hint), and refreshed the SPEC header version plus the enum section status. Added 2 must-fail cases (`div_overflow_int`, `rem_overflow_long`).

**Tests**: 430/430 (115 pass + 298 must-fail + 17 check)

#### 0.0.84 — 2026-11-12

**Function types and non-capturing lambdas (SPEC 3.16)**: added `(T1, T2) => R` as a one-word function value — binary compatible with a C function pointer, no declarators and no `*`, `sizeof`/`alignof` = 8, `=>` binds lowest and is right-associative (`*`/`[]` belong to the return type). A free function name is already a value: `&f` is rejected with a hint, overloaded names are resolved by the target type, and generic function names plus methods are not values. Added the `Lambda` expression `(int a) => expr` / `(int a) => { ... }`: parameter types are written, the return type is inferred from the `return` expressions (all must be identical, `null` adapts; no `return <value>` means `void`; a non-void lambda must return on every path), it lowers to a hidden `private` function and allocates nothing; captures (an enclosing local, `this`, an enclosing lambda parameter) are compile errors that point at `class` + single-method `interface`. Calls through a function value work from locals, parameters, struct/class fields, array elements and unit-level variables; a unit-level initializer may name a function (an address constant, emitted as the symbol address); `void*` ↔ function type follows the pointer rules (`as` required back). Replaced the obsolete `func_as_value` must-fail case with `fnptr_assign_to_int`; added 2 run cases (`fnptr`, `lambda`), 7 must-fail cases and 1 check case (`fn_type`).

**Tests**: 428/428 (115 pass + 296 must-fail + 17 check)

#### 0.0.83 — 2026-11-12

**Conservative module-local may-throw optimization**: added `src/may_throw.cpp`, a whole-program fixpoint over visible function, constructor and struct-method bodies. A call whose callee is proven not to return with a pending exception skips the B2 pending-flag check; free functions, operators, constructors, local struct construction, `new`, `super(...)`, `for-in` `length()`/`operator[]`, and struct methods use the summary. Class/interface dispatch, external/precompiled/static-library/dynamic-library and otherwise unknown targets are conservatively may-throw, so the optimization does not constrain the future library ABI. Catch barriers are over-approximated; destructors and `finally` are nothrow (an escaping exception aborts). Added 3 runtime cases (nothrow free/recursive/struct-method code, may-throw struct methods, and a cleanup-free struct constructor that throws).

**Tests**: 418/418 (113 pass + 289 must-fail + 16 check)

**Docs only (no code change, version not bumped)**: added SPEC 3.16 — function types `(T1, T2) => R` (a one-word value, binary compatible with a C function pointer, no declarators and no `*`) and non-capturing lambdas `(int a) => expr` / `(int a) => { ... }` with a return type inferred from the body; block- and unit-level declarations plus return types never use an anonymous function type (`auto` for locals, a `type` alias elsewhere), while fields and parameters may; a free function name is already a function value (`&f` is rejected) while methods are not values. Cross-references updated in 2.3, 3.2, 7, 10, 11 and 12 (#10, #116–#119); backlog U35 records the decisions, the rejected alternatives and the future two-word `delegate`.

#### 0.0.82 — 2026-11-12

**Exceptions stage 2: `finally`**: `finally` now runs on every exit of the `try` statement — normal completion, a matched `catch`, `return`, `break`, `continue`, and an exception that is caught or propagated — exactly once per exit. Nested `try/finally` runs inner before outer. The finally body is emitted by duplicating it on each exit path, so no platform EH support is needed. A `finally` body must not use `return` / `break` / `continue` / `throw` directly (compile errors); a nested `try/catch` is allowed, but an exception escaping it aborts (cleanup must not throw). While the finally body runs, the pending exception is saved and temporarily cleared, so ordinary calls inside `finally` do not observe the in-flight exception; the original pending state is restored afterwards. Interface catch is explicitly not supported: `catch` accepts only classes derived from `Exception`. Added 10 runtime cases (normal/catch/no-match/return/catch-return/break/continue/nested/throw-from-catch/inner-catch) and 4 must-fail cases; removed the old "finally unsupported" error case.

**Tests**: 415/415 (110 pass + 289 must-fail + 16 check)

#### 0.0.81 — 2026-11-12

**Exceptions stage 1 (B2 lowering)**: implemented `throw`, `try`, and `catch` (class matching in written order over the descriptor base chain), read-only catch bindings, RC cleanup on unwinding, constructor-throw cleanup, uncaught `abort` at `main`, and `std.exception.Exception` as an implicit standard-library unit. The function ABI is unchanged: a thread-local pending slot (`flag` + object pointer) is checked after every mu_lan call; a throwing function releases its active scopes and returns a dummy value with the flag set, and callers propagate it until a matching `catch`. `extern "C"` calls are not checked. No `.eh_frame`/`.pdata`/personality/unwind runtime is involved, so Windows/Linux/macOS share one lowering and the libc-only portability is preserved. `finally` was not yet implemented and an interface is not a valid `catch` type (catch accepts `Exception` subclasses only); may-throw analysis is stage 3. Added `libs/std/exception.mu`, 12 runtime cases (including a generic function carrying try/throw, a destructor that calls a mu_lan method while unwinding, a destructor that catches a local throw, and exception-object ownership across a catch), and 5 must-fail cases. Cleanup runs with the pending flag temporarily cleared, so ordinary calls inside destructors do not see the in-flight exception; a destructor may still catch locally, and a nested throw that escapes unwinding aborts.

**Tests**: 402/402 (100 pass + 286 must-fail + 16 check)

#### 0.0.80 — 2026-11-12

**English comments + English docs**: translated all 874 Chinese comment lines across the 42 files in `src/` with one consistent terminology; added the English editions of the specification (1633 lines, section-for-section parity with the Chinese text) and of the version history (79 entries). String literals (user-visible diagnostics) are untouched, and the full suite verifies no behavioural change.

**English-only public tree (documentation follow-up, no version bump)**: removed the Chinese `README.md`, `docs/SPEC.md` and `docs/versions.md`; the English editions took over the plain names and are the only maintained copies from now on. Translated the remaining Chinese comments and prose in every other published file: 420 comment lines across 86 `.mu` files (`examples/`, `libs/std/` and `tests/`), plus `tests/README.md`, `tests/run_tests.py` (including its console output), `CMakeLists.txt`, `ci.yml`, `.gitignore`, `.vscode/settings.json` and `NOTICE`. `agents/` (internal, gitignored) keeps its Chinese notes, and the multilingual tests (`unicode_escape`, `char_nonascii`, `unicode_*`) deliberately keep their non-ASCII literals and expectations. Every `.mu` change was verified to be comment-only (code, string and char literals byte-identical).

**Stale statements corrected (same documentation follow-up)**: the README no longer lists generics and automatic `weak` nulling as unimplemented (both have shipped and are covered by tests), gains a generics row, narrows the SPDX claim to `src/`, and drops the reference to an internal cross-platform plan; the SPEC header no longer calls destructors a non-goal and states the version it is current through; the public changelog no longer points at an internal glossary; CMake now requires `LLVM ≥ 17`, matching the README.

**Generics showcase example (documentation follow-up, no version bump)**: added `examples/generics.mu`, a commented walkthrough of monomorphization — a generic `struct Vec<T>` with methods, constrained free functions (`max<T: IOrdered>`, `sum<T: IAddable>` taking a nested `Vec<T>`), and a user type (`Money`) that satisfies the constraints structurally with no `implements` clause. The test runner now auto-discovers every `examples/*.mu` that defines `main` (instead of hardcoding `examples/example.mu`) and compares it against `tests/expected/<name>.txt`, so the example is itself a regression case (`generics.txt`). No compiler changes.

**Tests**: 385/385 (88 pass + 281 must-fail + 16 check)

#### 0.0.79 — 2026-11-12

**Version history switched to section format**: long entries were easily broken by pipes in a table (incomplete escaping shifted the columns), so the structure became `### version — date` + summary + `**Tests**`; the 78 records were re-sorted in descending version order (newest first). No code changes

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

#### 0.0.78 — 2026-11-12

**Documentation cleanup wrap-up**: `versions.md` moved back into `docs/` (publicly released, readers need the changelog); fixed the unescaped pipe in the 0.0.57 entry (the diagnostic fragment `` `  |` `` used to break the table column, shifting the following columns). AGENTS and the README/README.en document tables synced. No code changes

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

#### 0.0.77 — 2026-11-12

**Documentation boundary tightened**: `docs/` keeps only `SPEC.md` (the public specification); the code map, backend notes, object model plan, cross-platform plan, version history, visibility design and std.string layout all moved into `agents/` (gitignored). README and SPEC no longer link to unpublished files. No code changes

**Tests**: 384/384

#### 0.0.76 — 2026-11-12

**Documentation directory cleanup (docs)**: `docs/` is designated the public documentation directory and `SPEC.md` moved to `docs/SPEC.md`; added `agents/` (**gitignored, not committed**) to hold local AI collaboration artifacts — `design-backlog.md`, `design-review.md`, `TODO.md`. README / README.en / AGENTS / SECURITY and the relative links between documents updated in sync (including the `examples/` and `visibility-design` paths inside SPEC), and the README version number refreshed to 0.0.75. `docs/design-review.md` is no longer publicly released. No code changes

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

#### 0.0.75 — 2026-11-12

**Explicitly dropping "value type as interface value" (documentation decision)**: after evaluation, without borrow checking a value type → interface value is necessarily boxed, costing roughly a `new` of a class and losing the explicit lifetime; runtime polymorphism is covered by `class`, and heterogeneous value collections use sum types. Closed backlog U33 S3/S5 and U34 M5b; SPEC adds 3.15.8, while 3.15.7 and table 110 changed to "explicitly not doing" with the rationale and the condition "can be re-evaluated once borrow checking lands". No code changes

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

#### 0.0.74 — 2026-11-12

**U33 S1+S2: `struct S : I` static satisfaction**: only interfaces may be listed after `:` (class/struct is an error); interface methods (including the inheritance chain and default methods) must be implemented explicitly with `override`, and a missing implementation or a missing `override` each have a dedicated diagnostic. `struct` methods support a leading `override` (previously only `class` did), and `MethodSig::is_override` is propagated faithfully. Static satisfaction produces no value type and no vtable, with zero codegen changes. Added the `struct_iface` end-to-end case + 2 error cases

**Tests**: 384/384 (87 pass + 281 must-fail + 16 check)

#### 0.0.73 — 2026-11-12

**SPEC generics chapter (docs)**: added **3.15 Generics** (monomorphization strategy, declaration/use syntax, instantiation rules, the constraint type set + member set model, `&`/`|` duality, structural satisfaction and exact matching, `Self`, current limitations); updated section 0 at a glance, 2.3 (`Self` contextual keyword), 3.10 (generic interfaces as constraints only), 7 (functions support generics), 10 (moved out of the non-goals), 12 tables 95/110/111, 14 implementation roadmap. Docs aligned with the implementation in 0.0.64–0.0.72

**Tests**: 381/381 (86 pass + 279 must-fail + 16 check)

#### 0.0.72 — 2026-11-12

**Generic interfaces as constraints (M5a)**: `instantiate_generic` supports `InterfaceDecl` (`make_interface`, member signatures including `is_operator/ret_self/params_self`, `Self` resolution, default method body checking), and `T: ISeq<int>` takes effect via structural shape matching; interface instances produce no value type/itable. `interface ISeq<T>` + `sum_all<T: ISeq<int>>(T)` works end to end. Limitation: generic interface inheritance and "interface as value type" still pending M5b. Added the `generic_iface` case

**Tests**: 381/381 (86 pass + 279 must-fail + 16 check)

#### 0.0.71 — 2026-11-12

**Generic inheritance (M3f)**: `class Derived<T> : Base<T>`. Added `base_specs` (`vector<TypeSpecPtr>`, parallel to the string list) next to `bases`, filled in by the parser at the same time and handled together on clone/replace; the class branch of `instantiate_generic` supports a single base class (after instantiation it sets `ty->base` and flattens fields), and constructor `super(...)`, method inheritance and vtable/desc all follow the ordinary class path. Generic interfaces and `const` members are still unsupported. Added the `generic_inherit` case

**Tests**: 380/380 (85 pass + 279 must-fail + 16 check)

#### 0.0.70 — 2026-11-12

**Generics M3e: generic class instantiation**: `instantiate_generic` uniformly supports struct/class (fields per `field_visibility`, method/constructor/destructor signatures, `is_abstract`, vtable/desc following the ordinary class path); `check_class` wired in (context save/restore). `class Box<T>` and `Counter<T>` (with a default constructor) work end to end. Limitation: generic classes do not yet support base classes, interfaces or `const` members. Added the `generic_class` case

**Tests**: 379/379 (84 pass + 279 must-fail + 16 check)

#### 0.0.69 — 2026-11-12

**Generic constructor call syntax**: `Pair<int,long>(3, 9)` / `Vec2<int>(3, 4)`; the `type_args` branch of `check_call` looks up generic functions first, otherwise instantiates a generic struct value construction (`instantiate_generic_named`) and reuses the existing `struct_ty` construction check. Added `find_generic_template` for unified template lookup. `Pair<A,B>` and `Vec2<T>` work end to end. Added the `generic_ctor` case

**Tests**: 378/378 (83 pass + 279 must-fail + 16 check)

#### 0.0.68 — 2026-11-12

**Generics M3d: methods/constructors/destructors of generic structs**: lifted the "fields only" restriction; instantiation resolves method/constructor signatures, registers `ty->methods`/`ty->ctors`, generates symbols (`method_cname`/`ctor_cname`/`dtor_cname` run `ident_safe` on the owner, `Vec<int>` → `Vec_int_`), and runs `check_struct` to check method bodies (with checking-context save/restore, supporting nested instantiation). `Vec<T>`'s `length()/at()/fill()` and `Wrap<T>::twice()` work end to end. Limitations: generic structs still do not support `const` members or interface bases; the constructor call syntax `Pair<int,long>(...)` comes later. Added the `generic_methods` case

**Tests**: 377/377 (82 pass + 279 must-fail + 16 check)

#### 0.0.67 — 2026-11-12

**Generics M3a+M3b: interface operator members + structural constraints**: interfaces allow `T operator+(T a, T b);` (`MethodDecl::is_operator`); `Self` is no longer rewritten to the interface name but marked via `TypeSpec::is_self` + `MethodSig::ret_self/params_self`, which sema substitutes correctly when resolving interface signatures. **Constraint checking became structural**: `T: I` matches by member shape, methods look up the method table (including the base class chain / interface defaults), and operators look up built-in or visible-unit `operator`s (exact signature, no implicit conversion); `&` requires all, `|` requires any. Built-in types satisfy at zero cost (`int: IAddable` thanks to the built-in `+`). Failure diagnostics name the missing items. Added the `generic_bounds` end-to-end and `generic_bound_missing` error cases

**Tests**: 376/376 (81 pass + 279 must-fail + 16 check)

#### 0.0.66 — 2026-11-12

**Generics M2d: free function monomorphization**: call site `f<int>(x)` (`Name<` tentative parse in `parse_postfix_tail`, falling back to a comparison expression on failure); `Call::type_args`; `instantiate_func` (instance cache, signature resolution, checking-context save/restore to support nested calls, `func_cname` symbol sanitization); `mangle.cpp` applies `ident_safe` to instance names (`Box<int>` → `Box_int_`). `T max<T>(T,T)` and `T twice<T>(T)` work end to end. Added the `generic_func` end-to-end case and the `generic_arity` error case

**Tests**: 374/374 (80 pass + 278 must-fail + 16 check)

#### 0.0.65 — 2026-11-12

**Generics M2: struct monomorphization end to end**: AST deep clone (`src/ast_clone.cpp`) + type parameter substitution (`src/ast_subst.cpp`) + instantiator (`src/sema_generic.cpp`, lazy instantiation + cache + recursion guard). `struct Box<T> { T v; }`, `Pair<K,V>` and `Box<Box<int>>` all instantiate and produce an executable; the output is an ordinary struct, so `same_type` / mangle / layout / codegen are unchanged. Current limitations: generic structs support fields only (methods/construction/interfaces/`align(N)` pending M3); free function generics pending M2d. Added the `generic_struct` end-to-end case and removed the M1 "not implemented" error case

**Tests**: 372/372 (79 pass + 277 must-fail + 16 check)

#### 0.0.64 — 2026-11-12

**Generics (M1: syntax layer)**: `<T>` type parameter declarations (on free functions / structs / classes / interfaces); the type argument `Vec<int>` and the `>>` splitting of `Vec<Vec<int>>`; constraints `T: I` / `T: A & B` / `T: int | long` (mixing them errors, suggesting separate parameters); `Self` rewritten to an interface-name placeholder inside interface bodies; generic base classes `: ISeq<T>` as a placeholder; `struct S : I` syntax in place (backlog U33). Templates are moved into `Unit::templates` during the sema collection phase, neither checked nor emitted; use sites report "instantiation not implemented". The instantiation architecture (clone the AST + rename, zero codegen changes) and all decisions are in backlog U34

**Tests**: 372/372 (78 pass + 278 must-fail + 16 check)

#### 0.0.63 — 2026-11-11

**Borrowed returns + borrowed receivers (ref semantics completed)**: ①**`ref T` return type**: `ref T f()` / `ref const T f()` (free functions, struct/class methods, interface methods, operators); the borrow source must be a `ref` parameter, `this`, a global/static, or a path through pointer/slice heap storage — borrowing a local, borrowing a by-value parameter (including inline fields) and borrowing a temporary are all compile errors; the result cannot be stored (`ref T r = f();` is an error) and can only be written through in place (`f() = v`, `f() += 1`, `f().field = v`), passed as a `ref` argument, or bound by `for ref`; a `ref const` location cannot be returned as a mutable `ref`. ②**`ref` parameters relaxed to scalars**: primitive types/enums/pointers can be borrowed too (`void inc(ref int x)`), and assigning to a scalar borrow writes through; struct borrows still require writing fields under "no rebinding". ③**Borrowed receivers**: operator parameters support `ref` (`ref int operator[](ref C c, usize i)`), fixing "the value receiver writes into the copy and silently drops the write"; method borrowed returns use the implicit `this` (`ref int at(usize i) { return v[i]; }`). ④**`[]` returns a borrow**: `c[i]` is directly a writable lvalue (`=`/`+=`/taking a value/passing `ref`/`for ref` all work), so `[]=` is no longer required. ⑤**`for ref` supports user types**: the index form requires `[]` to return `ref T`. ⑥Symbol names encode the parameter borrow bit (`f(T)` vs `f(ref T)` no longer collide); `ref` in variable/field/element position gives a "binding mode is not a type" diagnostic. New cases: ref_return (1 pass), ref_return_local / ref_return_param_copy / ref_return_const / ref_return_assign_const / ref_return_extern / ref_return_void / ref_var_decl / ref_loop_const / ref_const_to_mutable (9 errors); ref_param_class / forin_ref expected text updated

**Tests**: 370/370 (78 pass + 277 must-fail + 15 check); cold build with zero warnings

#### 0.0.62 — 2026-11-11

**Language feature batch (all TODOs executed)**: ①**Type aliases** `type A = B;` (contextual keyword, parsed only when a declaration starts with `type Ident =`; one alias per declaration; transparent aliases, supporting alias-of-alias/forward references/cycle detection; unit-level + `public` across units; `TypeAliasDecl` + `UnitInfo::aliases` + lazy resolution). ②**`ref` parameters** (free functions only): `ref T p` mutable borrow, `ref const T p` read-only borrow; borrows pure value structs only (no RC fields/no destructor/not a union); the argument must be an addressable lvalue, temporaries are not accepted, and a `const` argument cannot bind a mutable borrow; no rebinding/storing/returning; on the C side a pointer is passed (the parameter slot holds a pointer, the call site passes the address); methods/constructors/`extern` explicitly error. ③**range extensions**: `..=` with an inclusive upper bound, `for i in a..b step n` (a non-zero compile-time constant; a negative step counts down; `a..b` does not reverse automatically); `i` in the index form becomes the iteration count; switch branch ranges support `..=` and `step` errors. ④**Brace range fill**: `int[10] a = {1, 2, 5..9, 10};` (endpoints must be compile-time constants, fixed-size integer arrays only, participating in zero-fill/over-length checks after expansion). ⑤**Bitfields explicitly rejected**: `int x : 3;` in struct/union/packed gives a dedicated diagnostic; U16 closed. ⑥**Docs**: `switch` extensions decided against (6.6), `ref` is a binding mode so no `T&` is introduced, `string` classified as library content (new `docs/lib-std-string.md`, SPEC keeps only the language-visible contract), class mechanics wrapped up (3.8 heading, tables 74/87 unified with 94, U9/U10/U23 closed). New cases: type_alias (1 pass + 4 errors + 1 check), ref_param (1 pass + 8 errors), range_ext / switch_incl (2 pass), bitfield (3 errors), init_range (4 errors)

**Tests**: 360/360 (77 pass + 268 must-fail + 15 check); cold build with zero warnings

#### 0.0.61 — 2026-11-11

**`const` semantics tightened (U7 closed)**: explicitly **not doing** `const` class views and `const` methods; `const` keeps only two meanings — a **read-only binding marker** on named types (locals / parameters: cannot rebind, cannot write fields directly through it; a class handle copied out is still writable, and mutating methods can still be called through it), and **pointee read-only** for pointers / slices. **Fixed `const` being silently dropped at parameter positions** (previously inside `void f(const Cat c)` rebinding and direct field writes were allowed; now consistent with a local `const`), and parameter signatures do not contain `const`, so overloading is unaffected. SPEC 3.7 / 3.8 / 3.11 / table 98 and backlog U7 synced; added 4 error cases + 1 check case

**Tests**: 333/333 (73 pass + 246 must-fail + 14 check)

#### 0.0.60 — 2026-11-11

**Source split (purely physical moves, no logic changes)**: every file ≤1000 lines. `parser.cpp`(1896) → `parser_impl.hpp` + `parser_unit/stmt/expr.cpp`; `codegen_llvm.cpp`(3912) → `codegen_impl.hpp` + `codegen_support/runtime/expr/expr2/stmt/class/func.cpp`; `sema.cpp`(4953) → `sema_impl.hpp` + `sema_decl/types/class1/class2/const/expr/call.cpp`. Approach: internal class declarations go into `*_impl.hpp` (`namespace muc_detail`), member functions are defined per TU by feature, and the public interfaces (`analyze` / `llvm_emit_*` / `parse_*`) and `main.cpp` stay completely unchanged; added `docs/code-map.md` (concept→file:function index) and file-header indexes; `AGENTS.md` gains a "code structure" convention (≤1000 lines/TU, helper function placement, default arguments only in declarations).

**Tests**: 328/328 (73 pass + 242 must-fail + 13 check); cold build with zero warnings

#### 0.0.59 — 2026-11-11

**Type-level compile-time constants + constant expression evaluation (U28/U29 closed)**: `const T NAME = <const-expr>;` inside a `struct`/`class` body is a **type-level compile-time constant** — it must be initialized on the spot, must be a constant expression, and **occupies no instance storage** (not in the field table, does not affect `sizeof`/`alignof`/layout/ABI/construction/destruction/RC); it is accessed as `Type.NAME`, can be written bare inside methods of the declaring type, and is looked up along the base class chain; `obj.NAME` / `this.NAME` / assignment / taking the address / `ref` targets all error; visibility follows `public:`/`private:` (class defaults to private), duplicate names and conflicts with fields/methods/constructors error, and union/interface do not allow it; types are limited to scalars (bool/integer/float/usize/isize) and enums. Added a **compile-time constant evaluator** (lazy + memoized + cycle detection, supporting forward references): literals, unary `- + not !`, arithmetic/bit/shift/comparison/`and`/`or`, `as`, references to unit-level `const` / enum members / type-level constants, `sizeof(T)`/`alignof(T)`; division by zero and circular dependencies error; it also takes over unit-level `const` initialization (`const int B = A + 1;`, `const int C = sizeof(int) as int;`), while string/aggregate constants keep the old path. **Instance fields no longer accept `const`** (`const int x;` → ``const member `x` must be initialized``), fixing the previous half-working state where "`&s.x` was unexpectedly `const int*`, yet `s.x = 2` was legal"; no read-only fields and no `readonly` keyword are provided. Added the const_member success case and 23 error cases; **post-audit fixes**: unsigned `>>` in constant evaluation now matches runtime (arithmetic shift by bit width, `uint 0x80000000 >> 4` = `0xF8000000`), a `use`d same-named constant no longer silently takes the first (reports ambiguity), `char` literals ≥128 are accepted by bit pattern in constant contexts, the `evaluating` flag became RAII (exception paths no longer falsely report circular), constant evaluation depth is capped at 512, diagnostics no longer print qualified type names such as `const int`, and **non-const global initializers also go through the constant evaluator** (`int g = 1 + 2;` used to fail in the backend, now works); comparisons and floating-point arithmetic round by the `usual_arith` common type (consistent with runtime). Added the const_global_init success case and 6 error cases

**Tests**: 328/328 (73 pass + 242 must-fail + 13 check); cold build with zero warnings

#### 0.0.58 — 2026-11-11

**`\u{...}` Unicode escapes + codepoint type decided**: character/string literals support `\u{` + 1–6 hex digits + `}` (the four-digit `\uXXXX` form is not supported), range `U+0000`–`U+10FFFF`, rejecting the surrogate range `U+D800`–`U+DFFF` and out-of-range values; inside a string it is UTF-8 encoded into 1–4 bytes (`\u{0}` errors just like `\0`), inside a character literal it is limited to `≤ U+FF` (beyond that suggests a string literal instead); the Unicode **codepoint type is fixed as `int`** (i32), with no `rune`/`unichar`/`char32` alias introduced; codepoint views such as `s.chars()` are left until the string class is implemented (backlog U3 deferred, strings stay byte-based: `s[i]`/`for c in s`/`len(s)` unchanged). Added the unicode_escape success case and 8 error cases (surrogate/out-of-range/non-hex/missing brace/empty hex/more than 6 digits/character beyond one byte/NUL)

**Tests**: 298/298 (71 pass + 214 must-fail + 13 check); cold build with zero warnings

#### 0.0.57 — 2026-11-11

**Diagnostics and debugging experience**: ①diagnostics carry a source snippet and a caret (`  |` + line number + `^`; tabs expanded to 4 columns, CJK counted as 2 columns for display width); ②**multiple error reporting** — global/function/class/struct/interface checks of the same unit collect errors item by item and print them together (resetting scope, loop depth and auto inference state after an error), capped at 20 per run; ③`-g` / `--debug` generates **DWARF** (compile unit, function DISubprogram, statement-level line numbers, locals and struct members), so gdb can break, step and run `info locals`. Added the multi_errors (3 errors reported at once) and dbgvars cases

**Tests**: 289/289 (70 pass + 206 must-fail + 13 check); cold build with zero warnings

#### 0.0.56 — 2026-11-11

**`main` with parameters**: supports `int main(int argc, char** argv)` (passed in by the C startup code, `argv` is null-terminated and `argv[argc] == null`); parameter types must be exactly `(int, char**)`, and 1 parameter / `char*` / a non-`int` return are all compile errors; `char**` rather than `string[]` (argv does not participate in RC). Added the main_args success case and the main_bad_argv error case, and updated the main_params expectation; SPEC 7 and §12 #15 synced

**Tests**: 287/287 (69 pass + 205 must-fail + 13 check); cold build with zero warnings

#### 0.0.55 — 2026-11-11

**C variadic calls (`...` on `extern`) + `std.cstdio`**: `extern` declarations may carry `...` (must be last, with at least one named parameter), arguments follow C default promotions (`float→double`, integer types smaller than `int`→`int`, enum→underlying integer type), and `string` automatically passes the underlying `const char*`; arguments containing RC are compile errors; custom varargs are not supported inside Mulan. Codegen uses `FunctionType::isVarArg` + `promoteVararg()`, with the ABI handled by LLVM. Also: `extern` declarations are now implicitly `public` (previously a cross-unit call was diagnosed as not public); constant initializers support unary `+`/`-` (`const int EOF = -1;`); added `libs/std/cstdio.mu` (stdio function and constant bindings). Added the varargs / cstdio success cases and 5 error cases; SPEC adds 7.1, backlog U31

**Tests**: 285/285 (68 pass + 204 must-fail + 13 check); cold build with zero warnings

#### 0.0.54 — 2026-11-10

**Unions `union`**: a POD value type, all members at offset 0, `sizeof = max(member)` rounded up to the largest member alignment; supports `packed` / `align(N)` (same rules as struct); members are limited to POD — `string`, dynamic arrays `T[]`, class/interface references, and structs with reference fields or a user destructor are forbidden; no methods/constructors/destructors/inheritance, members are always public; initialization supports only `{}` zero initialization and named `{ member = value }` (exactly one member), with no positional initialization and no `T(...)` construction; reading an inactive member reinterprets the bytes per C semantics. On the LLVM side it is represented as "alignment carrier + tail padding", and member access uniformly goes through offset 0. Added the union_basic success case and 11 error cases; SPEC adds 3.4.1, backlog U14 closed

**Tests**: 278/278 (66 pass + 199 must-fail + 13 check); cold build with zero warnings

#### 0.0.53 — 2026-11-10

**Invalid character hints switched to backticks**: `quote_char()` output changed from `'x'` to `` `x` ``, matching the code-snippet style of the other diagnostics; backslash / backtick itself / control characters are printed as `` `\` ``, `` `\`` `` and `` `\x0b` `` respectively. Added the `invalid_char` error case (previously this diagnostic had no coverage)

**Tests**: 266/266 (65 pass + 188 must-fail + 13 check); cold build with zero warnings

#### 0.0.52 — 2026-11-10

**All diagnostic messages and CLI output in English**: 787 string literals in `src/` (lexer/parser/sema/codegen/main/loader) changed to English, styled after clang/rustc — lowercase start, no trailing period, idiomatic `cannot`/`expected X but got Y`/`unknown`/`invalid`, with code identifiers still in backticks; `--help` rewritten into the three sections `Usage:` / `build options:` / `Environment:`; code comments stay in Chinese. The 187 expectation files `tests/errors/*.err` were synced to the corresponding English message fragments (mapped one-to-one by the original "Nth diagnostic"; success cases such as `strings` are unaffected)

**Tests**: 265/265 (65 pass + 187 must-fail + 13 check); cold build with zero warnings

#### 0.0.51 — 2026-11-10

**Fixed garbled Chinese in the Windows console**: at startup, if the console output codepage is not UTF-8 (Simplified Chinese systems default to 936/GBK), `muc` temporarily switches to `CP_UTF8` and restores the original value on exit via `atexit`. The Chinese messages in the source have always been UTF-8 bytes; previously a 936 console interpreted them as GBK and displayed mojibake; when output is redirected to a file or pipe the bytes are unchanged (still UTF-8)

**Tests**: 265/265 (65 pass + 187 must-fail + 13 check); cold build with zero warnings

#### 0.0.50 — 2026-11-10

**Compiler renamed `mulanc` → `muc`**: the executable target and the CMake `project()` name became `muc` (artifact `build/muc.exe`), with `--version` / `--help` output synced; environment variables `MULANC_CMD`/`MULANC_LIBS`/`MULANC_BACKEND`/`MULANC_LLVM_TRACE` became `MUC_CMD`/`MUC_LIBS`/`MUC_BACKEND`/`MUC_LLVM_TRACE`, and the compile macro `MULANC_VERSION` → `MUC_VERSION`; the test runner's default compiler became `muc`; command examples in README / SPEC / docs / AGENTS synced (historical entries in `versions.md` keep the old name). Also: added `LICENSE` (Apache-2.0) and `NOTICE`, named the language Mulan, and added SPDX headers to 17 source files; the `"mu-lan"` literal in examples and tests changed to `"Mulan"` (length/truncation expectations derived by the strings case updated to 5 / `Mu`)

**Tests**: 265/265 (65 pass + 187 must-fail + 13 check)

#### 0.0.49 — 2026-11-10

**U24: forbid taking the address of structs with reference fields**: `&s` and `&arr[i]` (the element is a struct with references) are compile errors — a raw pointer could bypass per-field retain/release; scalar fields `&s.x` are still allowed; structs with a user destructor are exempt (they can only be used through pointers anyway). `void keep(S* out, ...)` in the `dynarray` case changed to return by value; added 3 address-taking error cases, and `addr_rules` covers the allowed forms (scalar fields of RC structs, address-of on destructor structs)

**Tests**: 265/265 (65 pass + 187 must-fail + 13 check)

#### 0.0.48 — 2026-11-10

**for-in index form + user type iteration + U21 landed + lvalue fix**: ①`for i, x in seq` / `for i, ref x` (`i` is a usize, read-only inside the loop; for built-in sequences `i` is the real subscript, for ranges it is the offset relative to `lo`); a user type (struct/class) providing `length() -> usize` + `operator[](C, usize) -> T` (exact usize match) degrades to the index form, `length()` is evaluated only once, and elements are retained/released per round as owning locals; multidimensional arrays `T[m,n]`/`T[,]` explicitly disallow for-in (use an index loop + `len(a,k)` instead); `for ref` does not support user types; SPEC adds 6.4.1–6.4.4 and backlog U30. ②**U21**: forbid taking the address of class/interface references (`&c`, `&i`) and of members/elements accessed through a reference (`&c.x`, `&h.items[0]`, `&catArray[i]`, `&h.v[0]`), while pure value structs / fixed-size array elements / pointer variables are unaffected; layout2 now verifies class layout against a mirror struct model from the same source. ③Fixed `is_lvalue` misjudging local/global variables that share a name with a unit as non-lvalues (`unit t; S t = ...; t.x = 1;` reported "the left side of an assignment must be an assignable object"). Added the forin_index / unit_name_var / addr_rules success cases and 12 error cases

**Tests**: 262/262 (65 pass + 184 must-fail + 13 check)

#### 0.0.47 — 2026-11-10

**Parameters become owning locals + `del` removed**: function/method/constructor/interface method parameters are retained on entry and released on exit (covering the return/break/continue paths), and parameters can be freely rebound inside the body; `for-in` value bindings are likewise treated as owning locals (retain/release each round, covering break/continue); this fixes the memory-safety defect where "assigning `c = null` / `c = new C()` to a parameter or loop variable released the caller's object early (use-after-free) or leaked"; the `del` statement is removed (`del x;` ≡ `x = null;`, no separate semantics), with the keyword kept only for migration diagnostics; migrated `del` in all tests/examples, removed the 6 del-specific error cases and class_del_null, added the del_removed error case and the param_ownership regression case; SPEC 3.8/4/8.3 and decision table #69/#70 synced

**Tests**: 247/247 (62 pass + 172 must-fail + 13 check)

#### 0.0.46 — 2026-11-10

**Conditional expression `?:`**: `cond ? a : b`, right-associative, with lower precedence than `or`; the condition must be `bool`; only the selected branch is evaluated; the two sides converge by "same type → implicit widening → `null` adaptation", requiring `as` when none applies; the result is a prvalue (struct/string/fixed-size array retain per field, class/interface borrows from a branch get +1 while `new`/call results transfer ownership, dynamic arrays follow handle semantics), and the result slot is materialized in the entry block; the result is not an lvalue (`(c ? a : b) = x`, `&(c ? a : b)` are errors) and does not participate in compile-time constants; when both sides are `void` it can only be an expression statement; also fixed the backend `cast<IntegerType>` assertion crash on `double d = 1;` (an integer literal adapting to a floating-point type); added the ternary success case and 5 error cases; SPEC adds 5.2, #14/#104 decided

**Tests**: 252/252 (62 pass + 177 must-fail + 13 check)

#### 0.0.45 — 2026-11-10

**`switch` (no fallthrough)**: `switch expr { pattern-list: { } }`, braces optional; branch bodies require braces and get their own scope; at most one `else`, which must come last; patterns are compile-time constants (integer literals / character literals / literals with unary `-` `+` / enum members), and `a, b` matches on any of several values; ranges `a..b` are half-open and must be non-empty; enums must be exhaustive (a missing member errors; `else` is mutually exclusive with it, and full coverage warns); enums do not support ranges or implicit integer conversion; duplicate/overlapping patterns (including overlapping ranges) are compile errors; the matched value is evaluated only once; `break`/`continue` still belong to the innermost loop; in codegen dense values go through LLVM `switch` (jump table) and ranges through comparison chains; added the switch_basic success case and 15 error cases; SPEC adds 6.6, and `match` drops its reserved word

**Tests**: 246/246 (61 pass + 172 must-fail + 13 check)

#### 0.0.44 — 2026-11-10

**Explicit `this`**: `this` can be written explicitly inside class/struct methods, constructors and destructors; `this` is a reference to the current object (for structs an lvalue alias of the object itself, not a pointer), supporting `this.field` reads and writes, `this.method()`, passing as an argument and returning by value; `this` cannot be assigned, cannot have its address taken (`&this` errors) and cannot be passed to `del`; added the explicit_this success case and the this_addr / this_outside error cases, removed the struct_this error case

**Tests**: 230/230 (60 pass + 157 must-fail + 13 check)

#### 0.0.43 — 2026-11-10

**Parameter declarations may not share a type (SPEC table 26 decided)**: every parameter must write `type name` separately, so `int f(int a, b)` is a compile error; the parser recognizes the C-style shared-type form early in `parse_params` (`Ident` directly followed by `,`/`)`) and gives a clear "declare a type for each parameter" diagnostic (previously it only reported a vague "expected parameter name" at `)`); a type suffix applies to a single parameter only; shared types for multiple names in variable/field declarations are unchanged (SPEC 4.1 gains a cross-reference); added the param_shared_type error case

**Tests**: 228/228 (59 pass + 156 must-fail + 13 check)

#### 0.0.42 — 2026-11-10

**Layout modifiers `packed` / `align(N)`**: `packed` takes no argument (= `pack(1)`, no internal/tail padding, `alignof` 1), `align(N)` (a power-of-two constant, overriding overall alignment, `sizeof` rounded up to a multiple of N, field offsets unchanged, `N < natural alignment` errors), and the two are orthogonal and composable; layout has a single source of truth `type_align`/`type_size` (shared by sema and the backend); packed uses LLVM packed structs (the ordinary struct ABI is unchanged), `align(N)` inserts explicit padding and sets alignment on alloca/global/member access (packed chains always `align 1`); class objects switched to the same-source "model layout + padding" (object alignment = max(16, max field alignment), using `_aligned_malloc` when >16); slice allocation rejects elements with `align > 8` (the data area sits after the 24-byte header); added the layout / layout2 success cases and 9 error cases

**Tests**: 227/227 (59 pass + 155 must-fail + 13 check); cold build with zero warnings

#### 0.0.41 — 2026-11-10

**Empty braces `{}` zero initialization**: aggregate types (struct / fixed-size array / slice / `string`) written as `{}` at the declaration mean the zero value, and `= 0` on an aggregate errors suggesting `{}`; scalars (numeric/bool/char/pointer/class reference) written as `{}` error, suggesting their own constants (`0`/`0.0`/`false`/`null`); in codegen empty aggregates use `zeroinitializer` / zeroing, and empty globals use zero constants; added the empty_brace success case and the brace_scalar / zero_aggregate error cases

**Tests**: 216/216 (57 pass + 146 must-fail + 13 check)

#### 0.0.40 — 2026-11-10

**Qualified enum member references**: `Color.Red` is equivalent to `Red`, and cross-unit `unit.Color.Red` can be written; sema resolves the type name at the member access site (current unit → `use`d public → std.string), members are constants (cannot be assigned/taken address), and `T.m` on a non-enum type errors; codegen folds the node directly into a constant as `enum_const`; enum_cmp and vis_import_ok cover qualified references, and 4 error cases were added

**Tests**: 213/213 (56 pass + 144 must-fail + 13 check)

#### 0.0.39 — 2026-11-10

**Enums become nominal types**: no implicit conversion to or from integers in either direction (both need `as`); the same enum supports `== != < <= > >=` (order = underlying integer value); comparing across enums, enum arithmetic/bitwise operations/unary `-+~`/compound assignment, and subscripts/range endpoints using enums all error; `usual_arith` rejects enums, and codegen emits `icmp` using signed int order for comparisons; added the enum_cmp end-to-end case and 8 error cases, migrated extras/enumfield/vis_import_ok

**Tests**: 209/209 (56 pass + 140 must-fail + 13 check)

#### 0.0.38 — 2026-11-10

**Bitwise NOT changed to `~`**: consistent with C, and `operator~` overloading is supported (1 parameter); `!` is no longer used and gives a migration hint; migrated the bitwise case, added the op_neg end-to-end and bang_removed error cases

**Tests**: 200/200 (55 pass + 132 must-fail + 13 check)

#### 0.0.37 — 2026-11-10

**++/-- uniformly error in expression contexts**: function arguments, if/while conditions, return, initializers, array subscripts, `new` arguments and parentheses all give a clear "no return value" hint; added the inc_call_expr / inc_return_expr error cases

**Tests**: 198/198 (54 pass + 131 must-fail + 13 check)

#### 0.0.36 — 2026-11-10

**Forbid `*(p++)`**: `++`/`--` have no return value, so appearing inside parentheses also reports a clear error; `*p++` still parses as `(*p)++`; added the inc_paren_expr error case and covered `*p++` in inc

**Tests**: 196/196 (54 pass + 129 must-fail + 13 check)

#### 0.0.35 — 2026-11-10

**++/-- statement support**: prefix/postfix `++`/`--` are only allowed as standalone statements (no return value, cannot appear in expressions); semantically lowered to `x += 1` / `x -= 1`; `i++; j++;` is legal; added the inc end-to-end and inc_expr error cases

**Tests**: 195/195 (54 pass + 128 must-fail + 13 check)

#### 0.0.34 — 2026-11-10

**Explicitly no `T* const`**: `const` is only a base-type prefix (value/pointee read-only), and neither pointer-binding read-only nor per-level const is introduced; SPEC 3.7 and #25 changed to "not provided" with the rationale

**Tests**: 193/193 (53 pass + 127 must-fail + 13 check)

#### 0.0.33 — 2026-11-10

**const changed to the C model**: removed `readonly`; `const` becomes a type qualifier (a prefix on the base type, `const int**` means the innermost level is read-only); `T* → const T*` is implicit while the reverse needs an explicit `as`; write checks and implicit conversions are decided per const level; migrated `readonly` → `const` across the whole repo; `T* const` is not supported for now

**Tests**: 193/193 (53 pass + 127 must-fail + 13 check)

#### 0.0.32 — 2026-11-10

**Removed `static`**: unit-level variables are declared directly (private by default, `public` across units); `extern` is kept; global variable linkage is decided by public/private; using `static` at top level or inside functions gives a clear migration error; migrated all cases and examples, removed unit_var_bare

**Tests**: 193/193 (53 pass + 127 must-fail + 13 check)

#### 0.0.31 — 2026-11-10

**string rewritten as an object**: `string` stays a value-type struct (canonical layout/literals/`s[i]`/`len`/`==`/for-in degradation unchanged) with private fields; added the constructor `string(readonly char*)` and the member methods `length/empty/is_empty/at/cstr/sub/concat/find/starts_with/ends_with/trim`; removed the `str_*` free functions, with `operator+` thinly wrapping `concat`; relaxed `= 0` to allow zero-value initialization of structs that have constructors; added the string_methods end-to-end case, removed struct_ctor_zero

**Tests**: 194/194 (53 pass + 128 must-fail + 13 check)

#### 0.0.30 — 2026-11-10

**public/private replace pub**: private = inside the unit, public = across units; struct members default to public and class members to private; struct/class support `public:`/`private:` section labels and per-member prefixes; struct fields can be private, and cross-unit aggregate initialization requires all fields to be public; migrated the whole repo and added 4 cases

**Tests**: 194/194 (52 pass + 129 must-fail + 13 check)

#### 0.0.29 — 2026-11-10

**C#-style import visibility**: after `use`, `pub` symbols can be used without a prefix (types/functions/global variables/enum constants/struct construction); local declarations shadow imports; the same name from multiple sources reports ambiguity at the use site and requires a qualified name; function overloads merge across units; the parser tentatively supports unqualified type names by declaration shape; added 6 import cases

**Tests**: 191/191 (52 pass + 128 must-fail + 11 check)

#### 0.0.28 — 2026-11-10

**Multi-segment unit name qualification**: the expression `lib.a.b.f()` is folded into a single Name by the longest imported unit prefix; the type `lib.a.b.T` is matched by the longest prefix in sema; the parser's type parsing supports dots at any segment; `std.string` is treated as implicitly `use`d; added 2 check cases

**Tests**: 186/186 (52 pass + 125 must-fail + 9 check)

#### 0.0.27 — 2026-11-10

**Visibility mechanism**: added the keyword `pub`; top-level declarations / class fields and methods / constructors are visible only inside the unit by default, and `pub` makes them cross-unit; `pub unit` exports across libraries; the library boundary = the first segment of the unit name (a cross-library `use` needs `pub unit`); `pub` signature propagation checks; non-`pub` symbols get LLVM `internal` linkage; migrated std.string/math/lib_* and added 11 visibility cases

**Tests**: 184/184 (52 pass + 125 must-fail + 7 check)

#### 0.0.26 — 2026-11-09

**Standard library directory and type rename**: added the `libs` default library search path (entry directory, `-I`, `MULANC_LIBS`, `libs`/`libs/std` in the current directory and its ancestors); `libs/std/string.mu` (unit `std.string`) replaces the compiler-embedded prelude; `unit`/`use` support dotted names (`std.string` → `libs/std/string.mu`); the string struct renamed from `Str` to `string`; tests/examples/docs synced

**Tests**: 172/172 (52 pass + 115 must-fail + 5 check)

#### 0.0.25 — 2026-11-09

**String changed to the std.string.string struct, built-in `string` removed**: the struct `string { byte* p; usize n; byte[] owner; }` in `libs/std/string.mu` (unit `std.string`) + the string library; the literal type is string (owner=null, rodata, zero allocation/zero RC); `s[i]`/`len`/`==`/for-in degrade using the built-in canonical layout; copies/fields/returns go through U24 per-field RC; std.string provides `str_from_cstr`/`str_cstr`/`str_sub`/`str_concat`/`operator+` and methods; removed the `string` keyword from lexer/parser/sema and migrated all tests and examples; added the `str_struct` case; runs the standard LLVM `-O2` pipeline after generating IR

**Tests**: 172/172 (52 pass + 115 must-fail + 5 check)

#### 0.0.24 — 2026-11-09

**Dynamic arrays `T[]` + multidimensional arrays**: `new T[n]` creates a heap array with an RC header (an 8-byte handle, O(1) copy sharing); static rectangles `T[m,n]` (nested fixed-size arrays, `a[i,j]`/`a[i][j]`), dynamic rectangles `T[,]` + `new T[m,n]`, and jagged `T[][]` + `new T[m][]` (one dimension per `new`, rows filled in a loop); `len(a)`/`len(a,k)` (literal axes for static, runtime-capable for dynamic); `T[N]→T[]`, `T[m,n]→T[,]` allocating copies; elements may be class/interface and structs with references, released per element; a null handle has len=0; `extern` recursively rejects `T[]`; structs with destructors are still forbidden as elements; removed the class/interface slice prohibition and the old borrowed-view semantics

**Tests**: 171/171 (51 pass + 115 must-fail + 5 check)

#### 0.0.23 — 2026-11-09

**U24: struct reference fields + per-field RC**: structs may contain class/interface fields (including arrays/nesting); copies/passing retain, assignment retains the new value before releasing the old, and destruction releases per field/per element; a returned value turns a borrow into owned; a temporary struct created by a call is released at the end of the statement; uninitialized local structs are zeroed automatically; extern/globals recursively rejected; structs with a user destructor still cannot be copied; added the `struct_rc_fields` success case + 2 error cases, removed the outdated "structs cannot contain class fields" case

**Tests**: 163/163 (49 pass + 109 must-fail + 5 check)

#### 0.0.22 — 2026-11-09

**struct construction/destruction**: constructor overloading, `S s;` default construction, `S s = S(args)` in-place construction; destructors are called automatically on scope exit/return/break/continue; structs with destructors are temporarily forbidden to be copied/passed/returned by value/in arrays/fields/globals; added 1 success case + 11 error cases

**Tests**: 161/161 (48 pass + 108 must-fail + 5 check)

#### 0.0.21 — 2026-11-09

**struct methods**: static dispatch, implicit `this`, method overloading, array/field receivers, implicit method calls; methods may not share a field's name; explicit `this` not supported yet; added 1 success case + 4 error cases

**Tests**: 149/149 (47 pass + 97 must-fail + 5 check)

#### 0.0.20 — 2026-11-09

Converged 8-bit types per the final decision: `char`=i8 (C style), `byte`=u8 and the only unsigned 8-bit name (**removed the `ubyte`/`uchar` aliases**); `s[i]`/string for-in return `byte`; character literals default to `byte` and adapt to `char`/`byte` by context; added an alias-removal case

**Tests**: 144/144 (46 pass + 93 must-fail + 5 check)

#### 0.0.19 — 2026-11-09

**`char` changed to signed 8-bit (C style); `byte` changed to unsigned 8-bit, with `ubyte`/`uchar` as its aliases (the same type)**; `'\xNN'` is interpreted by bit pattern; added char/byte range error cases

**Tests**: 143/143 (46 pass + 92 must-fail + 5 check)

#### 0.0.18 — 2026-11-09

Clarified character literal semantics: `char` is an unsigned 8-bit single byte and non-ASCII literals are compile errors; SPEC gains item 101; added the `char_bytes` (UTF-8 byte view) and `char_nonascii` cases

**Tests**: 141/141 (46 pass + 90 must-fail + 5 check)

#### 0.0.17 — 2026-11-09

Check that foreign C calls cannot pass class/interface; added a case of an interface returning an interface and a comprehensive case (interface arrays/for-in/downcast/weak references)

**Tests**: 139/139 (45 pass + 89 must-fail + 5 check)

#### 0.0.16 — 2026-11-09

Added regression cases for single evaluation of comparisons/subscripts and temporary release (verifying the double-evaluation fix in 0.0.14)

**Tests**: 136/136 (43 pass + 88 must-fail + 5 check)

#### 0.0.15 — 2026-11-09

SPEC formally downgraded: `readonly` class views, `ref` parameters and interface→derived-interface downcasts are listed as unsupported in phase one (tables 98–100); added checks that `extern` cannot be overloaded/cannot share a name with an ordinary function, and that `main` cannot be overloaded

**Tests**: 135/135 (42 pass + 88 must-fail + 5 check)

#### 0.0.14 — 2026-11-09

**weak targets auto-null on destruction**: a global weak registry (target, field address) + spinlock; assignment registers, overwriting/destruction of the holder unregisters, and the target's destroy zeroes them; also fixed repeated evaluation in binary comparisons (a function call executed twice inside a comparison, leaking temporary references)

**Tests**: 132/132 (42 pass + 85 must-fail + 5 check)

#### 0.0.13 — 2026-11-09

Cleared the last warning in a cold build (unused variable); cold build has zero warnings

**Tests**: 131/131 (41 pass + 85 must-fail + 5 check)

#### 0.0.12 — 2026-11-09

`super.method` resolves only along the class chain (calling an interface default method through super errors); `del null` is a no-op; multi-unit interface/inheritance check cases (lib_ dependencies skipped); error case for compound assignment to a subscript

**Tests**: 131/131 (41 pass + 85 must-fail + 5 check)

#### 0.0.11 — 2026-11-09

Class field resolution changed to a topological order based on base class dependencies (a base class may be declared later in the same unit and field layout is still correct); added forward base class and interface array cases

**Tests**: 127/127 (40 pass + 83 must-fail + 4 check)

#### 0.0.10 — 2026-11-09

Per-element RC for arrays of references: copies retain, overwrites/scope exit release, and by-value returns and temporary arguments transfer per element (fixing early release/leaks of shared arrays); added 2 end-to-end cases

**Tests**: 125/125 (38 pass + 83 must-fail + 4 check)

#### 0.0.9 — 2026-11-09

Fixed a leak where an operator/subscript overload returning a reference was treated as a borrow; override return types are compared against all base class/interface declarations (conflicts error); added 2 cases

**Tests**: 123/123 (36 pass + 83 must-fail + 4 check)

#### 0.0.8 — 2026-11-09

Tightened `as`: pointers/slices no longer convert to each other, and slice conversion requires identical element types; added 2 error cases

**Tests**: 121/121 (35 pass + 82 must-fail + 4 check)

#### 0.0.7 — 2026-11-09

When a constructor omits `super(...)`, the base class no-arg constructor is called implicitly (C++ semantics); added a linked list case (strong next + weak prev) and cases for nested `new`/base class arrays

**Tests**: 119/119 (35 pass + 80 must-fail + 4 check)

#### 0.0.6 — 2026-11-09

`as` downcast: class→derived class and interface→class are checked at runtime along the desc base class chain, yielding null on failure; desc gains base_desc; added the downcast end-to-end case

**Tests**: 116/116 (32 pass + 80 must-fail + 4 check)

#### 0.0.5 — 2026-11-09

`new`/`len`/`sizeof`/unary expressions can be followed by postfixes (`new C().f()`, `len(s) as int`); `sizeof` supports class/interface/enum; class pointers, `del` on operator subscripts and `return` inside destructors are forbidden; identity comparison between related reference types; added 4 end-to-end cases and 3 error cases

**Tests**: 115/115 (31 pass + 80 must-fail + 4 check)

#### 0.0.4 — 2026-11-09

Local reference cleanup (block/return/break/continue), temporary reference release, interface default methods dispatched through class variables, interface array layout fixed (16 bytes), `del` restrictions on parameters/loop variables; added 4 end-to-end cases

**Tests**: 107/107 (26 pass + 77 must-fail + 4 check)

#### 0.0.3 — 2026-11-09

Object model end to end: semantics and LLVM codegen for class/interfaces/inheritance/construction and destruction/virtual dispatch/reference counting/`weak`; overloads and operator dispatch; added 4 end-to-end class cases, 21 error cases and 4 check cases

**Tests**: 103/103 (24 pass + 75 must-fail + 4 check)

#### 0.0.2 — 2026-11-08

Removed the C99 backend, making LLVM the only backend; cleaned up `--emit-c`/`-DMULANC_WITH_LLVM`; version plumbing wired into `--version` and the LLVM module flags

**Tests**: LLVM backend 58/58; cold build with zero warnings

#### 0.0.1 — 2026-11-08

Baseline: C++20 compiler (lexer/parser/sema), C99 backend, LLVM backend (LLVM C++ API → COFF), 58 regression cases, reference/object model design discussion

**Tests**: C backend 58/58; LLVM backend 58/58; cold build with zero warnings
