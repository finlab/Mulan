# Version history

## Rules

- Version format: `major.minor.build`, starting from **0.0.1**.
- **After each official build passes the full test suite, the build number +1**; recorded in this file by the maintainer.
- **Only changes to the compiler code raise the build number**: documentation-only, comment-only or config/CI-text changes are recorded inside the current entry's follow-up paragraph instead of bumping the version.
- **Major / minor version numbers are raised only on explicit request**, otherwise they stay unchanged.
- The single source of truth for the version number is the `VERSION` file at the repository root; CMake, the compiler `--version` and the generated C code all read from it.

## Releases

### 0.0.113 — 2026-11-14

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

### 0.0.112 — 2026-11-14

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

### 0.0.111 — 2026-11-14

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

### 0.0.110 — 2026-11-14

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

### 0.0.109 — 2026-11-14

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

### 0.0.108 — 2026-11-14

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

### 0.0.107 — 2026-11-14

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

### 0.0.106 — 2026-11-14

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

### 0.0.105 — 2026-11-14

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

### 0.0.104 — 2026-11-14

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

### 0.0.103 — 2026-11-14

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

### 0.0.102 — 2026-11-14

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

### 0.0.101 — 2026-11-14

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

### 0.0.100 — 2026-11-13

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

### 0.0.99 — 2026-11-13

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

### 0.0.98 — 2026-11-13

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

### 0.0.97 — 2026-11-13

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

### 0.0.96 — 2026-11-13

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

### 0.0.95 — 2026-11-13

**Compiler-facing contracts are explicit interfaces (`libs/core`)**: a new `core.iter` unit (`libs/core/iter.mu`, loaded implicitly like `std.thread`) declares the `for-in` contracts as real interfaces — `IIterator<T>` (`bool next()`, `T current()`) and `IIndexable<T>` (`usize length()`; the subscript half stays structural because an interface cannot declare `operator[]`). Nothing changes semantically: the compiler keeps checking the protocol structurally (no vtable, no boxing), but the contract now has a definition site, so an implementer can `use core.iter;` and declare `: IIterator<T>` to have their cursor checked against the interface. SPEC 6.4.2 now names the contract and records the stream clauses (`next()` may block, it is a park/cancellation point in a task, one consumer per cursor, value elements).

**Tests**: 456/456 (124 pass + 314 must-fail + 18 check)

### 0.0.94 — 2026-11-13

**`para for` over arrays/slices/strings (concurrency P2b, part 2)**: non-range sources now work by reusing the ordinary `for-in` lowering once per chunk with a **stride override** (two new Emitter fields, `forChunkStart`/`forChunkStride`): chunk k visits indices `k, k+8, …`, so fixed arrays, slices, strings and user types that satisfy the index form all work without a second code path, and the loop variable keeps the ordinary per-iteration binding (value or `ref`, with element retain/release). The sequence must be a simple variable or member access, because each chunk evaluates it (explicit diagnostic otherwise); `for i, x` is still rejected, and the cursor form of `for-in` remains unimplemented, so a blocking stream cannot be a `para for` source yet. Tests: `para_for_array` (Σ 3·0..7 = 84); the range test and the `break` must-fail case are unchanged.

**Tests**: 456/456 (124 pass + 314 must-fail + 18 check)

### 0.0.93 — 2026-11-13

**`para for` over ranges (concurrency P2b, part 1)**: `para for i in a..b { ... }` (including `..=` and `step`) is implemented by splitting the range into a fixed number of **strided chunks (8)** and running one OS thread per chunk; each chunk is a normal loop over a private copy of the loop variable, so the body keeps the same capture rules as `para` (captures travel through the shared read-only frame) and `break`/`continue` at the body level are compile errors (`para_for_break`). The chunk body and its exception wrapper are queued like `para` sections and emitted after the enclosing function; the queue is drained until empty because a body may itself contain a parallel region. Not yet: the non-range sources (arrays/slices/strings) and the cursor form of `for-in`, so `para for` currently rejects anything that is not a range (`para_for_array`). Tests: `para_for_basic` (sum of squares 0..15 = 1240) plus two must-fail cases.

**Tests**: 456/456 (123 pass + 315 must-fail + 18 check)

### 0.0.92 — 2026-11-13

**`para` (concurrency P2a)**: `para { { ... } { ... } }` is implemented with one OS thread per section (`src/parser_stmt.cpp`, `src/sema_class1.cpp`, `src/codegen_stmt.cpp`). Each section becomes a hidden function that receives a frame of **pointers to the caller's captured locals** — the caller is suspended at the join, so nothing is copied and no reference count is touched; a nested `para` forwards captures through every enclosing frame (a capture is recorded in all active sections, so the inner frame can reach the outer function's slot through the outer frame). The compile-time rules of SPEC 3.17.2 are enforced: writing an enclosing local, taking its address, passing it by `ref`, `return`, and `break`/`continue` that would leave the section are errors, while writes through a class handle, a slice buffer or a pointer are allowed (explicit sharing). v1 limitations are recorded in SPEC 3.17.7: sections are joined in source order with no cancellation, an exception that escapes a section aborts the process (the B2 thread-boundary rule) instead of being rethrown, and `this` / implicit field access inside a section is a compile error. `std.thread` is now loaded implicitly because it provides the two lowering entry points (`muc_para_start` / `muc_para_join`); a program without `para` never starts the runtime. Tests: `para_basic` (two sections, captured local, results through a class), `para_nested` (nested sections) and five must-fail cases (`para_write_outer`, `para_return`, `para_addr_outer`, `para_ref_outer`, `para_this`).

**Tests**: 453/453 (122 pass + 313 must-fail + 18 check)

### 0.0.91 — 2026-11-13

**Thread primitives: `std.thread` + platform search path (concurrency P1)**: the loader now puts `libs/std/platform/<os>` ahead of `libs` (`src/loader.cpp`), so a unit can have a platform-specific implementation chosen at load time without a preprocessor. The first user is `libs/std/thread.mu` — a platform-neutral API (`Thread`, `thread_start/join/detach/sleep`, `monotonic_ms`, `Mutex`, `CondVar`, `WAIT_INFINITE`) — with the Win32 implementation isolated in `libs/std/platform/windows/std/thread/backend.mu`: `CreateThread` with a `(void*) => uint` trampoline over a heap context, `WaitForSingleObject`, `Sleep`, `GetTickCount64`, and SRWLOCK / CONDITION_VARIABLE (both pointer-sized and zero-initialized, so a zeroed `Mutex`/`CondVar` is immediately usable — no init/destroy call). The API is deliberately C-shaped (function values plus `void*` userdata, no captures) and is the layer the task runtime will be built on; the front-end only knows the portable names. Two new tests: `thread_smoke` (4 threads × 1000 mutex-protected increments, deterministic 4000) and `rc_stress` (50k producer/consumer handoffs through a size-1 slot: every object is created on the producer and destroyed on the consumer, count 50000, which is exactly the cross-thread RC path the 0.0.90 ordering fix addresses). Both tests use only the portable API, so they keep working once a POSIX/macOS backend exists; Win32 is the only backend today.

**Tests**: 446/446 (120 pass + 308 must-fail + 18 check)

### 0.0.90 — 2026-11-13

**RC memory ordering corrected for cross-thread sharing (concurrency P0)**: the strong reference count used `monotonic` for both the increment and the decrement and destroyed the object as soon as the decrement observed 1 — the classic relaxed-RC bug: the thread that releases the last reference need not see what the other threads wrote before dropping theirs. The count now follows the Arc pattern: increments stay **relaxed**, the decrement is **release**, and the destroy path executes an **acquire fence** before loading the class descriptor (`src/codegen_support.cpp:ensureRelease`). The dynamic-array/slice header release got the same treatment (`src/codegen_runtime.cpp:emitSliceRelease`), and the global weak-registry spin lock now uses **acquire** on lock and **release** on unlock instead of a relaxed exchange (harmless on x86-TSO, wrong on ARM). Verified by inspecting the emitted IR: `atomicrmw sub ... release` + `fence acquire` inside the destroy block, `atomicrmw add ... monotonic` for retains, `atomicrmw xchg ... acquire/release` for the weak lock. SPEC 3.8/8.3 and questions 74/87/94 updated from "relaxed" to the precise ordering. The test count does not change: a single-threaded program cannot observe the difference, and the cross-thread stress test arrives with the task runtime (P1), because the language has no thread or mutex primitives yet.

**Tests**: 444/444 (118 pass + 308 must-fail + 18 check)

### 0.0.89 — 2026-11-13

**`string` is now a pure library type (no compiler knowledge at all)**: a string literal is a static, NUL-terminated UTF-8 buffer whose type is **`const char*`** (as in C), and `string s = "abc";` goes through the library's `implicit string(const char*)` (0.0.88). Every operation moved into `libs/std/string.mu` as ordinary code: byte length is `s.length()`, indexing is the **member** `operator[]` (borrowed receiver: no copy, no RC, 0.0.87), `for c in s` uses the index form, `==`/`!=` are library operators with overloads for `string` and `const char*` (`"abc" == s` works too), `+` is a member operator, and `s.cstr()` replaces the implicit `string → const char*` conversion. `TypeKind::String` was deleted, so codegen no longer knows the `{p, n, owner}` layout: `sizeof(string)` now comes from the resolved type (it used to answer 8 in expressions and 24 in constants) and `stringConst` only emits a `const char*` to the literal bytes. Consequences, all documented in SPEC 3.3: `string` cannot be a unit-level variable any more (reference-counted data has no exit-time destruction — use `const char*`), `printf("%s", s)` needs `s.cstr()`, and `len(s)` is a compile error (`len` serves built-in sequences only). A `char[N]` may still be initialized from a literal (byte copy + NUL padding, now with a length check). Added 2 run cases (`string_library` plus the migrated string cases), 2 must-fail cases (`string_global`, `string_no_implicit_cstr`).

**Tests**: 444/444 (118 pass + 308 must-fail + 18 check)

> **Doc-only change (2026-11-13, no version bump)**: added SPEC 3.17 (concurrency: `para` / `para for` /
> `thread`) together with the keyword table (2.3) and reserved-word list (2.5) updates, the UB
> entry for data races (8.2), the concurrency non-goals (10), open questions #120–125 (12) and a roadmap
> note (14). The decision record, invariants, library contracts and phasing live in
> `agents/task-runtime.md`. No compiler code changed and the test baseline is unchanged.

### 0.0.88 — 2026-11-13

**Implicit struct construction (`implicit T(one_param)`)**: a `struct` constructor marked `implicit` is now the conversion path used at the coercion points (initialization, assignment, argument passing, `return`). It is struct-only (a class conversion would allocate implicitly), must be public, exactly one parameter and not the struct itself; at most one step and matched by the built-in conversions, so `string s = cstr;` will resolve to `implicit string(const char*)` while `f(int)` still beats `f(Wrap)` for an `int` argument (rank: exact 0 > widening 1 > implicit construction 2). It does not apply inside expressions, and it costs exactly what the constructor body costs — this is the missing piece for moving `string` out of the compiler (0.0.89). Codegen materializes the value with the constructor call (`src/codegen_expr.cpp:coerce`); `implicit` is a contextual keyword (a modifier only in front of a constructor name), and writing it in a class body is a compile error. Added 1 run case (`implicit_ctor`), 3 must-fail cases.

**Tests**: 441/441 (117 pass + 306 must-fail + 18 check)

### 0.0.87 — 2026-11-13

**Member operators on `struct` (borrowed receiver)**: a struct may now declare `T operator+(B other)`, `T operator[](usize i)`, `void operator[]=(usize i, V v)`, unary `-`/`+`/`~`, and the comparison/bitwise set as **member** functions (`src/parser_unit.cpp`, `src/sema_types.cpp:check_member_operator_sig`, `src/sema_call.cpp:find_member_operator`). The receiver is the implicit `this`, i.e. a borrowed pointer, so `s[i]` / `s == t` copy nothing and touch no reference count — which is the prerequisite for moving `string`'s operators into the library at built-in cost. Member operators are static-dispatch only and **forbidden in classes** (a class operator would be virtual and would break `==` identity); a member operator wins over a free function with the same signature, and falls back when no overload accepts the arguments (`-s` vs `s - t`). The index-form `for-in` protocol now also accepts a member `operator[]` (and prefers it), `a != b` converts through a member/free `operator==` with the result negated, and the `T: I` constraint check accepts operator members as well. Added 1 run case (`member_op`), 3 must-fail cases and 1 check case.

**Tests**: 437/437 (116 pass + 303 must-fail + 18 check)

### 0.0.86 — 2026-11-12

**Out-of-range shift counts in constant expressions + stale status text**: a shift by ≥ the bit width (and a negative shift count) inside a constant expression is now reported directly (`shift count 40 is out of range for `int` (0..31)`) instead of returning "not evaluable", which let `check` pass and then failed in the backend with "global initializer must be a constant" pointing at a standard-library line. Valid shifts (`1 << 31`, `(1 as long) << 40`) still evaluate. Documentation: the non-goal list no longer claims dynamic arrays are unimplemented (they are built in; only library container classes are missing), the two duplicated open-question numbers (110/111) were renumbered so the table runs 110–119, the `deinit` row now points at the implemented `~T()` destructor, and backlog U33 is marked implemented (S1 static satisfaction and S4 constraint dispatch, with the S3/S5 dynamic channel closed). Added 2 must-fail cases (`shift_count_overflow`, `shift_count_negative`).

**Tests**: 432/432 (115 pass + 300 must-fail + 17 check)

### 0.0.85 — 2026-11-12

**Signed division overflow in constant expressions, test-runner hardening, documentation drift**: `MIN / -1` and `MIN % -1` are now rejected in a constant expression (both 32-bit and 64-bit) with an explicit diagnostic instead of silently wrapping for `int` and reporting "not a compile-time constant" for `long`. The specification records division/remainder overflow as UB (like division by zero) and keeps the two's-complement wraparound promise for `+`/`-`/`*` only — the CPU traps on the division, so a runtime `MIN / -1` is UB by contract; `-x` wraps. `tests/run_tests.py` now discovers `main` in `examples/*.mu` by scanning code with comments and string/char literals stripped (comments or formatting no longer hide or fake an entry point) and deletes only the artifacts of the current invocation (`build/<case>.o`, `build/tests/<case>.exe`) instead of globbing `build/`, so a developer's own `.o`/`.ll` files survive a test run. Documentation: dropped the stale `!` operator from the constant-expression lists, corrected `tests/README.md` (`~` is the complement, `const` instead of `readonly`, no `++` expression), narrowed the `const`-on-function-type rule to named aliases (with a parser hint), and refreshed the SPEC header version plus the enum section status. Added 2 must-fail cases (`div_overflow_int`, `rem_overflow_long`).

**Tests**: 430/430 (115 pass + 298 must-fail + 17 check)

### 0.0.84 — 2026-11-12

**Function types and non-capturing lambdas (SPEC 3.16)**: added `(T1, T2) => R` as a one-word function value — binary compatible with a C function pointer, no declarators and no `*`, `sizeof`/`alignof` = 8, `=>` binds lowest and is right-associative (`*`/`[]` belong to the return type). A free function name is already a value: `&f` is rejected with a hint, overloaded names are resolved by the target type, and generic function names plus methods are not values. Added the `Lambda` expression `(int a) => expr` / `(int a) => { ... }`: parameter types are written, the return type is inferred from the `return` expressions (all must be identical, `null` adapts; no `return <value>` means `void`; a non-void lambda must return on every path), it lowers to a hidden `private` function and allocates nothing; captures (an enclosing local, `this`, an enclosing lambda parameter) are compile errors that point at `class` + single-method `interface`. Calls through a function value work from locals, parameters, struct/class fields, array elements and unit-level variables; a unit-level initializer may name a function (an address constant, emitted as the symbol address); `void*` ↔ function type follows the pointer rules (`as` required back). Replaced the obsolete `func_as_value` must-fail case with `fnptr_assign_to_int`; added 2 run cases (`fnptr`, `lambda`), 7 must-fail cases and 1 check case (`fn_type`).

**Tests**: 428/428 (115 pass + 296 must-fail + 17 check)

### 0.0.83 — 2026-11-12

**Conservative module-local may-throw optimization**: added `src/may_throw.cpp`, a whole-program fixpoint over visible function, constructor and struct-method bodies. A call whose callee is proven not to return with a pending exception skips the B2 pending-flag check; free functions, operators, constructors, local struct construction, `new`, `super(...)`, `for-in` `length()`/`operator[]`, and struct methods use the summary. Class/interface dispatch, external/precompiled/static-library/dynamic-library and otherwise unknown targets are conservatively may-throw, so the optimization does not constrain the future library ABI. Catch barriers are over-approximated; destructors and `finally` are nothrow (an escaping exception aborts). Added 3 runtime cases (nothrow free/recursive/struct-method code, may-throw struct methods, and a cleanup-free struct constructor that throws).

**Tests**: 418/418 (113 pass + 289 must-fail + 16 check)

**Docs only (no code change, version not bumped)**: added SPEC 3.16 — function types `(T1, T2) => R` (a one-word value, binary compatible with a C function pointer, no declarators and no `*`) and non-capturing lambdas `(int a) => expr` / `(int a) => { ... }` with a return type inferred from the body; block- and unit-level declarations plus return types never use an anonymous function type (`auto` for locals, a `type` alias elsewhere), while fields and parameters may; a free function name is already a function value (`&f` is rejected) while methods are not values. Cross-references updated in 2.3, 3.2, 7, 10, 11 and 12 (#10, #116–#119); backlog U35 records the decisions, the rejected alternatives and the future two-word `delegate`.

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
