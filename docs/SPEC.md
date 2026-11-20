# Mulan Language Specification (Draft v0.2)

> Date: drafted on 2026-11-07; the object model (class / single inheritance / interface / overload / constructor) was added on 2026-11-09;
> 2026-11-14: aligned with the current implementation — string literals are `const char*` (0.0.89), `len` is
> removed and static arrays are C storage (0.0.101), unit-level init/cleanup (9.1), `export` and the export
> surface (0.0.104), arrays decay to pointers (C99, 0.0.106).
> **2026-11-17: `const` redesigned on the C++ model (3.7), `move` defined as `&&` (3.8/4.4), and the
> `comptime` unit with `builtin` declarations added (9.3); `constexpr`/`consteval`/`constinit` stay out.**
> **2026-11-20: `type` declares only transparent aliases; tagged unions are declared with `variant` and
> dispatched with `match` (implemented in build 258); interface extension uses `extend`; a field's public
> methods are forwarded with `promote` (10.1, 3.18, 3.19); all four are implemented (build 261).**
> Status: draft, current through build 278 (2026-11-20; the per-build change log is docs/versions.md,
> where versions up to 0.0.144 used a single `major.minor.build` number). The text distinguishes
> "decided", "recommended", and "undecided"; items that are not frozen can be discussed one by one.
> Items marked **decided <date>** are frozen by design; when an item also says "not implemented yet"
> (for example the 2026-11-20 syntax surface), the compiler still has to catch up.
> Positioning: a compact, modern, C-like small language; it retains the core capabilities of C; manual memory management + reference counting for heap objects.
> Non-goals for phase one: multiple inheritance, closures, GC, a preprocessor (no macros or textual substitution; `#if` only selects code at build time, see 9.2), reflection. (Generics have landed, see 3.15; exceptions have landed, see 3.14; destructors are specified in 3.13.)
>
> **Scope: this document specifies the *language*.** The standard library — unit inventory, APIs,
> error model, C bindings, naming — is specified separately in
> [docs/StdLibSPEC.md](file:///D:/dev/mu_lan/docs/StdLibSPEC.md). Sections here that describe a
> library contract (§3.15.9, §3.17, §7.2) keep the language rule and point at the library spec;
> section numbers are unchanged on purpose, because source comments and tests cite them.

---

## 0. Summary of Discussion Conclusions

| Topic | Conclusion |
|---|---|
| Loops | `while`, `for-in` and C-style `for (init; cond; step)` (implemented 2026-11-19, see 6.3.1); `do-while` is not kept |
| Iteration | `for x in seq { }` (parentheses optional); ranges `a..b` / `a..=b` (optionally `step n`) and managed sequences (`T[]` / `T[,]` / `String` and library containers that provide `length()` + `operator[]`) are supported; static arrays are C storage and are not iterable; `continue` proceeds to the next iteration |
| Mutability | C++ model (decided 2026-11-17): mutable by default; `const` is a **cv qualifier that is part of the type**, at every level (top-level and low-level), participating in overloads and symbol names; `const` methods are provided; `constexpr`/`consteval`/`constinit` are not |
| Declaration initialization | Declarations use only `=`: `Point p = {1, 2}`, `Point p = {}`, `Point p = Point(1, 2)`; in non-declaration positions use `Point(...)` |
| Type inference | Local variables may use `auto x = expr;`; local only, and an initializer is required; `dynamic`/`any` are reserved names only |
| Dynamic types | Not done in phase one; heterogeneous values use language `variant` types (10.1); `dynamic`/`any` are reserved only |
| Scope | Declarations do not write `T x(...)`; `Point p` is uninitialized, `= {}` zero-initializes; unit-level variables are declared directly and are `private` by default |
| Unit initialization | Unit-level variables may be initialized at runtime and are destroyed at exit (9.1, implemented 0.0.102); the order is a DFS post-order over `use` edges from the entry unit, cleanup is its exact reverse, declaration order inside a unit; **the same sequence orders compile-time constant evaluation and `#if` selection** (9.2) |
| Object model | Values and references are separated: `struct` is a value type; `class` is a reference type (single inheritance + interfaces; virtual methods by default; atomic RC + weak; no GC/cycle collector) |
| Strings | `string` (a value type provided by the std library: an immutable UTF-8 byte sequence; for layout and API see the library documentation) |
| Generics | **Monomorphization** (one copy of the code per combination of type arguments); `<>` syntax; constraints use `interface` + `&`/`\|`; `Self` refers to the implementor; constraints are matched **structurally**, and built-in types satisfy them at zero cost; see 3.15 for details |
| Tagged unions | Declared with `variant Name { Alt T; Pair T, U; None; }` (decided 2026-11-20; layout and value semantics unchanged from the 0.0.115 sum implementation); dispatched **only** with `match` (10.1) |
| Declaration split | `type` declares only transparent aliases (4.5); `extend I with J;` extends an interface with another interface's default methods (3.18); `promote field;` forwards a field's **public methods** (3.19) |
| Dispatch | `match` is the only dispatch construct: constant patterns for integers, `char`, enums and `bool`; alternative patterns for a `variant` (10.1.2). There is no `switch` (6.6) |
| Memory | Pointers + `alloc/free/realloc` (raw memory); `class` uses `new` + atomic reference counting (write `x = null` to release early); no GC, no borrow checking |
| Control flow | `if/else`, `while`, `for-in`, C-style `for` (6.3.1), `break/continue/return`; conditions must be `bool`; braces are mandatory |
| Conditional expression | `cond ? a : b` (right-associative; the condition must be `bool`; only one branch is evaluated; both sides use the same type / implicit widening / `null` adaptation) |
| Condition parentheses | Optional; the canonical form omits them (`if cond { }`); C-style `if (cond)` is still legal |
| Struct construction | `Point(x = 1, y = 2)` with named arguments, or `Point(1, 2)` with positional arguments; the two cannot be mixed |
| C capabilities | Pointers, structs, unions (`union`), arrays, functions, enums, C ABI interop; `goto` is not provided; varargs are `extern`-only; function pointers/lambdas per 3.16 |
| Backend (non-normative) | The LLVM C++ API generates COFF directly; the C99 backend was removed in 0.0.2; Windows x64 is a first-class target |

---

## 1. Design Principles

1. **Keep the hard capabilities of C**: value semantics, pointer arithmetic, manual memory, struct, C ABI.
2. **Drop C's historical baggage**: no preprocessor (no macros, no textual substitution; `#if` only
   selects code at build time, see 9.2), no implicit narrowing, no integer truthiness, no `do-while`;
   `++/--` keep only the statement form (no value). A C-style `for (init; cond; step)` **is** provided
   (6.3.1) because it is the readable form of an index loop.
3. **Explicit beats implicit**: use `as` for type conversions; use `const` for mutability; a read-only API boundary uses `const` methods (3.11) or a read-only `interface`.
4. **Small and predictable**: generics only do monomorphization (no erasure, no runtime generics), no capturing closures, no hidden allocations; overloading is limited to the bounded rules in 3.11; errors that can be caught at compile time are not deferred to runtime.
5. **Leave room for optimization without sacrificing semantics**: `const` is a static guarantee (each object is initialized once), a structured `for-in`, and future alias annotations.
6. **Separate values from references**: `struct` has value semantics and `class` has reference semantics, and the two are never confused; heap objects are managed with reference counting, and cycles are broken manually with `weak`.
7. **No `unsafe` mechanism**: every facility (raw pointers and arithmetic, `alloc`/`free`, `union`, uninitialized locals, `extern`, future intrinsics) is available to every programmer. There is no safe subset to protect, so a marker would be decoration; what is unsafe stays **documented UB** (8.2), and the compiler's job is to make it *findable* (explicit intrinsics with fixed names) and *debuggable* (optional runtime checks), not to fence it off.

---

## 2. Lexical

### 2.1 Source Files

- UTF-8 encoding, extension `.mu`.
- Comments: `//` line comments; `/* ... */` block comments, **nesting is supported** (implemented).
- Statements end with `;`; semicolons are not inserted automatically.
- **Unit discovery** (a loader convention, not a language feature): `unit a.b.c;` is looked up as
  `<root>/a.b.c.mu` or `<root>/a/b/c.mu` under each search root (in `-I` order), so both the flat
  dotted name and the slashed path are accepted everywhere.
- **Platform shadows**: a directory `platform/<os>/` (`<os>` = `windows`, `linux`, `macos`) shadows
  the units below its parent, nearest directory first. For `a.b.c` the loader tries
  `<root>/a/b/platform/<os>/…`, `<root>/a/platform/<os>/…`, `<root>/platform/<os>/…`, and only then
  the portable unit. Inside a shadow directory the file may keep the **full** unit name
  (`libs/std/platform/windows/std/ctypes.mu`) or **drop the segments the directory already
  represents** (`libs/std/platform/windows/ctypes.mu` is the same unit); the full name is always
  accepted, and having both is an error (ambiguous unit). A library therefore ships its own per-OS
  implementations next to its code and stays vendorable as one directory.
  `platform` is reserved for this: a unit name may not start with it, and a `platform.<os>` segment
  pair is rejected.
  `muc check --os <os>` / `--platform <os>[-<arch>[-<abi>]]` resolves the program against another
  platform's units, on any host; the shadow keys are tried most specific first
  (`<os>-<arch>-<abi>`, `<os>-<arch>`, `<os>`), so a unit only needs the level where it really differs.
  **A shadow directory replaces the whole unit**, so it is the right form when the implementations barely
  overlap. When they share most of their surface, keep one file and put the differences in `#if` arms
  (9.2): the `cstd` library is written that way.

### 2.2 Identifiers

- `[A-Za-z_][A-Za-z0-9_]*`, case-sensitive; non-ASCII identifiers are not allowed in phase one.

### 2.3 Keywords

```
unit use public private export const builtin auto struct enum extern class interface new weak packed union
if else while for in switch ref break continue return
try catch throw finally
true false null as sizeof alignof offsetof alloc realloc free
void bool byte char short ushort int uint long ulong
float double usize isize
and or not override operator super this
```

> `del` was removed in 0.0.47 (it is kept as a keyword only to give a migration diagnostic): to release a reference, write `x = null;`.

> **`builtin` is a hard keyword** (decided 2026-11-17, 9.3): it declares a constant or function whose value/implementation is provided by the compiler; a `builtin` declaration has no initializer and no body.

> `goto/typedef/fn` are not included for now (function types are written `(T1, T2) => R` with no keyword, and no
> `typedef`-style declarator is introduced; see 3.16). `sizeof/alloc/realloc/free/alignof` and the like are built-in identifiers,
> **and must not be shadowed by user declarations** (implemented: using them as a variable/parameter/field name is a hard error). **`len` was removed (0.0.101); it stays reserved only to report the migration.**
> `dynamic` / `any` are reserved for future dynamic types / top types and are not keywords in phase one.
> `packed` is a **hard keyword** (a layout modifier, see 3.4, implemented); `align` is a **contextual keyword** — it is parsed as a modifier only when it appears at the start of a type declaration
> and is immediately followed by `(` (`align(8) struct S {...}`); in other positions it may still be used as an identifier (`int align = 4;` is legal).
> If the implementation would rather not maintain contextual keywords, `align` can simply be added to the hard keyword table (there is currently no identifier with the same name anywhere in the repository).
> **`type` is a contextual keyword** (0.0.62): it is parsed as a **type alias** only when `type Ident =` appears at the start of a declaration;
> in other positions it is still an ordinary identifier (so `type` fields in C interop structs are unaffected).
> **`step` is a contextual keyword**: it is parsed as a step size only after the range sequence of a `for` (`for i in 0..10 step 2`); in other positions it is an ordinary identifier.
> **`Self` is a contextual keyword** (0.0.67): it is parsed as the "implementor type" only when it appears in type position inside an `interface` body (see 3.15.6); in other positions it is still an ordinary identifier.
> **`para` is a hard keyword** (implemented 0.0.92, 3.17.2). **`thread` is reserved as a contextual keyword** for the compute-pool modifier (3.17.4, not implemented yet): until it lands it remains usable as an identifier. `spawn` is **not** a keyword and is not used by the concurrency design (deferred join is library-only). `any` stays reserved for the future top type (10.1).

### 2.4 Literals

- **Integer**: decimal, `0x`, `0b`; `_` may be used as a separator; octal is not supported.
  - A **C-style suffix** fixes the literal's type (implemented 2026-11-19): `u`/`U` = `uint` (value ≤ 4294967295),
    `l`/`L` = `long`, `ul`/`UL`/`lu`/`LU` = `ulong`. A suffixed literal does not adapt to the expected type.
  - Without a suffix the literal adapts to the expected numeric type when it fits, otherwise it is `int`
    (fits in 32 bits) or `long`; the maximum is **2^63-1** — write `ul` for the full unsigned 64-bit range
    (`ulong u = 18446744073709551615ul;`).
  - `-9223372036854775808` (the magnitude `2^63` directly after unary `-`, in any base, with or without
    the `l` suffix) is the minimum of `long`; the magnitude alone is a compile error.
- **Float**: decimal point / exponent; `double` by default, the `f` suffix means `float`.
- **Character**: `'a'`; escapes `'\n' '\t' '\\' '\'' '\0' '\xNN' '\u{...}'`; the default type of a literal is `byte` (unsigned 8-bit), adapted to `char`/`byte` according to context.
  - `char` is **signed** 8-bit (C style, −128–127); `byte` is **unsigned** 8-bit (0–255). There is no `ubyte`/`uchar` alias.
  - Literals are interpreted by **bit pattern**: `'\xE4'` is by default the `byte` value 228; assigned to a `char` its value is −28. Use `byte` (or `x as byte`) when an unsigned byte is needed.
  - A character literal denotes a **single byte**, not a Unicode code point: `'a'` is `0x61`; multi-byte characters such as `'中'` are a **compile error** (non-ASCII raw characters are not supported for now).
    `'\u{...}'` only accepts `U+00`–`U+FF` (a single byte); larger code points use the string literal `"\u{4E2D}"` (see below).
  - Unicode text uses `string` (a UTF-8 byte sequence, see 3.3); both `s.length()` and `s[i]` operate on bytes; the element type of `s[i]` and of string for-in is `byte`.
- **`\u{...}` escape** (shared by character and string literals):
  - The form is `\u{` + 1–6 hexadecimal digits + `}` (either case is allowed, leading zeros are allowed, e.g. `\u{4E2D}`, `\u{1f600}`); the four-digit `\uXXXX` form is not supported.
  - The value range is `U+0000`–`U+10FFFF`; **the surrogate range `U+D800`–`U+DFFF` and values outside the range are compile errors**.
  - Written in a string literal it is encoded as **UTF-8** into 1–4 bytes (`"\u{4E2D}"` has the same bytes as `"中"` in source code); the NUL produced by `\u{0}` is an **error** just like `\0` (a string literal cannot contain NUL).
  - Written in a character literal it requires a code point ≤ `U+FF` (a single-byte bit pattern); otherwise it is an error and the compiler suggests using a string instead.
- **Unicode code point type = `int`** (i32): the range is 0–`0x10FFFF`, arithmetic and comparison work naturally, and `-1` can serve as an invalid sentinel; **no** types or aliases such as `rune` / `unichar` / `char32` are **introduced**.
  APIs that iterate a string by code point (`s.chars()` and the like) belong to the string library and are designed together with the string type (see backlog U3); they will return `int`.
- **String**: `"..."`, UTF-8, statically stored at compile time, NUL-terminated; **the type of a string
  literal is `const char*`** (0.0.89, as in C). `String` (the std library value type, 3.3) is built from
  a literal through its implicit `String(const char*)` constructor; the reverse direction needs
  `.cstr()` (there is no implicit `String → const char*` conversion); supports `\u{...}` (see above).
- **Boolean**: `true` / `false`.
- **Null**: `null` (not 0); it can be assigned to pointers and class references, but cannot be used for structs/scalars.

### 2.5 Reserved Words (future extensions, not usable as identifiers for now)

The following identifiers are reserved for future features; the compiler should reject their use as variable/function/type names:

```
where  move  typeof  variant  match  extend
```

- Generics: `where` (reserved; constraint syntax currently uses `<T: I>`, see 3.15).
- Syntax rework (decided 2026-11-20): `variant` declares a tagged union (10.1.1), `match` dispatches on it (10.1.2), and `extend` declares an interface extension (3.18). `promote` is a **contextual keyword**: it is recognized only at the start of a member declaration inside a type body (3.19), so it remains usable as an identifier elsewhere. `sum` is **not** a keyword (the declaration uses `variant`).
- `type` remains a contextual keyword and declares only a transparent alias (4.5).
- Others: `move` / `typeof`.
- **Released from the reserved list**: `async` / `await` are decided **not** to be provided (see 10); `shared` / `atomic` are not language features (sharing is explicit and atomics are a library intrinsic, see 3.17.1); `spawn` is released as well — deferred join is a library facility (`TaskGroup` + a one-slot `Channel<T>`), not syntax; `para` became a **keyword** in 0.0.92 (3.17.2); **`unsafe` is released**: there is **no unsafe mechanism** (principle 7 in 1) — every facility is available to every programmer, so the marker has nothing to protect and would only be decoration; the C-level surface stays documented UB (8.2).
- `try` / `catch` / `throw` / `finally` (exceptions) are **implemented keywords** listed in 2.3.

---

## 3. Type system

### 3.1 Basic types

| Type | Width | Description |
|---|---|---|
| `bool` | 1 | `true`/`false`; cannot implicitly convert to an integer |
| `byte` | 1 | unsigned 8-bit (0–255); byte/UTF-8 handling |
| `char` | 1 | **signed** 8-bit (−128–127, C style): C string/byte interop; `'\xNN'` is a bit pattern |
| `short` / `ushort` | 2 | signed/unsigned 16-bit |
| `int` / `uint` | 4 | signed/unsigned 32-bit |
| `long` / `ulong` | 8 | signed/unsigned 64-bit |
| `float` / `double` | 4 / 8 | IEEE-754 |
| `usize` / `isize` | pointer width | platform-dependent; `isize` for lengths, indices and byte sizes; `usize` for unsigned arithmetic and C-facing sizes |
| `string` | defined by the library | value type of the std library: UTF-8 byte sequence, immutable; for layout/`sizeof`/API see the library docs |
| `void` | — | no value; usable only as a return type or as the base type of `void*` |

- Phase one introduces no `wchar`, `decimal`, or `i8/u8`-style aliases; `decimal` comes later as a library implementation.
- All type widths are fixed; `usize/isize` is the only platform-dependent type; library types (such as `string`) do not promise a fixed `sizeof` (see the library docs).
- `void` cannot declare variables, fields, parameters, or array elements; `void*` is legal; `sizeof(void)` / `alignof(void)` are illegal.

### 3.2 Pointers

- Types: `T*`; `void*` is the untyped pointer; `T**` is legal.
- Operations: `&expr`, `*p`, `p[i]`, `p + n`, `p - n`, pointer comparison, pointer subtraction, and the
  member shorthand `p->f`, which is exactly `(*p).f` (one dereference level; `p->f` and `p->f[i]` chain like
  postfix operators). `p.f` on a pointer is an error — write `p->f`. **Style: use `p->f` in preference to
  `(*p).f`**, including `&p->f`, `p->f[i]` and `p->f->g`; spell out the dereference only when the
  dereference itself is the subject (e.g. a test for `*p`).
- `null` can only be assigned to a pointer or a class reference (see 3.8); `p == null` is legal.
- `T*` implicitly converts to `void*`; `void*` to `T*` requires an explicit `as`.
- `T*` implicitly converts to `const T*`; the reverse requires an explicit `as` (unsafe).
- Pointer arithmetic is in units of `sizeof(T)`; arithmetic on `void*` is not allowed.
- Function pointers are written `(T1, T2) => R`: a one-word value, binary compatible with a C function pointer,
  with no declarator nesting and no `*`; see 3.16 (implemented in 0.0.84).

### 3.3 Arrays and dynamic arrays

- Fixed-size arrays: `T[N]`, where `N` is a compile-time integer constant. **C storage, C99 semantics** (0.0.106):
  - **Decay**: an array expression used as a value converts to a pointer to its first element
    (`int* p = a;`, `f(a)` for `f(int*)`, `int* r = m[1];`, a struct field `int* q = s.v;`). No decay
    under `sizeof`/`alignof`, which still report the array's own size.
  - **Not assignable**: `a = b;`, `int[N] b = a;` and `s.v = a;` are compile errors ("arrays are not
    assignable (C99)"). Copy element by element, or use `memcpy` for POD elements.
    Two Mu extensions remain: an array-typed `return` still copies, and initializing an array from a
    **prvalue** array (a function result) is allowed — there is no lvalue to alias. Arrays of
    `class`/`interface` handles follow the same rule and are copied element by element (each element
    assign retains/releases).
  - **Parameter adjustment**: a parameter declared as `T[N]` is `T*` (C99 6.7.5.3p7) — `sizeof(xs)`
    inside the callee is the pointer size, writes are shared with the caller, and only the outermost
    dimension decays (`int[2][3] rows` is a pointer to `int[3]`). `ref T[N]` is still rejected
    (`ref` borrows scalars and value structs, 3.8). Arrays whose element is a `class`/`interface`
    keep value semantics, because this language has no pointer-to-reference type (3.8): such a
    parameter copies the handles (retain per element).
  - No `length()` and no `for-in`; use `sizeof(a) / sizeof(a[0])` for the count and an index loop.
  - Aggregate initialization `T[N] a = {…}` (zero-padded when short) and `= {}` (all zeros) are
    unchanged, as is initialization of a `char`/`byte` array from a string literal.
- **Dynamic arrays**: `T[]` / `T[,]` / `T[,,]` are **built-in types** (rank 1–3; decided 2026-11-16,
  superseding the earlier "sugar for `Array<T>`" rule). A dynamic array is the single way to obtain a
  heap block of elements: the representation is the compiler's, and the former `buffer<T>` name is
  **removed** (`buffer<T>` is an error telling the user to write `T[]`). A library container
  (`Array<T>` and friends) wraps a `T[]`; it adds methods, not representation.
  - **Representation**: a reference-counted handle `{ i64 rc; i64 len; data... }` (rank 1; rank r keeps
    `r` dimension fields). There is **no capacity field**: the length is fixed for the whole lifetime of
    the block, elements cannot be added, and the block is never reallocated — so an element address
    taken from it stays valid until the handle is released.
  - **Built-in members**: exactly three — `length()`, `operator[]`, `ptr()`. Everything else
    (`push`, `reserve`, `find`, `sort`, …) belongs to a library container.
  - `new T[n]` creates one (`n` is a runtime integer) and **zeroes the element block**;
    `uninit new T[n]` omits that zeroing and is legal only for a trivial element type (below).
    `T[] s = {}` means empty (null handle).
    Every length must be **non-negative** and the resulting byte count must not overflow: a negative
    constant is a compile error, a negative or overflowing computed length aborts like a failed
    allocation (6.6). `length()` is therefore always the number of valid indices (6.4.3).
  - **`uninit`**: `uninit new T[n]` leaves the element area undefined (reading an element before writing
    it is UB, 8.2). The header — reference count, length, dimensions — is always valid, so `length()`
    and the bounds check keep working. The element type must be **trivial**: no `class` / `interface` /
    `string` / `T[]` field (directly or nested) and no user destructor (directly or nested, through a
    struct or fixed-size array field); otherwise the program is rejected, because a non-zero handle would
    corrupt the reference count at release time.
  - **`ptr()`**: the address of element 0 as `T*` (`const T*` for a `const` block); a null handle
    yields a null pointer. It borrows the block, so the operand must be a variable or a field
    (binding a temporary first is required). It requires a **trivial** element type for the same reason
    as `uninit` (a destructible element included): a raw write would bypass the element's retain/release
    and its destructor. `ptr` is a method, not a field
    (`x.ptr` is an error).
  - Assignment/argument passing copies the **handle** (O(1)), sharing the same array; destroyed when the reference count reaches zero;
    if the element type holds class/interface references, release element by element.
  - Element types: scalars, enums, pointers, structs (including reference fields and user destructors),
    class/interface references. A struct with a user destructor is a valid element: `new T[n]` **runs the
    parameterless constructor of every element** and releasing the block destroys every element. A struct
    that declares constructors but no parameterless one cannot be an element of `new T[n]` (there is
    nothing to call); `{ … }` initialization and `new C[n]` for a class element are the exceptions
    (the latter yields null handles).
  - `s.length()`: 0 for a null handle; `s[i]` indexing (bounds-checked, see below); `for x in s` iterates over the elements.
  - `for ref x in s` can write to a shared array; **not allowed when the element is a class/interface** (the element is a handle, iterate with value binding);
    `string` also cannot use `for ref` (immutable) — use `for const ref` (read-only) or value binding.
  - Null value: `T[] s = null;` and `T[] s = {};` are equivalent (both null handles); but **a slice cannot be compared with `null`**
    (use `s.length() == 0`).
  - Releasing a dynamic array: setting the handle to empty (`s = {}` / `s = null`) releases it once; `del` was removed (0.0.47).
  - `sizeof(T[])` / `sizeof(T[,])` = 8 (handle size); the size of library types is defined by the library (for `string` see the library docs).
  - Aggregate initialization `T[] s = {1, 2, 3};` allocates a new array and initializes the elements.
  - `T[N]` → `T[]` implicit conversion: **allocates and copies** (dynamic arrays no longer provide a borrowed view).
  - Sub-slicing/borrowed-view syntax (`s[1..]` etc.) is not provided yet; `ptr()` plus an explicit
    length is the current escape hatch, and it is limited to trivial element types.
- **`String` is the canonical name** of the library string type; `string` is a `type` alias (0.0.101).
- **Static rectangular multi-dimensional arrays**: `T[m,n,...]` (`m,n` compile-time constants), contiguous C storage.
  - `int[2,3] a = {{1, 2, 3}, {4, 5, 6}};` (nested braces, missing elements filled with zero).
  - Indexing `a[i,j]`, equivalent to the chained form `a[i][j]`; a partially indexed `a[i]` is a row
    **lvalue** of type `T[n]` that decays to `T*` (it is not copied; assignment of the row is an error).
  - The dimensions are derived with `sizeof`: `sizeof(a)/sizeof(a[0])` is dimension 0, `sizeof(a[0])/sizeof(a[0][0])` is dimension 1, and so on.
  - May contain class/interface/struct reference elements (RC element by element); `sizeof` is the total byte count.
- **Dynamic rectangular multi-dimensional arrays**: `T[,]` (only the rank is written; the size is determined by `new`) —
  the rank-2 built-in type (see the dynamic-array bullet above), same for `T[,,]`; rank 4+ is not
  provided (use a flat `T[]` with a manual index).
  - `int[,] a = new int[m,n];`; any `new T[d0,d1,...]`.
  - Indexing `a[i,j]` (all dimensions must be given; taking a "row view" is not supported).
  - `a.length()` = dimension 0, `a.length(k)` = dimension k (k may be a runtime expression; subscripts are bounds-checked).
  - **`for-in` is not supported** (iterate multi-dimensional arrays with index loops; use `a.length(k)` for each dimension's length, see 6.4).
  - Conversion: `T[m,n,...] → T[,]` allocates and copies.
- **Jagged arrays**: `T[][]` (the element is itself a dynamic array); rows may have different lengths.
  - `new T[m][]` creates `m` empty row slots (elements are null `T[]`), then loop
    `a[i] = new T[n_i];`.
  - `new T[m][n]` is **not** legal syntax: a jagged array creates only one dimension at a time; for rectangular arrays use
    `new T[m,n]`. Static jagged arrays can use fixed-size row slots `T[][N]` or aggregate initialization.
- All-zero: `int[5] c = {};`; empty dynamic/rectangular arrays: `int[] d = {};`, `int[,] e = {};`.
- Arrays **do** decay to pointers in value position (C99 6.3.2.1, 0.0.106); `&arr[0]` remains an
  equivalent explicit spelling. The array object itself is not assignable (see the rules above).
- Indexing `x[i]` / `x[i,j]`; the index type is `isize` (any integer widens to it; a negative index is out of range and throws).
- Managed sequences answer `x.length()` (`isize`); **static arrays are C storage and have no `length()`** - derive the count with `sizeof(a) / sizeof(a[0])`.
- **Bounds checking**: managed sequences (`T[]`) are checked at the subscript; `--bounds=on|off` (default on) controls it, and a violation throws `std.exception.BoundsError { index, length, axis }`. Static arrays and `T*` are unchecked (C semantics).
- All-zero/null values use `{}`: `int[5] a = {};`, `int[] s = {};`, `string s = {};` (aggregate types no longer support `= 0`).
- **`String` is a value type provided by the std library** (`string` is a lowercase `type` alias; not a language built-in type; the spec of the library is
  [docs/StdLibSPEC.md](file:///D:/dev/mu_lan/docs/StdLibSPEC.md) §6.3, the implementation notes are an
  internal record). **The compiler knows nothing about its layout** (0.0.89):
  - **A string literal is a static, NUL-terminated UTF-8 byte buffer; its type is `const char*`** (as in C).
    `string s = "abc";` and `string s = cstr;` go through the library's `implicit string(const char*)`
    (a copy, one allocation, see 3.6); `"abc"` on its own stays `const char*` and is passed to C as is.
  - **Language-visible contract** (the part SPEC guarantees):
    - `string s = {}` is the empty string (zero value); `s.length()` is the **byte length**
      (`length()` is the uniform accessor, 6.4.3).
    - **Immutable**: `s[i] = ...` is a compile error (the subscript does not return `ref`); `for ref` is not supported.
    - `s[i]` yields `byte` (the library's member `operator[]`); `for c in s` iterates by byte through the
      index form (`length()` + `operator[]`, 6.4.1).
    - `==` / `!=` compare by **byte value** (library `operator==`); a string can be compared with a literal
      directly (`s == "abc"`, `"abc" == s`); comparing a struct value with `null` is a compile error.
    - C interop: `s.cstr()` returns the underlying `const char*` (NUL-terminated); the compiler no longer
      converts a `string` to a pointer implicitly. `strlen(cstr)` gives the byte length of a C string.
    - Usable as struct/class fields, array elements, parameters, return values; **copyable and destructible**
      (value semantics, copies are independent).
  - Size: `sizeof(string)` is defined by the library (currently 24 bytes: `byte* p; isize n; byte[] owner`);
    to pass strings across the ABI use `const char*`.
  - **A `string` may be a unit-level variable** (9.1): it is a struct with reference-counted fields
    and is destroyed by the exit sequence. A `const` unit-level `string` is allowed (decided 2026-11-17):
    it is initialized at run time by the unit initializer and is **not** a compile-time constant, so it
    cannot be used in `#if`/array lengths/`step`. Write `const char* G = "hi";` when a compile-time
    constant is needed.
  - Construction and operations (`string(const char*)`, `length()`, `at`, `sub`, `concat`, `operator[]`,
    `operator==`, `cstr`, `strlen`, ...) are **library API**, see the library docs; they are not part of the
    language specification. Everything about `string` that used to be compiler magic (`s[i]`, `for-in`,
    `==`, the literal→string conversion) is now ordinary library code (member operators, 3.11).
- Initializing a fixed-size `char[N]` from a string literal performs a byte copy: copies the content and appends NUL and zeros
  (a literal that does not fit, including the terminating NUL, is a compile error).

### 3.4 Structs

```c
struct Point {
    int x, y;
}
```

- Value type; assignment, argument passing, and return all copy.
- Fields are laid out in declaration order with natural alignment; compatible with the C ABI.
- Aggregate initialization in a declaration: `Point p = {1, 2};` (positional) or `Point p = {x = 1, y = 2};` (named).
  - All fields must be given; positional and named cannot be mixed; unknown/duplicate/missing fields are compile errors.
  - All-zero: `Point p = {};`.
- Construction in an expression: `Point(1, 2)` (positional) or `Point(x = 1, y = 2)` (named); `Point()` means zero initialization.
  - Non-zero arguments must supply all fields; positional and named cannot be mixed.
  - `Point p = Point(1, 2);` is legal; the RHS is a prvalue, constructed directly into p, with no extra copy; equivalent to `Point p = {1, 2};`.
- Do not write `Point p(1, 2)` in a declaration; non-declaration positions use neither `{...}` nor `{}`/`= 0` construction.
- `Point(...)` is value construction, not heap allocation; `new` is not introduced (reserved for future heap allocation).
- Resolved during semantic analysis: if the called name is a type it is a struct construction, if it is a function it is a function call; a type name must not conflict with a function/variable name in the same scope.
- Can declare **methods** (`ret name(params) { ... }`): static dispatch, no vtable, no inheritance/virtual functions (an interface default can be inherited, see 3.10 — it is materialized as an ordinary method of `S`, not inherited);
  `this` is a reference to the current object (not a pointer), available implicitly, or written explicitly as `this` to access fields/call methods;
  methods can be overloaded, and cannot share a name with a field.
  Methods do not change the C layout (field order/alignment still match C).
- Can declare **`const` fields** (decided 2026-11-17): `const int id;` is a per-instance read-only field, initialized by every constructor (member initializer list or a default member initializer) and read-only afterwards; it occupies instance storage and participates in layout like any other field (see 3.7). Type-level compile-time constants are gone — use a unit-level `const` instead.
- Can declare **constructors/destructors**:
  - `S(...)` constructors can be overloaded; `S s;` calls the parameterless constructor; `S s = S(args);` constructs in place;
    with a constructor present, non-empty aggregate initialization is forbidden; use `{}` for the zero value (no constructor call, just zeroing);
  - `~S()` is called automatically at scope exit and on `return`/`break`/`continue` (local variables);
- No inheritance, no virtual functions, no anonymous struct/union **members** (unions themselves see 3.4.1).
- Recursion only through pointers: `struct Node { int v; Node* next; }`. A by-value cycle — direct, mutual
  (`A` holds `B`, `B` holds `A`), through a fixed-size array, or through a `union` member — has no finite
  layout and is a compile error that spells the cycle out (`A -> B -> A`). A generic template is checked
  when it is instantiated, since the cycle can depend on the argument.
- Fields can be scalars, pointers, enums, fixed-size arrays, dynamic arrays `T[]`, nested structs, `string` fields, and
  **class/interface reference fields** (in which case the struct is a non-POD value type).
- A struct containing class/interface reference fields (including references in nested structs/arrays) gets compiler-generated field-by-field RC:
  - Copy initialization `S b = a;` and pass by value: retain the reference fields after copying;
  - Assignment `b = a;`: retain the new value first, then release the old value (self-assignment safe);
  - Destruction (scope exit, `return`/`break`/`continue`, host class destruction, array element by element):
    release field by field;
  - Parameters are owning local variables (retain on callee entry, release on exit, see 4); function return values are owned values
    (retain fields first when returning a borrow); struct temporaries returned from calls are released at the end of the statement;
  - A local struct that is not explicitly initialized is zeroed first, to avoid releasing uninitialized pointers during destruction.
- **Temporary restrictions**:
  - **Cannot take the address of a struct with reference fields** (`&s`, `&arr[i]`; a scalar field `&s.x` still works): a raw pointer could bypass
    field-by-field retain/release (backlog U24). To modify in place, pass/return by value, or switch to `class`;
    **structs with user destructors are the exception**—they may take their address (see the "user destructors" item below).
  - Structs with reference fields may be unit-level variables (they are destroyed by the exit
    sequence, 9.1);
  - `extern C` signature recursive check: structs containing class/interface cannot be C parameters/return values;
  - Structs with **user destructors** may be fields of structs/classes, elements of fixed-size arrays and
    elements of dynamic arrays: construction and destruction recurse through fields and elements, in
    declaration/index order
    (`agents/value-semantics.md` §4). Whether such a struct can be **copied / assigned / passed /
    returned by value** is governed by the rule of three: it must declare a copy constructor
    `T(const ref T other)` (0.0.138) and, for moves, `T(move T other)` (0.0.137) — then all four work
    through those constructors (assignment is synthesized as copy-and-swap); without them it is a
    "cannot copy" compile error, because a byte copy would double free (see the copy/move/`swap` items
    below). A struct that *contains* one does **not** inherit the constructors of its field: the
    compiler's copy and move reproduce reference-counted handles, they never invoke a nested user
    constructor, so such a host is copyable/movable only when it declares its own copy/move constructor
    (build 162). They may be unit-level variables (9.1);
- `sizeof(S)` is available; pure-value struct assignment has byte-copy semantics, and copies of structs with reference fields get RC operations inserted by the compiler.
- Fields also support multiple names sharing a type: `int x, y;`; the type-first fixed-size array form is also allowed, e.g. `char[20] name;`.
- **Move semantics (implemented, 0.0.137)**: `move x` transfers ownership out of a local variable or
  parameter; `S b = move a;`, `b = move a`, `f(move x)` and `return move x;` are the positions.
  - **`move` is C++'s `&&`** (decided 2026-11-17): `move T` is a **non-const rvalue reference**. `const`
    and `move` cannot qualify the same parameter (`const move T` / `move const T` are errors), and `move`
    applied to a `const` object is an error — it does not silently degrade to a copy.
  - Only a whole name may be moved: `move o.field` / `move a[i]` are compile errors. A value type is meant
    to be small and self-contained; needing to move a single field out of one is the signal that the object
    should be modelled as a `class` instead (heap entity, reference semantics, shareable). `const` objects
    and `ref` borrows cannot be moved.
  - **Every value type is movable by default** (decided 2026-11-18). A type without a user move
    constructor is transferred by the compiler: copy the bytes, **empty the source, and do not destroy
    the source** — which is the correct ownership transfer for a raw pointer, a reference-counted handle
    and a nested destructor-bearing field alike (the destination owns what it received; the emptied
    source owns nothing to release).
  - `nocopy` forbids **copying** only; the value is still movable. `nomove` is the explicit opt-out
    (see below). A user-declared `T(move T other)` takes over the transfer rules: it must leave the
    source in a valid, destructible state, and **the source's destructor runs normally** (C++ model) —
    a move constructor that forgets to empty a field therefore double-frees (loudly) instead of
    leaking silently.
  - `move` accepts a **plain variable name only**: partial moves (`move o.field`, `move a[i]`) are
    rejected, so a moved-from slot is never left behind (a container that wants to hand an element out
    provides its own operation).
  - A **`ref` parameter is a name too**: `move p` transfers the *referent* and leaves the caller's slot
    as the type's zero value — the destructible empty shell a moved-from local is left in. That is the
    mechanism behind an out-parameter and `std.mem.take` / `replace`; a `for ref` loop variable stays
    immovable (the element belongs to the container).
  - The move constructor receives its source **by pointer** (like `ref`), so the body steals without the
    parameter retaining anything; the compiler zeroes the source afterwards and stops destroying it.
  - **A moved value is out of reach**: any later read, write, borrow, pass or return of that variable is a
    compile error until a new assignment re-arms it. `if`/`else` merge the two arms; `move` may not target a
    variable declared outside the enclosing loop; nothing is tracked across functions (a moved value cannot
    escape except through the transfer points themselves).
  - **`return x;` moves** when `x` is a local or parameter of a movable-but-uncopyable type: the name
    dies with the frame, so transferring it cannot alias anything the caller observes. Writing
    `return move x;` stays legal and means the same thing. A **copyable** type keeps the copy path
    (moving it would silently change copy counts); asking for a move there is written `move`.
  - Uncopyable-but-movable types may be returned and passed by value; passing a plain lvalue of such a type
    reports "write `move`".

- **`swap(a, b)` (implemented, 0.0.143)**: exchanges the values at two writable places (a variable, a field,
  an element or a dereference) with a pure byte exchange — no constructor, destructor or reference-count
  operation runs. It is built in rather than a library function because it needs the addresses of both places
  even for types whose address must not be taken (`&` is refused for a struct with reference fields), and
  because a byte exchange is exactly what a reference-counted handle needs (the counts do not change). Both
  sides stay initialized, so it never interacts with the moved tracking; `swap(a, a)` is a no-op. It is the
  primitive a container needs for `remove`/`swap_remove`. **`weak` fields cannot be swapped**, and neither
  can a value that embeds one: the weak-reference registry is keyed by (target, field address), so a raw
  exchange would leave both registrations attached to the wrong holder.

- **Copy construction and assignment (implemented, 0.0.138; `const ref` source in 0.0.139-0.0.140)**:
  `T(const ref T other)` declares the copy constructor (`ref`/`const ref` parameters on a constructor are accepted only
  for that form, and the source is read-only — `ref T` reports that a copy must not modify its source);
  `nocopy` forbids it. For a type with a user destructor — or with a field/array element that declares
  one, which is what `needs_ctor_glue` means — **assignment is synthesized as copy-and-swap**:
  `b = a` copy-constructs into a temporary, swaps the byte blocks and destroys the temporary (the old
  value); `b = move a` does the same with the move constructor. A throwing copy constructor therefore
  leaves the destination untouched, and self-assignment is safe without an identity check. Value types must
  not store pointers into themselves (the swap is a byte swap) — such types belong to `nocopy`. A
  user-written `operator=` and the explicit construct form `S(a)` are not available yet.
  **A prvalue on the right of an assignment is moved, not copied** (decided 2026-11-19):
  `a = T(args);` and `a = f();` (where `f` returns a value of the type) build the value in a slot the
  expression owns and move it into the destination; the old value is destroyed as above. This works for
  uncopyable types too (`nocopy`, `has_dtor`) and needs no `move` spelling. An **lvalue** right-hand side
  still requires copyability (`a = b;` needs the copy constructor, or `a = move b;`).

- **`nomove` modifier** (**implemented, 2026-11-18**): `nomove struct Node { int v; }` declares a value
  type that must never be moved — its address is part of its meaning (a node in an intrusive list, a
  handle registered elsewhere). Same declaration position as `nocopy`, `struct` only. Such a value can
  still be constructed in place, passed by `ref`, and have its fields read and written; `move` on it is
  a compile error.
- **`nocopy` modifier** (**implemented, 0.0.136**): `nocopy struct Handle { int fd; }` declares a value type
  that must never be copied by value (it is still **movable**; combine with `nomove` when neither is
  allowed). It is a declaration modifier (same position as `public`/`packed`), and
  only `struct` accepts it — `class` is a heap handle whose "copy" is a reference-count increment (nothing is
  duplicated, so it needs no opt-out), `union` is copied by bits, and `interface`/`enum`/`unit`/`extern`
  reject it outright.
  - `copyable` becomes false for the type **and for every type that contains it** (the property is
    recursive): `S b = a;`, `f(s)`, `return s;`, a `variant`/`union` payload, and copying an array of it are all
    compile errors. Borrowing (`ref`), pointer access, field assignment and aggregate initialization are
    unaffected — they do not copy.
  - Also refused at the C boundary, because C copies by value.
  - Motivation: a struct that owns a resource (file descriptor, socket, lock) must not be duplicated by an
    implicit copy, and a struct without a user destructor is otherwise copyable by definition.
    `nocopy` alone is the move-only type (values are movable by default, see 3.13); `nocopy nomove` is
    the immovable one (`agents/value-semantics.md` §8.6).

- **Layout modifiers `packed` / `align(N)`** (**implemented, 0.0.42**):
  - Both are **declaration modifiers** (the same position as `public`/`private`/`nocopy`), written before the `struct` declaration (equally applicable once `union` lands,
    see U14), order does not matter, usable alone or combined: `packed align(8) struct Msg { ... }` and
    `align(8) packed struct Msg { ... }` are equivalent.
  - `class` / `interface` / `enum` do not accept layout modifiers (layout is not part of their ABI); writing one is a compile error.

  ```c
  packed struct WireHeader { byte version; uint seq; ushort kind; }  // offsets 0/1/5, sizeof 7, alignof 1
  align(16) struct Vec4 { float x, y, z, w; }                        // sizeof 16, alignof 16
  packed align(8) struct Msg { byte type; int seq; short len; }      // offsets 0/1/5, sizeof 8, alignof 8
  ```

  - `packed`: **takes no argument**, equivalent to C's `#pragma pack(1)` / `__attribute__((packed))`.
    Fields are placed by contiguous accumulation, with no internal padding and no tail padding (the tail is only padded out by `align(N)`); `alignof` defaults to 1.
    - `packed(N)` is not provided. When `pack(2)`/`pack(4)` is needed, use `packed` + explicit padding fields (`byte _pad[2];`);
      "explicit offsets beat implicit rules" follows the same reasoning that rejected bit-fields in backlog U16. If it is truly needed later, add a separate keyword `pack(N)`,
      **do not overload** `packed`.
    - The combined form `pack(1, 8)` is not supported (positional arguments can be silently reversed, and `pack(8)` has two readings: "level/alignment").
  - `align(N)`: `N` is a **compile-time constant that is a power of two** (implementation limit `N ≤ 65536`). `alignof(S) = N`; `sizeof(S)` is rounded up to a multiple of `N`,
    so that `sizeof(S) % alignof(S) == 0` always holds (array stride, `alloc(n * sizeof)`, and pointer arithmetic rely on this invariant).
    - **Does not change any field offset**, nor the internal layout of nested structs; as a field it only requires "the next field offset is a multiple of N".
    - `N` is compared with the **natural alignment** (the maximum of the field alignments), **not** with `sizeof`:
      - `N ≥ natural alignment`: legal. When `N > sizeof`, zeros are only padded at the **tail** (e.g. a 12-byte struct with `align(64)` → `sizeof` 64,
        `alignof` 64, for SIMD / cache lines / false-sharing avoidance);
      - `N < natural alignment`: **compile error**—the only way to reduce alignment is `packed`, no second "compression" mechanism is provided, and it is not silently ignored.
        After `packed`, the natural alignment counts as 1, so any power-of-two `N` is legal.
    - An illegal `N` (`0`, not a power of two, not a constant expression) is a compile error.
  - Field type restrictions inside `packed`: scalars, pointers, enums, fixed-size arrays, nested structs (`packed` or normal;
    a normal inner struct keeps its own internal padding). Types that **contain pointers and have natural alignment > 1 are forbidden**
    (`string`, `T[]`, class/interface references), to avoid unaligned pointer access.
    `packed` structs do not recursively flatten inner layouts (consistent with GCC/MSVC).
  - `&p.field` (where the field's offset does not satisfy its natural alignment) is legal; dereferencing it is **undefined behavior** (C semantics; no unaligned pointer type is introduced).
  - `class` fields insert padding according to the model alignment (types with `align(N)` / `packed` fields also keep their promised alignment);
    class layout remains an internal layout and does not participate in the C ABI.
  - `sizeof` / `alignof` reflect the actual layout; aggregate initialization, value semantics, and field-by-field RC rules are unaffected.
  - Heap allocation (for types with `align(N)`):
    - `class` objects are allocated by the compiler, with fields and the object base address both aligned to the model alignment (object alignment = `max(16, max field alignment)`);
      class fields with `N > 16` use aligned allocation (Windows `_aligned_malloc`/`_aligned_free`).
    - For **dynamic arrays** (`T[]` / `new T[n]` / `new T[m,n]`, `T[N]→T[]`, `T[m,n]→T[,]`, slice aggregate initialization)
      the data area follows a 24-byte header and guarantees at most 8-byte alignment: an element type with `align(N) > 8` is a compile error
      (switch to fixed-size arrays/pointers, or lower N). Static arrays, pointers, struct fields, globals, and stack variables are not subject to this limit.
    - A struct heap instance obtained by manual `alloc` + `as` only guarantees 16 bytes: do not place types with `align(N) > 16` that way (UB).
  - Cross-testing the ABI against C's `__attribute__((packed))` / `#pragma pack(1)` / `aligned(N)` is **not done yet**:
    within the language it is already covered by `sizeof`/`alignof`/byte-offset assertions; cross-language `sizeof`/`offsetof` comparison is still to be added (backlog U15).
- **Bit-fields are not provided (decided, 0.0.62; U16 closed)**:
  - C-style bit-fields `int x : 3;` inside a struct / union / `packed` struct are always a **compile error**, with an explicit diagnostic
    (`bit-fields are not supported; use explicit shifts/masks or \`byte[N]\` with offset constants`).
  - Zig `packed struct` / Rust bitfield-style explicit bit-width syntax is also **not provided** (once a fallback for "if it is ever necessary",
    now rejected; if it is truly needed later, add a separate keyword, do not overload existing syntax).
  - Rationale: the allocation order of C bit-fields, their crossing of storage-unit boundaries, and their interaction with `packed`/`align` are all **implementation-defined**
    (MSVC differs from GCC), which directly conflicts with the core selling point of "predictable layout + consistent cross-platform ABI".
  - Alternatives when bit packing is needed: explicit masks + shifts (most portable), `byte[N]` + offset constants,
    `packed` struct + hand-written accessors, or a C shim.

### 3.4.1 Unions `union` (implemented)

```c
union Value {
    int i;
    float f;
    byte[8] raw;
}
```

- Value type, sharing the same type namespace as `struct`; **all members are at offset 0**, `sizeof = max(member sizeof)` aligned up to the largest member alignment.
- `packed` / `align(N)` follow the same rules as 3.4: a `packed union` has alignment 1 and `sizeof` is not padded to the natural alignment; `align(N)` overrides the overall alignment.
- **Members are limited to POD**: scalars, pointers, enums, fixed-size arrays, POD structs / unions.
  Types that need **cleanup** (destructible / require field-by-field release) are forbidden—`string`, dynamic arrays `T[]`, `class`/`interface` references,
  structs with reference fields, structs with user destructors (a union cannot destroy members one by one). A union with no members is a compile error.
- No methods, no constructors, no destructors, no inheritance; `public:` / `private:` sections are not supported (members are always public).
- Initialization:
  - `U u = {};` zero initialization;
  - `U u = { member = value };` named initialization, must specify **exactly one member**;
  - Positional initialization `= { value }` is not provided, nor is `U(...)` construction—which member is selected should not be decided implicitly by position.
- Access `u.member`; reading a member that was **not the last one written** behaves as in C: reinterpret by bytes (the value must be representable), with no runtime check.
- Can be assigned, passed, returned, used as array elements / global variables / struct or class fields; addressable (POD, not subject to "structs with reference fields are not addressable").
- C ABI: members share the same address, `sizeof`/`alignof` match C, so it can directly interface with `INPUT`, `VARIANT`, `VkClearColorValue`, etc.

### 3.5 Enums (implemented)

```c
enum Color { Red, Green, Blue }
```

- Underlying type `int`; increments from 0 by default; explicit assignment `Red = 1` is allowed.
- Named constants only, not a tagged union (`variant`, 10.1.1); `match` performs a compile-time exhaustiveness check on enums (10.1.2).
- Member names must be unique within the same enum.
- **Reference syntax**: members can be used without a prefix (`Red`, both in this unit and in units imported with `use`),
  or with the qualified name `Color.Red`; across units write `unit.Color.Red`. `type.member` holds only for enums;
  members are compile-time constants, cannot be assigned, cannot have their address taken.
- **Nominal type**: there is **no implicit conversion** between enums and integers—`Color c = 1;` and `int n = Red;`
  are both compile errors; both directions require `as` (`1 as Color`, `c as int`). `as` only performs an underlying bit-pattern
  conversion and does not check whether the value belongs to a declared member.
- **Comparison**: allowed only between **the same enum type**, supporting `==` `!=` `<` `<=` `>` `>=`; the result is `bool`.
  `c == 1`, cross-enum comparison (a member of `A` compared with a member of `B`), arithmetic/bitwise operations (`+ - * / % & | ^ ~ << >>`),
  compound assignment (`c += 1`), unary `-`/`+`/`~` are all compile errors and require an explicit `as int`.
- The semantics of ordering comparison = **the ordering of the underlying integer value** (`Mid > Low` holds when `Mid = 5`), so it only holds for
  enums where "the numeric value is meaningful"; for flag enums the ordering comparison is well-defined but usually meaningless.
- Enums cannot be used implicitly as integers: array/pointer indices (`a[c]`), range endpoints (`for i in Low..High`),
  shift amounts (`x << c`), pointer offsets (`p + c`) all require an explicit `as int`.
- Enums cannot be converted to/from floating point (`c as float` is illegal), nor serve as an intermediate type for pointer/integer conversions.

### 3.6 Conversions and promotions

- Implicit conversions are allowed only for widenings that **do not lose values**:
  - unsigned small type → wider signed/unsigned (e.g. `byte`→`int`, `uint`→`ulong`).
  - signed small type → wider signed.
  - `T*` → `void*`; `T*` → `const T*`; `T* const` → `T*` (top-level const is dropped on copy).
  - `T[]` → `const T[]`; the reverse (removing const) requires an explicit `as`.
  - A read-only **class or interface handle** (`const Base base`) → the mutable handle requires an explicit
    `as` (3.7); the same holds for `ref` to such a view.
  - **Const may be added only at the outermost pointer level that is being viewed** (C++ rule):
    `T** → const T**` has **no implicit conversion** — adding const to an inner level would let a mutable
    pointer observe a const object. As in C++, `as` can force it (it is `const_cast`-equivalent; writing
    through the result to a const object is UB).
  - `T[N]` → `T*` (array-to-pointer decay, C99 6.3.2.1; same element type, const may only be relaxed;
    also applies to `void*`). This is the only conversion that drops information (the bound).
  - `T[N]` → `T[]`, `T[m,n,...]` → `T[,]` (array to dynamic array: allocates and copies).
- All other numeric/pointer conversions must be written with `as`.
- Enums and integers (excluding floating point) can only be converted explicitly with `as` in both directions; enums do not participate in implicit widening, nor implicitly convert to `int`.
- Integer literals adapt to the context type (if representable); the literal upper bound is `2^63-1`, and exceeding it is a compile error.
- Floating-point literals likewise adapt to the context: `float f = 1.5;` is legal; but `double` → `float` between **variables/expressions** still requires an explicit `as`.
- Integer arithmetic: types smaller than `int` are promoted to `int` first; mixed sign/width follows the rules above, and it is an error when no implicit conversion is possible.
- `bool` does not participate in numeric conversions; conditions must be `bool`.
- A `string` no longer converts to a pointer implicitly: use `s.cstr()` (3.3). A string literal is itself a `const char*`.
- `string` is not a numeric type and cannot be converted implicitly to/from pointers/integers.
- **Implicit struct construction** (`implicit T(one_param)`, implemented 0.0.88): when a `struct` declares an
  `implicit` single-parameter constructor, a value whose type is implicitly convertible to that parameter can be
  used where the struct is expected. Rules:
  - **struct only** (a class conversion would allocate implicitly); exactly one parameter, `public`, and not the
    struct type itself;
  - **at most one step**, matched by the built-in conversions above (no chained user conversions);
  - it applies at the **coercion points only**: initialization, assignment, argument passing, `return`;
    it does not apply inside expressions (`"a" + s`, `cond ? cstr : s` need an explicit `T(...)`);
  - in overload resolution its rank is **weaker than every built-in conversion** (exact 0 > widening 1 > implicit
    construction 2), so an overload taking the source type always wins;
  - the conversion is a constructor call, i.e. its cost (allocation, copy) is visible in the constructor body.
- `as` can be used for: numeric truncation/extension, pointer/integer conversion, `const` removal (explicit and unsafe).

### 3.7 Mutability: `const` (C++ model; decided 2026-11-17)

`const` is a **cv qualifier that belongs to the type**: `const T` and `T` are different types. This is the
C++ model, with the deliberate omissions listed at the end of this section.

- **Levels.** `const` qualifies the object it is written on:
  - `const int x` — the **object** is read-only (top-level const);
  - `const int* p` — the **pointee** is read-only (low-level const); `p` itself can be retargeted;
  - `int* const p` — the **pointer** is read-only (top-level const); `*p` can be written;
  - `const int* const p` — both;
  - `int* const* q` — the intermediate pointer is read-only; `*q` and `**q` are writable.
- **Repeated `const` is allowed and folds**: `const const int` ≡ `const int`; a declaration may repeat it at
  different levels.
- **Top-level const is dropped on copy/assignment**: `int y = const int x;` is legal; `T* const → T*` is
  implicit. Low-level const may only be **added**: `int* → const int*` is implicit; `const int* → int*`
  requires `as` (explicit and unsafe). `T** → const T**` has **no implicit conversion** (adding const at an
  inner level would let a mutable pointer observe a const object); as in C++, `as` can force it.
- **Signatures.** Top-level const is ignored in a function signature (`f(const int)` ≡ `f(int)`); low-level
  const is part of it (`f(int*)` and `f(const int*)` are different overloads). `const` therefore participates
  in overloading and in symbol names (9).
- `const` objects must be initialized. A local `const` may use a runtime expression (`const int x = f();`).
- **Pointers, slices, arrays.** `const T*` / `const T[]` are read-only views; the view itself can be
  reassigned. A `const T[N]` array has read-only elements and decays to `const T*`. `const` applies to the
  innermost base type: `const int**` is "pointer to pointer to const int".
- **`const` objects are deep read-only (C++ semantics).** Through a `const` object no field or element may be
  written (`s.x = 1`, `c.a = 1` are errors), and only `const` methods may be called (3.11). A by-value
  `const S` parameter protects the copy inside the body and does not affect the caller. For a `class` handle
  the object is shared: `const` protects this view, not the object's identity (3.8).
- **`const` fields.** `const T x;` is a per-instance read-only field: it must be initialized by every
  constructor (member initializer list `C(...) : x(v) { }`, or a default member initializer
  `const int x = 5;`), cannot be written afterwards, and participates in layout like any field. For a type
  without constructors, aggregate initialization (`S s = {x = 1, y = 2}`, `S s = {}`) counts as
  initialization. **A type that declares default member initializers but no constructor of its own gets a
  synthesized zero-argument constructor** (decided 2026-11-18): every construction site — a declaration
  `S s;`, `S()`, `new C()` — then applies those initializers instead of leaving the fields zeroed.
  Aggregate initialization is **not** available for such a type (it would have to ignore or partially
  apply the initializers); construct it instead. A type with a `const` field is still copy-constructible;
  assignment is **allowed** and runs as copy-and-swap, i.e. the destination is rebuilt as a whole rather
  than written field by field (decided 2026-11-17; C++ deletes the assignment instead).
- **Default member initializers are limited to `const` fields** (decided 2026-11-19). `const int x = 5;` is
  allowed; a non-`const` field cannot carry one — `int score = 10;` is a compile error and the diagnostic
  points at the constructor body. A non-`const` field is part of the value, so a default would have to
  interact with aggregate initialization, copy/move and the synthesized zero-argument constructor; C++
  NSDMI is deliberately **not** adopted yet. Nothing above changes: aggregate initialization and the
  synthesized constructor keep working, and a `const` field may still use `field(value)` in an
  initializer list.
- **Reassignment.** A top-level `const` variable cannot be reassigned (`const int x = 0; x = 1;` is an error).
- **`const` methods.** `T f() const` declares a read-only method: `this` is `const Self`, fields cannot be
  written, and `f()` / `f() const` may coexist as overloads. A const object can only call const methods.
  Constructors and destructors cannot be `const` (3.11).
- **Address-of and iteration.** `&const_local` / `&s.const_field` / `&const_slice[i]` yield `const T*`.
  `for const ref x in s` is read-only iteration; `for x in s` copies (a copy of a const element is mutable).
- **No backdoors.** `const_cast` is not provided — use `as` (explicit and unsafe, 4.3); `mutable` is not
  provided; `volatile` is not provided. `constexpr` / `consteval` / `constinit` are **not** introduced.
- `const` does not change layout/ABI and is **not an optimization promise**. A unit-level `const` whose
  initializer is a compile-time constant may be placed in read-only storage; writing it after `as` is UB.
- **Not allowed:** as a type-declaration modifier (a `const struct` / `const class` body was removed on
  2026-11-20 — write `const` on each field and each method) and on an anonymous function type
  (`(int) => int`). A **top-level `const` return type is accepted but ignored** (`const int f()` is treated
  as `int f()`, as in C++); `const char* f()` is a low-level const pointer and keeps its const.
- `weak const T` / `const weak T` are errors (a read-only weak reference has no use).

### 3.7.1 Type-level compile-time constants (removed 2026-11-17)

Type-level constants are no longer part of the language. `const int K = 3;` inside a `struct` / `class` body
now declares an ordinary **`const` field** (3.7), not a storage-free constant.

- Constants live at **unit level**: `const int K = 3;` at the top of a unit, referenced as `K` (same unit) or
  `unit.K` (after `use`); a `public const` is visible across units.
- `Type.NAME` (`Point.K`) is gone. Enum members keep their qualified form (`Color.Red`, 3.5), because they are
  still constants of the enum type.
- A generic struct/class can have `const` fields like any other type; there is no special "generic const
  member" rule anymore.

### 3.7.2 Compile-time constant expressions (decided 2026-11-17)

Compile-time constants are the values that can be computed before the program runs. The same evaluator is used
for **unit-level `const` initializers**, array lengths `T[N]`, `step`, `align(N)`, `match` range
endpoints, and `#if` conditions (9.2).

- Literals: integer, floating point, character, boolean.
- Unary `-` `+` `not`; binary `+ - * / %`, `& | ^ << >>`, `< <= > >= == !=`, `and` `or`.
- `as` conversions (the same truncation / sign extension / integer↔float semantics as at runtime).
- References to other constants that are **already declared and visible at this point**: an earlier unit-level
  `const` of this unit, an enum member (`Color.Red` or a bare `Red`), or a `public const` of a unit that was
  `use`d before this point. **Forward references are not supported** (`const int A = B; const int B = 1;` is
  an error: `B` is not declared yet).
- Built-in `sizeof(T)` / `alignof(T)`: the result is `isize`; assigning it to `int` and similar types
  requires an explicit `as` (`const int W = sizeof(int) as int;`).
- **Integer width is part of the fold.** A value carries the width of its type: an integer literal is the
  narrowest of `int` (32) and `long` (64) that holds it, a named constant takes its declared type, and a
  binary operation runs at the wider operand's width (unsigned if either side is unsigned). An addition
  that overflows wraps at that width (8.2), so `const int MAX = 2147483647; MAX + 1` is negative and cannot
  be an array length.
- Enum members are compile-time constants under both spellings (`Color.Red` and the bare `Red`); they
  compare with values of the same enum only, and arithmetic on them needs an explicit `as int`.
- Errors: division by zero / modulo by zero, signed division/remainder overflow (`MIN / -1`), a reference to
  a constant that is not yet evaluated (self-reference or a failed initializer), and nesting depth above 512.
- In constant expressions `and` / `or` **evaluate both sides** (at runtime they short-circuit); therefore
  `false and (1 / 0 == 0)` reports division by zero in a constant context.
- Constants of the same name coming from different `use`d units still **report ambiguity** under the normal
  name-resolution rules (they do not silently take the first).
- Not supported: function calls (there are no `constexpr` functions), `?:` conditional expressions, and
  pointers / strings / aggregate values participating in evaluation. Aggregate constants are written as
  literals or aggregate initialization (`const int[] G = {10, 20, 30};`), not computed.
- **A `const` variable is not automatically a compile-time constant.** A unit-level `const` whose initializer
  the evaluator can fold **is** a compile-time constant; any other unit-level `const` (e.g.
  `const string S = "hi";`, `const int x = f();`) is initialized at run time by the unit initializer (9.1)
  and cannot appear in the positions listed above. Local `const` variables are always run-time values.
- Enums are nominal types: enum members still require an explicit `as` to participate in arithmetic or
  comparison (`(Color.Red as int) * 2`).
- **Initializers of unit-level variables** (including non-`const` globals such as `int g = 1 + 2;`) also go
  through this evaluator: if the value can be computed it is emitted to the global data area as a constant;
  otherwise the variable is initialized at run time (9.1).

### 3.8 Reference type `class` (RC + weak; implemented, including automatically nulling weak targets upon destruction)

Values and references are strictly separated: `struct` always has value semantics, `class` always has reference semantics; the two do not interoperate.

```c
class Cat {
    string name;      // value-type field
    Cat parent;       // class reference field (nullable, strong reference)
    weak Cat owner;   // non-owning weak reference field (used to break cycles)
}
```

- **The type is the handle**: `Cat` *is* the reference type; there is no `&` type suffix, and `&` only means address-of.
- `class` and `struct` are both unit-level type declarations; they may declare fields, methods, and constructors, and support single inheritance and interfaces (see 3.9–3.12).
- They may also declare **`const` fields** (decided 2026-11-17): `const int id;` is a per-instance read-only field initialized by the constructor (3.7); class members are private by default. Type-level constants are gone — use a unit-level `const`.
- Construction: `new Cat(...)`; the parameter rules are the same as for struct construction (positional or named, all fields must be given).
  `new Cat` / `new Cat()` means all fields are zero-initialized (numeric 0, references null, `string` empty).
  A failed `new` means `abort()` (allocation failure is not reported through exceptions; a constructed reference is never null).
- `new` is only for `class`, not for `struct` (structs keep value construction `Point(...)`).
- Initialization / assignment:
  - `Cat b = a;` copies the handle (strong reference +1)
  - `Cat c = null;` a null reference (`= 0` in a declaration is equivalent to `null`)
  - `b = c;` rebinds (old target -1, new target +1)
  - `b = null;` rebinds to null
- Null: class references allow `null`; `c == null` / `c != null` are legal.
- **Null-reference check** (decided 2026-11-19): dereferencing a null class handle or a null interface
  value (a method call, a field read or a field write) throws `std.exception.NullReferenceError`, and
  `--null-check=off` removes the check, which makes the access undefined behavior (8.2). `null` itself
  stays a legal value: the check fires only where the handle is used as an object.
- Identity comparison: `c == d` compares whether both denote the same object; value comparison of classes is not supported (compare field by field when needed).
- Member access auto-dereferences: `c.name`, `c.parent.name`; writes are constrained by `const`.
- **Early release** (`del` was removed as of 0.0.47): just set the handle to null; `c = null;` releases one strong reference and sets `c` to null.
  The object is destroyed when the strong reference count reaches zero; for a shared object only your own reference is decremented. The target must be a writable class/interface
  reference lvalue (a `const` object cannot be reassigned); function parameters and `for`/`for-in` loop variables are likewise owning bindings,
  and can be set to null (see 4); `this` cannot be assigned.
- `weak Cat p;`:
  - can only be a class field (not a local variable, parameter, or return value);
  - does not increment the target's count; it is automatically set to null when the target is destroyed (zeroed);
  - reading yields a nullable borrowed handle that is valid until the end of the enclosing expression; assign it to a strong reference to keep it (+1);
  - cycles must be broken with weak references; the language provides no cycle collector, so unbroken cycles leak (see 8.3).
- **class static arrays**: `Cat[3]`, `Cat[0]` are allowed (the elements are handles); copying increments each element by +1, destruction decrements each element by -1,
  and element assignment decrements the old value by -1 and then increments the new value by +1. A struct may contain class/interface reference fields (non-POD; the compiler generates
  per-field RC, see 3.4).
- **class dynamic arrays**: `Cat[]` is allowed (`new Cat[n]`); the array holds the element handles and decrements each element by -1 on destruction. The dynamic array
  handle itself is copied/released according to the RC rules of 3.3.
- `const Cat c` (C++ model, decided 2026-11-17): `c` is a **const handle**. Through it no field can be
  written (`c.a = 1` is an error) and only `const` methods can be called; a non-const handle cannot be
  obtained from `c` without `as`. Because a class is shared, the object may still be mutated through another
  non-const handle — `const` protects this view, not the object's identity.
- **`ref` is a binding mode, not a storable type** (since 0.0.62; extended in 0.0.63): borrows appear in exactly two places, **parameters** and **return types**,
  written as the prefix keyword `ref T p` / `const ref T p` (or its older spelling `ref const T p`) / `ref T f()`; **no `T&` type suffix is introduced**. The reason: `&` in this language already is
  the unary address-of operator and the binary bitwise-and operator; modifiers in type position uniformly use a lowercase prefix (`weak`, `const`, `ref`).
  **Style: when `const` and `ref` appear together, write `const ref`** (in parameters, return types and `for const ref x in s`); the older `ref const` order is still accepted but is not to be used in new code.
  A borrow cannot be the type of a variable, field, or array element (`ref T x;` and `T&[]` are both errors, and the compiler emits a clear diagnostic).
- The borrow family shares one rule: **not storable** — the results of `for ref x in s` / `for const ref x in s`, `ref T p`, and `ref T f()` must not be assigned to another variable or field,
  must not have their address taken, and must not outlive the expression that produced them.
- `ref T` parameters (0.0.62; scalars supported since 0.0.63): `void f(ref Point p)` / `void inc(ref int x)` borrow the caller's variable;
  the callee may write to it; it cannot be rebound.
  - Borrowable types: primitive types, enums, pointers, and structs that contain **no reference-counted fields, have no destructor, and are not a union**.
    A type parameter is checked when it is substituted, i.e. at **instantiation** — `void f<T>(ref T c)`
    instantiated with a type that owns a block is an error, not a run-time surprise.
  - The argument must be an **addressable lvalue**; `ref` parameters perform no conversion (they do not accept temporaries).
  - Assignment to a struct borrow is still a "rebinding" error (as in 0.0.62); assignment to a scalar/enum/pointer borrow **writes through** to the caller's variable.
  - Method/constructor parameters do not support `ref` yet; operator parameters do (see 3.11); `extern` does not.
- `const ref T p` (read-only borrow, 0.0.62): symmetric with the `const` rules of `for ref`; the callee can neither
  rebind the parameter itself nor write fields/elements through the borrow; the argument must be an **addressable lvalue** (temporaries are not accepted).
  Difference from a by-value `const T p`: the latter protects the **copy** inside the function body, whereas the former restricts **writes to the caller's object**.
  A mutable `ref T` does not accept a `const` argument ("cannot bind a readonly variable to a mutable `ref` parameter").
- **`ref T` returns** (0.0.63): `ref T f()` returns a borrow of storage that the caller already holds.
  - The root of the returned expression must be: a `ref` parameter, `this`, a global/static variable, or storage reached through a **pointer/slice** on the path (heap memory).
    Borrowing a local variable, a by-value parameter (including its inlined fields), or a temporary is a compile error.
  - The result is **not storable** (`ref T r = f();` is an error); but it can be written through in place: `f() = v;`, `f() += 1;`, `f().field = v;`,
    and it can also serve as the argument of a `ref` parameter, or be bound in a `for ref`.
  - A `const ref T` position cannot be returned as a mutable `ref`; the result of `const ref T f()` can only be read.
- **A `const` method cannot return a mutable `ref T`** (decided 2026-11-18): its receiver is borrowed
  read-only, so anything it hands out must be read-only as well — otherwise a `const` handle could write a
  field through `ref int at() const` and the deep-readonly rule would hold only at the call site. The
  read-only spelling `const ref T at() const` is the way to lend a view; a mutable borrow comes from a
  non-`const` method or a free function. (This also covers passing a `ref` parameter's borrow through: the
  compiler does not track provenance, and the caller may have aliased the receiver.)
- **Parameters are owning local variables** (since 0.0.47): on entry to a function a reference is retained for the parameter value, and on exit (including `return`/`break`/
  `continue`) it is released. Therefore the body may freely rebind a parameter (`c = null;`, `c = other;`) without affecting the caller.
  The caller still passes arguments by value and releases its own temporaries, so the net reference count is unchanged; later, borrow/escape analysis may optimize away the
  retain/release without changing semantics.
- `sizeof` / `alignof` applied to class/interface types (**implemented**): a class yields the **handle** size (8),
  an interface yields the interface value size (16); the size of the object itself is not exposed; an enum yields the underlying `int` (4).
- Forbidden (phase one):
  - passing a class to an `extern` C function (the C side does not participate in reference counting);
  - passing a dynamic array `T[]` to an `extern` C function (it carries an RC header internally and is not C layout);
  - **taking the address of a class/interface reference** (`&c`, `&i`), and **taking the address of a member/array element reached through a reference**
    (`&c.x`, `&h.items[0]`, `&catArray[i]`, `&h.v[0]`): the RC lifetime cannot be expressed with a raw pointer,
    and retain/release could be bypassed (backlog U21). When an in-place borrow is needed, copy it to a local variable first and assign it back when done:
    `S tmp = c.s; api(&tmp); c.s = tmp;`. Taking the address of a pure-value struct / fixed-size array element / pointer variable is unaffected.
  - `weak` on local variables, parameters, or return values;
  - `const_cast` / `mutable` / `volatile` (use `as` for an explicit const removal, 3.7);
- Memory: atomic reference counting (relaxed increments, release decrement + acquire fence before destruction); fields are released in reverse declaration order; weak references are not counted.
- Threads: objects may be shared across threads; the destructor runs on the thread that releases the last reference and must be thread-safe.

### 3.9 Inheritance (single inheritance)

```c
class Widget {
    Rect bounds;
    Widget(Rect b) { bounds = b; }
    virtual void paint(Canvas c);              // abstract method (no body)
    virtual bool event(Event e) { return false; }
}

class Button : Widget {
    string label;
    Button(Rect b, string s) : Widget(b) { label = s; }
    override void paint(Canvas c) { ... }
}
```

- **Only single inheritance is allowed**: `class D : B`; `B` is a class (for interfaces see 3.10).
- Field layout: base class fields come first, derived fields after; field names must be unique along the whole inheritance chain (shadowing is not allowed).
- Methods are **static-dispatch by default** (build 160); a base class method that subclasses may replace must be declared `virtual`, and the replacement is written `override` with a signature that matches exactly. Overriding a non-virtual method is a compile error (methods are never silently shadowed).
- Same name with a different signature → overload (see 3.11), not an override.
- A class with unimplemented methods (declared only, without a body) is an **abstract class** and cannot be `new`ed; no `abstract` keyword is introduced.
- An abstract class may have constructors, for derived classes to call with `: Base(...)` in their initializer list (3.12).
- A base class handle may point to a derived object (implicit upcast); converting a base class to a derived class requires an explicit `as`, which yields null on failure.
- Virtual calls during construction are dispatched C++-style (see 3.12).
- `super.m(...)` **only resolves along the class inheritance chain**; interface default methods cannot be called with `super` (error). `super` is only that qualified call form: a base-class **constructor** call is an initializer-list item (3.12).
- No multiple inheritance and no `final`; a derived class may itself be inherited from.

### 3.10 Interfaces `interface`

```c
interface IIterable {
    IEnumerator get_iterator();
}

interface IList : IIterable, IObject {      // an interface may inherit multiple interfaces
    int count();
    void add(int x);
    bool is_empty() { return count() == 0; }  // default method
}

class Vector : IList { ... }                  // implements only interfaces
class Button : Widget, IFocusable { ... }     // the first is a class, the rest are interfaces
```

- `interface`, like `class`/`struct`, is a unit-level type declaration; it has no fields and no constructors, only methods (which may have default implementations).
- A class may implement multiple interfaces: in the `:` list there is **at most one class, and it must come first**; the rest are interfaces.
- Interface methods are abstract and virtual by default; write `override` when implementing them.
- **Operator members are static contract members** (`Self operator+(Self a, Self b);`): they have no vtable
  slot, are resolved at the call site, and never make an implementor abstract — an interface may therefore
  carry both ordinary methods and operators (`interface IBoth { int area(); Self operator+(Self, Self); }`).
  The receiver is the first operand and must be spelled `Self`; the operands after it are any types
  (`Self operator<<(Self b, byte c)`, `Self operator*(Self a, int k)`, `bool operator<(Self a, int b)`).
  The receiver may borrow: `ref Self` (mutate in place) or `const ref Self` (read-only). A `ref Self`
  receiver needs a free `operator` whose first parameter is `ref` (or a struct member operator, whose
  `this` is borrowed); a `const ref Self` receiver is satisfied by a free `const ref` (or by-value) first
  parameter, or by a `const` member operator (whose `this` is `const Self`, 3.11).
- A default method may only call interface methods and cannot access fields; **it may also use `this`**,
  whose type is `Self`: a contract operator applies to it (`this[0]`, `this + x`), `return this` returns
  the implementor, and `this` may be passed to a `Self` parameter. Because the receiver is then the
  implementor, such a default **has no itable slot**: it is materialized per implementor as an ordinary
  static-dispatch method (for a `class` as well as a `struct`), and calling it through an interface value
  is a compile error (3.15.6); a default that never mentions `this` keeps the dynamic form. When two interfaces provide
  default implementations with the same signature, the class must explicitly override them, otherwise it
  is a compile error. A **derived interface's default does not silently replace the base interface's**
  (decided 2026-11-18, the conservative reading): `interface D : B` that re-declares a default of `B` makes
  the signature ambiguous for an implementor, so the class must write the `override` explicitly. The rule
  is uniform (`IBase`/`IDerived` in a chain, or two unrelated interfaces) and a one-line `override` is the
  answer; the alternative (a derived default winning) makes the resolution depend on the inheritance graph
  in a way a reader of the class cannot see.
- An interface value is laid out as two machine words `{object pointer, class descriptor pointer}`; it may be `null`.
- Conversions: `class → interface` is implicit; `interface A → interface B` (A inherits B) is a static conversion;
  `class → derived class` and `interface → class` use `as` for a runtime check (along the desc base class chain), yielding `null` on failure;
  the downcast `interface → derived interface` is not supported in phase one (error).
- Interface references are strong references: copying is +1, overwriting is -1; `weak` is only supported for class fields, and **interfaces do not support `weak` (not done in phase one)**.
- An interface cannot be used as a parameter of an `extern` C function.
- **Generic interfaces are real interfaces** (0.0.108): `interface ISeq<T>` instantiates on use, and an instantiated
  interface (`IIterator<int>`, `IIterable<int>`) can be implemented by a class (`class Cursor : IIterator<int>`),
  implemented by a generic class through its parameter (`class Src<T> : IIterable<T>`), used as a **value type**
  (16-byte `{obj, desc}`, dispatch through the itable), a return type, a field/local type, a constraint
  (`T: IIterable<int>`) and an `as` downcast target. Interface inheritance may be generic
  (`interface Derived<T> : Base<T>`); each level is an interface value of its own type. What stays closed is the
  *value type → interface value* direction: a `struct` cannot be handed out as an interface value (that is boxing,
  3.15.8), so a struct container returns a **class** cursor.
- A `struct` may also implement interfaces (`struct S : I1, I2`): after `:` you may **only list interfaces**; this is
  **static satisfaction**, which produces no value type and generates no vtable (see 3.15.7 and backlog U33).
  - An interface method with a default implementation is **materialized as an ordinary method of `S`** when `S` does not
    declare it: the body is instantiated with `this` = `S`, so its calls resolve statically (monomorphization, not
    inheritance — no layout change, no vtable slot, no `super`). A default written in another unit is resolved in that
    unit, while `S`'s own members stay reachable.
  - Everything else must be implemented explicitly; a missing implementation is a compile error
    (naming the interface and the full signature). **`override` is optional for a struct** (0.0.108): the `:` list is
    the contract and a value type cannot be subclassed, so the marker would not distinguish anything; a class still
    requires it on every interface method.
  - Two unrelated interfaces providing defaults with the same signature must be resolved explicitly, exactly as for a
    class; a default on a derived interface refines the base interface's default and wins.
  - A default is materialized when its body reads `this` **or** its signature mentions `Self`: neither
    form has an itable slot (object safety, 3.15.6), so materialization is the only way to reach its body.
    The clone's `Self` slots are substituted with `S` (build 248).
- `for-in` does not depend on interfaces for the **static** path: a container with `iterator()` and a struct cursor
  is iterated with static dispatch and zero allocation (6.4.2). A cursor that is an **interface value**
  (`IIterator<T>`, the shape `IIterable<T>` returns) is iterated through the itable — same loop, dynamic dispatch,
  one RC-managed 16-byte local.

### 3.11 Methods, Overloading and Operators

- Class method: `ret name(params) { ... }`; `this` is a reference to the current object (for a class, the object handle; for a struct, an lvalue alias of the object itself),
  and is implicitly available; write `this` explicitly when disambiguation is needed; `this` is not a pointer and its address cannot be taken.
- Fields can be accessed directly inside a method; `this` cannot be rebound.
- **`const` methods** (C++ model, decided 2026-11-17): a method declared `ret name(params) const` is a
  read-only method — `this` is `const Self`, fields/elements cannot be written through it, and it may be
  called on a `const` object. `f()` and `f() const` may coexist as overloads; a `const` object selects the
  `const` overload, a non-const object prefers the non-const one. `const` is part of the signature and
  participates in overriding (an `override` must match the base method's constness). Constructors and
  destructors cannot be `const`.
- A read-only API boundary may also use a read-only `interface` (3.10); both mechanisms coexist.
- Methods are **static-dispatch by default** (build 160): `virtual` marks a method a subclass may replace, and `override` must have a signature identical to the base class/interface method. A subclass may not redeclare a non-virtual base method (compile error, not silent shadowing).

**Overloading** (free functions, methods, constructors, operators):

1. Filter candidates by name + number of arguments;
2. Argument conversion rank: exact match (including literals adapted to the parameter type) = 0; implicit widening = 1; incompatible candidates are eliminated;
3. Pick the unique candidate that is "no worse than every other candidate and better in at least one respect";
4. No unique best → ambiguity error;
5. No user-defined conversions, no default parameters, and no variadic parameters; the return type does not participate in overload resolution.

- Method overloads **merge the same-named methods of the base class** (the derived class's overload set includes the base class's overloads).
- The same signature must use `override`; a different signature is an overload.
- `extern` C functions do not participate in overload resolution; neither does `main`.

**Operator overloading** (free functions, plus member operators on a `struct` or `class`):

```c
// free function form
Point operator+(Point a, Point b) { ... }
bool  operator==(Point a, Point b) { ... }

// member form: only inside a `struct`; the receiver is the implicit `this` (a borrowed pointer)
struct Vec {
    int n;
    Vec operator+(Vec o) { ... }       // binary: 1 parameter
    int operator[](isize i) { ... }    // read:   1 parameter
    bool operator==(Vec o) { ... }
}
```

| Operator | Allowed | Description |
|---|---|---|
| `+ - * / %` | Yes | Binary; may return any type |
| Unary `-` | Yes | |
| `< <= > >=` | Yes | Must return `bool` |
| `& \| ^ << >>` | Yes | Bitwise operations |
| `[]` | Yes | **Read and write through one member operator**: `T operator[](I i)` reads; `ref T operator[](I i)` returns a borrow, so `c[i]` is a writable lvalue (`c[i] = v`, `c[i] += v`, `for ref` all work). **Member-only since build 161** — the free-function form `T operator[](C c, I i)` is a compile error (it copied the receiver per element access). **`operator[]=` was removed (0.0.101)**: a computed setter with no storage to borrow is no longer expressible |
| `== !=` | struct only | A class's `==` is fixed as identity comparison and cannot be overloaded; a struct that does not define `==` is not comparable |
| `()` `=` `&& \|\|` `as/new` | No | Conflicts with the built-in semantics |

- An operator must have at least one struct/class operand; built-in types cannot be redefined.
- **Member operators** (`implemented 0.0.87`; allowed in a `class` since build 160) are static-dispatch unless
  declared `virtual`. The receiver is the implicit `this`: a `struct`'s `this` is the object address, a
  `class`'s `this` is the handle, so `s[i]` / `s == t` / `c[i]` cost no copy and no retain/release. The
  parameter list is the free form minus the receiver (binary 1, unary 0, `[]` 1) and a member operator wins
  over a free function with the same signature. **`==` / `!=` are the exception for a class**: they stay
  identity comparison and are rejected as members (overloading them would make `a == b` mean two different
  things for the same type depending on the static type).
  > Why a class member operator matters: the free-function form copies its receiver on every call, and a
  > class cannot take a `ref` parameter, so `for x in c` over a class container with a free `operator[]`
  > costs one retain/release pair per element (measured 7.5 ns vs 0.15 ns/element; see
  > `agents/stdlib/containers.md`). The member form removes that entirely.
- An operator function is an ordinary function and may be placed in any unit; a duplicate definition or a resolution ambiguity → compile error.
- **Compound assignment is not an overloadable operator** (`+=`, `-=`, `*=`, `<<=`, … are absent from the
  table above, and `x++` / `x--` are statement-only). For a numeric or pointer target — including a `ref`
  subscript whose element is numeric — it is a built-in in-place operation (evaluated once). For a **user
  type** (struct / class / interface) it is sugar: `x op= y` is rewritten as `x = x op y` and uses
  `operator<op>`, so the same operator set applies and `x++` needs an `operator+` that can consume `1`.
  The left side must be side-effect free to re-evaluate — a name, a field, or a subscript whose indices
  contain no calls — because the rewrite spells it twice; an impure target is a compile error that asks
  for the update to be written out.
- **Operator parameters may be `ref` / `const ref`** (free functions only): the receiver is the first
  operand, so `Buf operator<<(ref Buf b, byte c)` mutates the caller's value. A mutable `ref` argument must
  be an addressable lvalue and binds no conversion; a readonly borrow never binds a mutable `ref`.
- **An operator may return `ref T`** (`ref Buf operator<<(ref Buf b, byte c) { ... return b; }`): the result
  denotes the borrowed storage (same rules as `ref T f()`), so chained calls (`b << 1 << 2`) pass the borrow
  on instead of a temporary, while `Buf x = b << 1` copies the borrowed value. Borrowable types only
  (3.8: no reference-counted fields).
- When `!=` is undefined it is derived as `not (a == b)`; for a class this means identity inequality.
- **A derived comparison evaluates its operands right-to-left** when the derivation swaps them
  (`>` and `<=`), because the receiver becomes the other operand. Combined with the previous item,
  `a != b` is `!(a == b)` and `a >= b` is `!(a < b)` keep the source order.
- The **ordering family is derived from `operator<`** when the operator itself is not declared:
  `a > b` ⟺ `b < a`, `a <= b` ⟺ `!(b < a)`, `a >= b` ⟺ `!(a < b)`. A declared operator always wins,
  so a type keeps control when it needs the native semantics (floats with NaN) or wants to avoid the
  extra call. Deriving `<=` this way is only correct for a **total order**; the contract `IOrder`
  (3.15.9) promises exactly that, and a float set containing NaN is not totally ordered — floats
  carry their own `<=`, so derivation never applies to them.
- Unary `~` (bitwise NOT) may be overloaded (`operator~`, 1 parameter).
- Precedence/associativity are unchanged.

### 3.12 Constructors and Construction Order

```c
class Widget {
    Rect bounds;
    Widget(Rect b) { bounds = b; }
    Widget() { bounds = zero_rect(); }
}

class Button : Widget {
    string label;
    Button(Rect b, string s) : Widget(b), label(s) { }
}
```

- A constructor has no return type and its name equals the class name; it may be overloaded; `new T(args)` is resolved by the rules of 3.11.
  **Named arguments work for constructors too** (2026-11-19): `T(b = 2, a = 1)` / `new T(b = 2, a = 1)` select the
  overload whose parameter names they spell out (each exactly once) and are reordered to positional order;
  positional and named cannot be mixed, and the names must match one overload exactly.
- **`implicit`** (implemented 0.0.88, struct only): `implicit string(const char* c) { ... }` marks a single-parameter
  constructor as the implicit conversion path (see 3.6). It must be `public`, and writing it in a class body or
  union is a compile error. The marker is contextual: `implicit` is a modifier only in front of a constructor name.
- On allocation: the object's memory is fully zeroed, `rc = 1`, and the vtable points to the topmost base class.
- **The construction prologue** (decided 2026-11-17, unified 2026-11-18): the list after the parameter list,
  `Ctor(params) : item, item, ...`, is the only place construction steps are written. An item is one of
  - `Base(args)` — the **base constructor** call (the base class's name, first item of the base list);
  - `CtorName(args)` — a **delegating constructor** call: another constructor of this same type, which
    constructs the whole object; the delegating constructor then runs only its own body. It cannot also
    name the base or a field, may not delegate to itself, and the delegating chain must not form a cycle;
  - `field(value)` — a **field initializer**, the way to initialize a `const` field (3.7). The same form is
    available to `struct` constructors (3.4). It is **direct initialization**: when the field's type has a
    constructor, `value` is that constructor's argument (`: a(1)` calls `E(int)`; `implicit` is not
    needed), and `: a(E(1))` means the same thing. A field the list does not name is default-constructed.
- **Execution order is fixed** (C++ model), regardless of the order the items are written in:
  **delegate → base → members in *declaration* order (from their initializer, else their default
  constructor) → body**. Each member is constructed **exactly once**: an initializer replaces the default
  construction, it does not follow it. An omitted base item calls the base class's zero-argument constructor (an error if it does not
  exist); an omitted field initializer falls back to the field's default member initializer
  (`const int x = 5;`). Every `const` field must end up initialized exactly once; a missing initialization is
  a compile error. A type that declares default member initializers but **no constructor of its own** gets a
  synthesized zero-argument constructor, so `S s;`, `S()` and `new C()` all apply them (aggregate
  initialization does not apply to such a type, 3.7).
- `super(...)` is **not** a constructor call form (it never was a statement): a base-class constructor call
  is the `Base(args)` item above. `super.m(...)` remains the qualified form that calls a base-class method
  (3.9), and is unrelated to construction.
- Construction order across layers: each constructor layer runs its prologue and then its body; on entering
  each constructor layer, the vtable is updated to that layer.
- Virtual calls during construction are C++-style: they dispatch only to the current construction layer and above, and never enter a derived implementation that has not been constructed yet.
- A class that declares no constructor keeps the zero-initialization of `new T`; once a constructor is declared, `new T` is legal only if a zero-argument constructor exists.
- An abstract class may have constructors, but cannot be `new`ed.
- User destructors are specified in 3.13 and implemented for both `class` and `struct`; on a class the
  destruction runs as the user destructor first, then the fields in reverse order, driven by the destroy
  entry of the vtable (see 8.3).

### 3.13 Destructors `~T()`

```c
class File {
    int fd;
    File(string path) { fd = open(path); }
    ~File() { close(fd); }        // called when the reference count reaches zero
}
```

- At most one destructor per class: `~ClassName() { ... }`; no parameters, no return value, cannot be overloaded.
- Invocation timing: when the strong reference count reaches zero, the **user destructor runs first**, then the fields are released in reverse order (derived fields → base class fields), and finally the memory is released.
- A class handle or a struct with a destructor held in a **unit-level variable** is destroyed by the exit
  sequence (9.1); that sequence is the exact reverse of the initialization sequence.
- Inheritance: the derived destructor runs first, then the base class destructor (C++ order); the base class destructor is virtually dispatched through the destroy entry of the vtable.
- Virtual calls inside a destructor are dispatched C++-style to the current destructor layer.
- It cannot be called manually; setting the handle to null (`c = null;`) only decrements the reference, and it triggers only when the count reaches zero.
- A cycle never reaches zero, so the destructors in a cycle never run (only after a weak reference breaks the cycle).
- A destructor must not store `this` into a field/global (that would resurrect the object); in phase one this can be constrained by documentation for now.
- When a constructor aborts, the destructor is not run; when a constructor throws, the partially constructed object is cleaned up as described in 3.14.
- A destructor may run on any thread (when the object is shared); resource release must be thread-safe.

### 3.14 Exceptions (implemented in 0.0.81–0.0.83)

```c
class MyError : Exception {
    int code;
    MyError(string msg, int c) : Exception(msg) { code = c; }
}

void f() {
    if bad { throw new MyError("bad", 42); }
}

void g() {
    try {
        f();
    } catch (e: MyError) {
        puts(e.message);
    }
}
```

- **Unchecked**: functions do not declare `throws` and any function may throw; the caller bears the risk.
- Only subclasses of `std.exception.Exception` may `throw`/`catch`; `Exception` is the **base class** (single inheritance).
- `throw expr;` is a statement; `expr` must be an `Exception` handle (not `null`).
- `catch (e: T)` matches by the class descriptor's base chain, in written order; `e` is a read-only borrow. `T` must be a class derived from `Exception`; an interface is not a valid catch type.
- `finally` runs on every exit of the `try` statement: normal completion, a matched `catch`, `return`, `break`, `continue`, and an exception that is caught or propagated. It runs once per exit.
- A `finally` body must not `return`, `break`, `continue`, or `throw` directly (compile errors). A nested `try/catch` inside `finally` is allowed; an exception that escapes it aborts, i.e. cleanup code must not throw.
- An exception object is an RC-managed class handle. The runtime pending slot owns one strong reference; a catch scope owns it until the scope ends. `throw e;` inside a catch rethrows (retains the object).
- Destructors must not throw; a pending exception inside a destructor aborts.
- Uncaught → `abort` at `main` (or at a thread/foreign-call boundary), after printing one line to
  `stderr`: `Uncaught exception: <unit>.<Type>: <message>` (2026-11-19).
- Exceptions do not cross the C ABI and do not cross threads.
- A constructor may throw: the partially constructed object is zero-initialized and released through its destructor, then the exception propagates.
- **Implementation (B2)**: the function ABI is unchanged. A thread-local pending slot (`flag` + object pointer) is checked after every mu_lan call. On `throw`, the function releases its active scopes and returns a dummy value with the flag set; callers propagate it until a matching `catch`. `extern "C"` calls are not checked. No platform unwind tables, personality functions or unwinding runtime are needed; only libc and TLS are used, keeping Windows/Linux/macOS on one lowering.
- **May-throw optimization (0.0.83)**: a conservative module-local analysis marks functions whose bodies are visible and cannot return with a pending exception. Calls to them skip the pending check; every external/precompiled/static-library/dynamic-library/unknown-indirect target is treated as may-throw, so the optimization never constrains the future library ABI. Cross-library summaries and `nothrow` contracts are not implemented.
- `finally` is emitted by duplicating its body on each exit path, so it needs no platform EH support. Internal design notes: `agents/eh-b2-design.md`.

---

### 3.15 Generics (implemented in 0.0.64–0.0.72)

A member may be declared `const` (`interface Sized { int size() const; }`): it is a read-only contract,
satisfied only by a `const` implementation and reachable through a `const I` view (3.7).

#### 3.15.1 Implementation strategy: monomorphization

- Uses **monomorphization**: a separate type/function instance is generated for each combination of type arguments.
- The instantiation result is an **ordinary declaration** (an ordinary `Type` + an ordinary `StructDecl`/`ClassDecl`/`InterfaceDecl`/`FuncDecl`),
  so `same_type`, symbol mangling (`mangle`), layout computation, and backend code generation **do not need to be aware of generics**.
- **A generic definition must be visible at the point of instantiation** (currently the whole program is loaded from source). Generic IR/metadata is not serialized into the object file;
  if precompiled libraries are done later, that requires a separate project.
- No type erasure and no runtime dispatch. Space is traded for time, to be optimized later.

#### 3.15.2 Declaration syntax

Type parameters are written after the name and before `:` and `{`:

```c
struct Vec<T> { T[] data; }
class Box<T> { T v; }
interface ISeq<T> { isize length(); T at(isize i); }
T max2<T>(T a, T b) { return a > b ? a : b; }
```

- Multiple parameters are supported: `struct Pair<K, V> { K key; V val; }`
- Constraints are supported: `<T: I>` / `<T: A & B>` / `<T: int | long>`
- A type parameter name acts as a type name inside the **declaration body** (fields, parameters, return types, local variables, `cast`, `sizeof`, and `new` all apply).
- Generic parameters apply only at the **type level**: `struct` / `class` / `interface` / free functions.
  Method-level generics (`U map<U>(...)`) and generic type aliases are not supported yet.

**Constraint syntax**

| Syntax | Meaning |
|---|---|
| `<T>` | No constraint |
| `<T: I>` | `T` satisfies the interface `I` (structural, see 3.15.5) |
| `<T: A & B>` | `T` satisfies both `A` and `B` |
| `<T: int \| long>` | `T` is one of the enumerated concrete types |

- `&` and `|` **must not be mixed in the same constraint list** (compile error; split them into multiple type parameters).
- An interface in a constraint may be a generic instantiation: `<T: ISeq<int>>`.

#### 3.15.3 Usage syntax

```c
Vec<int> v;
Vec<Vec<int>> nested;            // `>>` split apart
int n = max2<int>(1, 2);         // explicit type arguments
int m = max2(1, 2);              // inferred: `T = int` from the argument types
Pair<int, long> p = Pair<int, long>(3, 9);   // generic struct value construction
Box<int> b = new Box<int>(7);    // generic class construction
```

- **A free function call may omit the type arguments**; they are then inferred from the argument types
  (build 251). An explicit list is always used as written and is never checked by inference.
- **A generic function can be qualified with its unit** (`std.option.unwrap_or(o, d)`), with or without
  an explicit list (`std.option.unwrap_or<int>(o, 0)`), exactly like an ordinary function; two visible
  templates of one name make the unqualified call ambiguous, and qualifying picks one (build 262).
- **Inference is structural and argument-driven**: every argument type is matched against its declared
  parameter *pattern*, top-down, and each type parameter is bound at its first occurrence:
  - a bare parameter `T` binds the argument type itself; a `const` argument binds the same `T` (the
    top-level `const` is a view, 3.10, so `f(const int)` instantiates `f<int>` and the body can write
    its own `T` local);
  - `T*`, `T[N]`, `T[]`/`T[,]` bind the element type; the slice rank must agree, and the static array
    length is not part of the inference (only the element is). A static array fits a `T*` parameter
    through the array-to-pointer decay and a slice parameter of the same rank through the
    `T[N] -> T[]` conversion (3.6); the conversion itself still runs at the call. Decay applies to a
    one-dimensional array only, so `int[2,3]` does not match `T*` (an explicit list is rejected there
    too); a two-dimensional slice pattern `T[,]` does take `int[2,3]`.
  - a generic pattern `Vec<T>` / `Box<Vec<T>>` matches an instance of the same template and binds from
    its type arguments, so nesting any depth works;
  - `ref T` / `const ref T` bind the referent, i.e. the argument's own type;
  - `(T) => R` and `delegate (T1) => R` match the parameter and return types the same way (a delegate
    pattern also accepts a plain function value, which converts to a delegate).
- **The return type is never used for inference.** A type parameter that no parameter mentions therefore
  cannot be inferred: `T empty<T>()` must be called as `empty<int>()`.
- **A failed inference is a compile error with the reason**: two arguments giving different types for one
  parameter (`max2(1, 2L)`), an argument whose shape does not match the pattern (a `Thing` value for a
  `T*` parameter, or `null` / a `void` / a range argument, none of which has a type of its own), or a
  parameter no argument determines. A deduced argument must also satisfy the template's constraint
  (`T: I`): `max2(p, q)` with a `Point` that has no `operator<` reports that the *inferred* `T = Point`
  does not satisfy `IOrder` and that an explicit list can choose another type. An argument that cannot
  be typed before its parameter type is known (`Some(3)` without a target variant type, an overloaded
  function name used as a value) is reported with that reason plus the hint to write the list.
- **Two visible templates of the same name are ambiguous** for a bare call exactly as they are for an
  explicit list: qualify with a unit name.
- **Scope**: only **free functions** infer. Method-level generics, constructors and a type in type
  position (`Vec<T>` inside another declaration) still require the arguments to be written out.
- `auto` may be used to receive the result.
- In an expression, `name<...>(` is resolved as a generic call/generic construction: it first tries to resolve as a type argument list,
  and it only holds if the resolution succeeds and is immediately followed by `(`; otherwise it falls back to being a comparison expression (`a < b`).
- The `>>` of nested arguments is treated as two `>`.

#### 3.15.4 Instantiation rules

- **Lazy**: `Vec<int>` is instantiated on demand when it is encountered; the same `(template, argument list)` reuses the same instance.
- **Canonical name**: `Type::name` is `Vec<int>`, `Vec<Vec<int>>` (one space after the comma),
  which keeps the string comparison of `same_type` stable.
- **Symbol name**: the instance name enters a symbol through the injective mangling of 12 #88
  (`Vec<int>` → `Vec_3C_int_3E_`): the unit/name/parameter components are separated by `$`, every
  literal `_` is doubled, and every other non-alphanumeric byte is written `_XX` (uppercase hex).
- **Recursive instantiation** (needing itself again during instance expansion with no cached instance) is an error: `recursive generic instantiation`. An expansion whose next instance never repeats a key (a strictly larger type argument, e.g. `Deep<Box<T>>`) is caught by a nesting limit (256 levels) and reported with the same diagnostic instead of exhausting the stack.
- An instantiated class follows the vtable/`class_desc`/RC path of an ordinary class; an instantiated struct follows the ordinary struct layout.
- Generic inheritance `class D<T> : Base<T>` supports a single base class: the base class is instantiated first, and the fields are flattened with "base class first";
  the base-class initializer (3.12), method inheritance, and vtable slot reuse are the same as with ordinary inheritance.

#### 3.15.5 Constraint semantics: type set + member set

Each type name defines two things:

| Branch | Type set | Member set |
|---|---|---|
| Scalars such as `int` | Single element | Built-in operators |
| `enum` | Single element | Comparison, `as`, `match` |
| `struct S` | Single element | Fields + methods |
| `class C` | `C` and all subclasses (open) | Public fields + methods |
| `interface I` | All types implementing `I` (open) | The methods of `I` (including default implementations) |

**Dual rules**:

```
T: A & B  →  type set is the intersection (narrower)  →  available members of T are the union (richer)
T: A | B  →  type set is the union (wider)  →  available members of T are the intersection (poorer)
```

**Structural satisfaction (important)**: `T: I` **does not require** `T` to declare `: I`; it only checks whether the shapes match:

- Method members: `T` has a method with the same name and exactly the same parameter types and return type (looked up along the base class chain and in interface default methods).
- **A default method still counts as a required member** (decided 2026-11-18): `interface IVal { int val(); int twice() { return val() * 2; } }` needs `twice` on a structurally-satisfying `T`, even though a declaring implementor (`struct S : IVal`) gets it materialized from the default. A structural relation has no place to materialize a default (there is no declared itable), so the type must provide the method itself; Rust's trait defaults work differently because an `impl` block is where the default would be attached.
- Operator members: the **receiver is the first operand and must be `Self`**; the operands after it are
  matched **as declared**, exactly (a stream `Self operator<<(Self b, byte c)`, a scaled arithmetic
  `Self operator*(Self a, int k)`, a comparison `bool operator<(Self a, int b)`). An implementation is a
  built-in operator when every operand is the type itself (`T`), or an `operator` free function / a struct
  member operator of a visible unit. A **subscript** is the same shape (`T operator[](Self c, isize i)`);
  its index is never `Self`.
- **A `ref` receiver** declares that the operator mutates the receiver in place:
  `Self operator<<(ref Self b, byte c)` (mutable) or `Self operator<<(const ref Self b, byte c)`
  (read-only). Satisfaction requires a matching implementation form: a mutable `ref Self` needs a free
  `operator` whose first parameter is `ref`, or a struct member operator (its `this` is borrowed); a
  by-value implementation only touches a copy and does not satisfy it. A `const ref Self` is satisfied by
  a free `const ref` (or by-value) first parameter, or by a `const` member operator (`this` is
  `const Self`, 3.11). Only the receiver may be a `ref` parameter in an interface
  operator declaration; the remaining operands are ordinary values.
- A `ref T` return satisfies a declared `T` (a borrow is usable as a value), and a declared `ref T` needs a
  `ref` implementation — the same rule as the subscript above. In an interface declaration only
  `operator[]` may return a borrow: the witness type has no receiver storage to hand out.
- **Subscript members are asymmetric**: `operator[]` takes the receiver plus an index or key, so only the
  *receiver* operand has to be `Self`; the operands after it may be any type. A signature must therefore
  spell the receiver explicitly (`T operator[](Self c, isize i)`), because an interface member has no
  `this`. A container satisfies it with a member `T operator[](isize)` (the receiver is the implicit
  `this`; the free-function spelling is a compile error since build 161), and a `ref T` implementation
  satisfies the
  declared `T`, since a borrow is usable as a value. A contract may declare the return as `ref T`
  instead, and then only a `ref` implementation satisfies it — that is what lets a generic body write
  through elements (`c[i] = v`) without an `operator[]=` or a second contract. A built-in block (`T[]`)
  satisfies the value form: its `length()` and subscript are compiler-lowered, so they count as members.
- Therefore **built-in types satisfy interfaces at zero cost**: `int` satisfies `Self operator+(Self, Self)` thanks to the built-in `+`.
  The ready-made contracts (`IAdd`, `IOrder`, `INumber`, …) live in `std.interfaces` (3.15.9).

**Exact matching, no implicit conversion**: constraint checking performs no implicit widening and no overload "best candidate" selection.
`Money operator+(Money, long)` does not satisfy `Self operator+(Self, Self)`.
(Consistent with Rust / C# / Go / Swift; the C++ concept "expression validity" model is not adopted.)

**Open branches do not enumerate implementors**: when checking interface members, only the declared signatures of the interface are used; all implementing types are not traversed.

**The body is checked against the contract (build 156)**: a generic function whose type parameters are
all constrained by a conjunction of interfaces is checked twice — once at its **definition**, with each
type parameter bound to a *witness* that carries exactly the constraint's members, and once per
instantiation as before. The definition-time check is what makes "it compiles" mean "it works for every
`T` that satisfies the contract": a body may use only the members the contract declares, so writing
through a value subscript, or reaching into a field of the type parameter, is reported at the
definition rather than at the first instantiation that happens to differ. Unconstrained parameters and
`A | B` bounds are not witnessed (there is no single member set) and keep instantiation-time checking.

**Diagnosing an instantiation-time failure**: a diagnostic raised inside an expanded body carries the
instantiation chain (`while instantiating \`pick<int>\` at file:line:col`) and, when a parameter is
unconstrained, a hint that the body was not checked at the definition and that a constraint would move
the check there. `muc check --strict` (also accepted by `build`) additionally warns about a generic
that is **never instantiated**: its body is never checked, so a broken template cannot pass silently.
`--strict` is opt-in because a library may legitimately ship generics for its users to instantiate.

**Members of `|`**: the body of a function with `T: int | long` may only use operations common to both (arithmetic, comparison, assignment);
when an open branch (`IComparable`) is mixed with a concrete type, the member set is usually empty; this is a semantic necessity, not a defect.

The **diagnostic** must state what is missing, for example:

```
type `Money` does not satisfy `IAdd` for type parameter `T`: no `operator+` accepting `(Money, Money)`
```

#### 3.15.6 `Self`

- `Self` is **valid only inside an `interface` body** and denotes the "implementor type".
- The same substitution applies everywhere the member is compared with an implementation: as a
  constraint (`T: I`) `Self` becomes the type argument, and a declaring implementor (`class C : I`,
  `struct S : I`) spells its **own** type in that slot — `bool same(Self o)` is implemented as
  `bool same(C o)`. Spelling the interface instead (`bool same(I o)`) does not satisfy the member.
- As a dynamic interface type, a method containing `Self` **does not enter the itable** (object safety):
  its parameter/return types differ per implementor, so it has no slot in the global signature-keyed
  table. The same holds for an interface default whose **body reads `this`** (the receiver is used as
  `Self`). Both forms are **materialized per implementor** instead — a struct gets an ordinary method,
  a class gets one too (it takes no itable slot). Calling such a member through an interface value
  is a compile error that points at the concrete type; the other members of the same interface remain
  dispatchable, so the interface itself stays usable as a value type for them.
- `Self` may only be used bare (`Self x`); nesting is not supported yet (`Self[]`).
- `auto` is not used as a substitute: `auto` is a deduction placeholder and `Self` is a dependent type; the semantics of the two are orthogonal.

#### 3.15.7 Current limitations (updated 2026-11-19, build 251)

| Limitation | Description |
|---|---|
| ~~A generic struct/class does not support `const` members~~ | **Lifted 2026-11-17**: `const` fields in a generic type are ordinary per-instance fields (type-level constants are gone) |
| Not supported | Method-level generics, associated types, specialization, generic `extern`, generic type aliases. Type-argument inference covers free function calls only (3.15.3) |

Lifted since 0.0.72 (kept here so the old table's rows are not misread as current): a generic interface is
a **usable value type** (`IIterator<int>` as a local/field/return type, dispatch through the itable,
0.0.108); a generic struct/class **can** implement interfaces (`struct S<T> : I`, 0.0.119/0.0.120); a
generic interface **can** inherit (`interface IList<T> : ISeq<T>`); `Self[]`/`Self`-typed members work
as constraints.

#### 3.15.8 Why there is no "value type as an interface value"

In a language without borrow checking there is only one implementation path from a value type to an interface value: **boxing** (copying to the heap + reference counting). Its cost is
comparable to `new`ing a `class` object (one object header saved, one extra copy), yet it loses the explicit lifetime of a `class`, the `as` downcast,
and `weak`. Therefore this language explicitly does not provide:

- Runtime polymorphism → `class` (single inheritance + interfaces + vtable + RC);
- Compile-time capability checking → `T: I` (3.15.5) and `struct S : I` (U33), at zero runtime cost;
- Heterogeneous value collections → `variant` types (10.1), rather than interface boxing.

Two questions are often mixed up here; only the second one stays closed.

- **A generic interface as a value type** (`IIterator<int> x;`) **is supported** (0.0.108). The object behind the
  interface is a `class`, so the two-word value is exactly the existing interface representation and dispatch goes
  through the itable; each instantiation (`IIterator<int>` vs `IIterator<string>`) contributes its own slots, because
  slots are keyed by the concrete method signature. Nothing is boxed.
- **A value type as an interface value** (`Animal a = cat;` where `cat` is a `struct`) **stays closed**: it would have
  to copy the struct to the heap and reference-count it — that is boxing, and the language does not do it implicitly.
  `struct S : I` therefore gives a compile-time contract only, never an interface value.

`Animal a = cat;` (struct) is a compile error. If borrow checking is introduced in the future, a zero-cost borrowed
interface of the `ref I` form can be reconsidered (see backlog U33 "Final Decision").

#### 3.15.9 Predeclared contracts (`std.interfaces`)

The predeclared contracts (`IIterator<T>`, `IIterable<T>`, `IIndex<T>`, `IEqual`,
`IOrder`, `IAdd` … `INumber`) are ordinary `interface` declarations in `std.interfaces`, a
library unit loaded implicitly. The member tables — which signatures each contract requires and which
types satisfy it — are library content:
[docs/StdLibSPEC.md](file:///D:/dev/mu_lan/docs/StdLibSPEC.md) §6.2. The language rules,
unchanged:

- A type satisfies a contract **structurally** (3.15): providing the members with exactly the
  required signatures is enough; declaring it (`struct Money : IAdd`) checks the same shape at the
  definition site and is optional.
- The constraint checker recognises only `+ - * / %` and the comparison operators as built-in
  arithmetic/comparison operators, so there is deliberately no bitwise contract. A **subscript** is
  declarable and satisfiable (3.15.9), which is what lets generic container algorithms be written; it
  is not an arithmetic operator and does not participate in `INumber`.
  Because a subscript carries a capability (read or read-write) that a single operator expresses only
  through its return type, the library ships it as a **pair**: `IIndex<T>` declares
  `T operator[](Self c, isize i)` (read — satisfied by a value or a `ref` implementation) and
  `IIndexMut<T> : IIndex<T>` declares `ref T operator[](Self c, isize i)` (read-write —
  satisfied only by a `ref` implementation). A generic body that writes through an element — including
  `for ref x in c` — must be constrained on the mutable one.
- User value types satisfy these contracts through `operator` members or free `operator` functions in
  a visible unit; `class` equality stays identity, so a `class` does not satisfy `IEqual`.
- **Each comparison contract demands only the operator that the compiler can derive the rest from**:
  `IEqual` requires `==` (and `!=` is derived, 3.11), `IOrder : IEqual` requires `<` (and `>`, `<=`,
  `>=` are derived). A type that declares the derived ones directly still satisfies the contract, and
  its declarations win. `IOrder` promises a **total order** — the derived `<=` is `!(b < a)`.

### 3.16 Function types and lambdas (implemented in 0.0.84)

#### 3.16.1 Function type syntax

```c
type Cmp   = (const void*, const void*) => int;
type Thunk = () => void;
type Unary = (int) => int;
```

- Parameters carry **types only**, never names; `()` means "no parameters" (the `(void)` form is not used).
  The return type may be `void`.
- **A function type is a pointer value**: one word, `sizeof`/`alignof` equal to the pointer size, binary compatible
  with a C function pointer. There is no "function type vs function pointer" distinction (unlike C), and **no `*` is
  written**: `(int) => int*` does not mean "pointer to"; it means "returns `int*`".
  `*`, `[]` and `[N]` bind to the return type.
- `=>` has the **lowest precedence** and is **right-associative**: `(int) => (int) => int` is a function returning a
  function. A function-typed parameter is written directly; its parentheses are a parameter list, not grouping:
  `((int) => int, int) => void`.
- Function types are **not parenthesized for grouping**: an array of function values is written with an alias
  (`type F = (int) => int; F[] a;`), not `(F)[]`.
- `type` aliases are transparent: `Cmp` and `(const void*, const void*) => int` are the same type.
- Allowed positions: parameter lists, struct/class fields, generic arguments, `as`, `sizeof`, and the right-hand side
  of `type`.
- **Block- and unit-level declarations never begin with `(`, and a return type is never an anonymous function type.**
  Fields and parameters may use the anonymous form — those positions accept only types, so no expression can be
  confused with one.

  ```c
  (int) => int f = sq;        // error: use `auto f = ...`
  (int) => void make() { }    // error: name the type first
  ```

  Write `auto` for locals and a `type` alias everywhere else:

  ```c
  auto f = (int a) => a * a;              // f: (int) => int
  type Handler = (int) => void;
  Handler h = null;
  ```

#### 3.16.1a `delegate`: the two-word callable (implemented 0.0.109, capture pending)

```c
type Op = delegate (int, int) => int;
```

- **A `delegate` is `{code, env}`, two words** (`sizeof`/`alignof` = 16/8): the entry point plus an
  environment object. It is a **separate type**, not a fatter `fn` — the one-word, C-ABI-compatible
  promise of `fn` stays intact.
- **Calling convention**: `d(args...)` calls `code(env, args...)`, i.e. the environment is always the
  first hidden argument. C code can therefore be handed `{code, env}` as a pair of `void*` and call
  `ret (*)(void* env, ...)`.
- **Environment lifetime**: `env` is a reference-counted object (today always `null` — see below);
  copying a delegate retains it, destroying the value releases it, so delegates live in fields,
  arrays, parameters and return values like any other RC-carrying value.
- **Conversions**: `fn → delegate` is implicit (the value becomes `{thunk, null}`, where the thunk
  drops the unused `env` argument); **`delegate → fn` is an error** — the environment has nowhere to
  go. `null` converts to a delegate (the empty value).
- **Not allowed in `extern` signatures** (parameter or return): C has no ABI for a pair whose
  environment must be released; pass `.code`/`.env` explicitly as `void*` instead.
- **Position rules** follow function types: parameters and fields may spell the anonymous form,
  block-/unit-level declarations and return types need `auto` or a `type` alias.
- **Status (0.0.130)**: the type, the layout, the calling convention, the thunk, the RC plumbing and
  capturing are in place. A capture list `[x]` / `[ref x]` / `[weak x]` is **mandatory for every outer
  variable the body uses** (implicit capture is an error, so the copying cost is visible at the
  definition site):
  - `[x]` copies the value into a synthesized environment **class** (one field per capture); a
    reference-counted value (`String`, slice, `class` handle, nested `delegate`) is retained by the
    environment, so the delegate keeps it alive.
  - `[ref x]` stores the **address** of the variable, so writes from the body reach it — the
    out-parameter pattern. Only borrowable types qualify (scalars, enums, pointers, value structs
    without reference fields or a destructor), mirroring `ref` parameters; the caller owns the
    variable's lifetime, and a delegate that outlives it is UB like any other dangling raw pointer.
  - `[this]` captures the receiver of the enclosing method: a **class** contributes its handle (the
    environment retains it, so the object stays alive), a **struct** contributes a copy (value
    semantics — later changes to the original are invisible). `[ref this]` on a **struct** stores the
    object's address so writes reach it; on a class it is rejected (the handle already is a
    reference). The body keeps working as usual: `this` and implicit field access resolve through the
    environment.
  - `[weak x]` / `[weak this]` store an **uncounted** reference: the environment does not keep the
    object alive. This is how a `class` keeps a callback on itself without an RC cycle
    (`this.cb = [weak this] () => n;` — the object is destroyed, the destructor runs). Only `class`
    handles qualify: a `struct` `this` is a value copy (no cycle to break) and an `interface` value
    has no `weak` form yet. The weak registry clears the slot when the target is destroyed
    (3.8), so a call on a delegate whose weak capture is already dead **returns the zero value of
    the return type without running the body** — inside the body every weak capture is therefore
    live, and no null checks are needed there. The test is a snapshot: a concurrent destruction
    between the check and a later use of the capture is a data race, exactly like reading a `weak`
    field.
  - A capturing lambda's value is a `delegate`; a non-capturing one is still a plain `fn`.
  - **`for` bodies**: each iteration captures its own copy with `[x]` (the loop variable is a fresh
    local per iteration). `[ref x]` shares the one variable, so every delegate sees its final value —
    the classic loop-closure hazard, documented rather than special-cased.
  - **Empty value**: `d == null` / `d != null` compare the entry point, so a delegate built from a
    `null` literal is "empty" (calling it is UB).
  - **`para` regions** (3.17.2): a section may **call** a delegate captured by value before the
    region, but `[ref …]` and `[this]` inside a section are rejected — a section must not write
    enclosing locals.
  - **Immediately-invoked lambdas allocate nothing** (0.0.113): `([x] (…) => …)(args)` keeps its
    environment on the stack and destroys the captured RC fields right after the call.
  - A capture may only name a variable of the **immediately enclosing function**; a deeper one must be
    captured by the enclosing lambda first (`[y]` there, then `[y]` here), otherwise the inner lambda
    would silently read a fresh slot.
  - Statically rejected: capturing the same name twice, capturing a name that is not a local,
    `[ref ...]` of a reference-containing type, `[weak ...]` of anything but a `class` handle,
    `[weak ref ...]` (the two modifiers contradict each other), by-value capture of a struct with a
    destructor, and
    **returning a `[ref ...]` lambda** of frame-owned storage (the variable always dies with the
    call). `[ref]` of caller-owned storage — `this` of a struct method, a `ref` parameter, a
    `for ref` binding — may escape; its lifetime is then the caller's responsibility.

  Rationale: "declaration vs expression statement" stays a single-token decision (a declaration starts with a keyword
  or an identifier), and `(int) => int f` would read like a cast.
- `const` cannot qualify an **anonymous** function type: there is no pointee to protect, and a declaration cannot begin
  with `(` anyway. Name the type with `type` when a read-only variable is needed (`type F = (int) => int; const F f = ...`).

#### 3.16.2 Function values

- A free function name in value position **is** a function value of the matching type:

  ```c
  Cmp c = byInt;                       // ok
  qsort(&xs[0], n, sizeof(int), byInt);
  ```

  No `&` is written: `&byInt` is an error ("a function name is already a function value"). `&` is likewise rejected
  on a value of function type — pointer-to-function-pointer is not supported (and its spelling would be ambiguous).
- Supported operations: assignment, argument passing, return, `==`/`!=` between values of the **same** type,
  `null` as the zero value, and calling `f(args)` — through a local, a parameter, a field, an array element or a
  unit-level variable.
  Calling a `null` function value is undefined behavior (as with any null pointer).
- **No conversions between function types** (not even `(int) => int` → `(long) => long`; `as` between them is an
  error). A function value → `void*` is implicit; `void*` → function type requires `as`.
- An **overloaded** function name in value position needs a target type that selects exactly one overload;
  `Cmp c = over;` is fine, `auto f = over;` is an error (no target type).
- A **generic** function name cannot be used as a value: `max2<int>` would be ambiguous with comparison parsing.
  Wrap it: `auto f = (int a, int b) => max2<int>(a, b);`.
- Functions imported from another unit (`u.f`) and `extern` functions are function values too.
- A unit-level initializer may be **a function name** (an address constant, 4): `const Cmp C = byInt;` is legal and
  the variable can be called. A lambda cannot appear there (its body belongs to a function).
- **Methods are not values.** Using `obj.m` outside a call is an error: a method value would be `{ code, this }`
  (a closure), and `this` is an RC reference whose lifetime a raw pointer cannot express. Use a single-method
  `interface` for stateful callbacks and a free function for stateless ones.

#### 3.16.3 Lambdas

A lambda is an **anonymous function with no captures**:

```c
auto sq    = (int a) => a * a;
auto clamp = (int x) => {
    if (x < 0) return 0;
    if (x > 100) return 100;
    return x;
};
Handler h = (int e) => { print(e); };
```

- Form: `(T1 a, T2 b) => body`. The parameter list repeats the type syntax and adds names; every parameter spells its
  own `type name` and **parameter types are always written** (there is no target-type inference).
- `body` is an expression or a block:
  - `=> expr` is shorthand for `{ return expr; }`; when the type of `expr` is `void` it is shorthand for `{ expr; }`
    and the lambda returns `void`.
- **The return type is inferred and never written**:
  - no `return expr;` in the body → the lambda returns `void` (`return;` and falling off the end are allowed);
  - otherwise the return type is the type of the `return` expressions, and all of them must have **exactly the same
    type** (`null` adapts to the other side). Mixing `return;` with `return expr;` is an error.
  - a non-`void` lambda must return on every path (the check of 7 applies unchanged).
- The type of a lambda follows from the lambda alone, so `auto` works: `auto f = (int a) => a * a;` has type
  `(int) => int`. No expected-type propagation and no target typing are involved.
- **No captures**: the body may use its own parameters and locals and unit-level symbols. Referring to a local of an
  enclosing function, to `this`, or to a parameter of an enclosing lambda is a compile error
  (stateful callbacks: use a `class` implementing a single-method `interface`).
- A lambda has no name and therefore cannot call itself; use a named function for recursion.
- Each lambda expression denotes a **distinct** function (no deduplication). It is lowered to a hidden `private`
  function of the unit, and its address is the value; lambdas therefore allocate nothing and are C-ABI compatible.
- In phase one lambdas appear inside function bodies only (a unit-level initializer needs a named function).
- Calling through a function value is an ordinary indirect call. `throw` inside a lambda behaves like `throw` in a
  named function (the call site checks the pending slot, 3.14); throwing across a C frame is UB.
- Generics: function types may mention type parameters (`void sort<T>(T[] a, (T, T) => int cmp)`), and a lambda inside
  a generic function body is instantiated by monomorphization (3.15). Generic type aliases do not exist, which is why
  the anonymous function type is allowed in parameter position.

#### 3.16.4 Stateless vs stateful callbacks

| Need | Use |
|---|---|
| Stateless callback, C interop | function type / lambda |
| Stateful callback, listener, strategy object | `class` + single-method `interface` |
| C API taking a callback plus `void*` userdata | function type + explicit `void*` |

Capturing closures are a phase-one non-goal (10). If they are ever added they must be a **separate type**
(`delegate`, `{ code, env }`, two words), not a fatter `fn`: the one-word C-ABI promise must stay intact.
The type itself has landed in 0.0.109 (3.16.1a) — with a null environment until capture analysis does —
so the table above still holds for stateful callbacks today.

#### 3.16.5 Implementation status

Implemented in 0.0.84 for the whole path — lexer (`=>`), parser (`(`-started parameter-type list, lambda detection
by the matching `)` followed by `=>`), the `Func` type kind (canonical spelling `(int, int) => int`, mangling
`fn_int_int__int`, 8-byte layout), sema (function names as values, overload selection by target type, calls through a
value, lambda lowering and single-pass result-type inference), and codegen (`ptr` + an indirect call; each lambda is a
hidden `FuncDecl` emitted with the unit).

Not implemented: method values, generic function values (`max2<int>` as a value), and anonymous function
types in declaration positions (a unit-level variable, a local declaration and a return type must name the
type with `type` first). Capturing closures **are** implemented since 0.0.110 (single-field environments,
immediate-invocation stack environments; `[weak x]` captures since 0.0.130) — see 3.16.1a.

### 3.17 Concurrency: `para`, `thread`

> **Status: `para` and `para for` are implemented over every source form, including the cursor form
> (0.0.92–0.0.97), and run on the M:N task runtime since 0.0.98 (3.17.7); `thread` is not.**
> The runtime layer (scheduler, task stacks, pollers, channels) is a **library**; this section fixes
> only the language surface. The library contract is [docs/StdLibSPEC.md](file:///D:/dev/mu_lan/docs/StdLibSPEC.md) §6.4;
> the decision record and invariants are an internal record (`agents/stdlib/task-runtime.md`).

#### 3.17.1 Model

- Concurrency is **structured**: every concurrent region has a join point. There is **no detached task**
  and no fire-and-forget. Work that must outlive a call is a long-lived service task driven by a channel.
- Execution units are **tasks** scheduled onto a bounded set of **carrier** OS threads (M:N, stackful
  fibers). `para` lowers to runtime primitives that are themselves public library API; the syntax adds
  no scheduler semantics that the library cannot express.
- **No function coloring**: any function may be called from a task or from a plain thread. A blocking
  primitive parks the task when a task context exists and blocks the thread otherwise, so the same
  `std.net` / `std.file` code works in both worlds.
- **No implicit sharing of stack variables**: a region may read enclosing locals, but may not write
  them, take their address, or pass them by `ref`. Sharing is expressed through `class` objects,
  `Channel<T>`, or atomics.
- **Deferred join (a future) is a library facility**, not syntax: `TaskGroup` plus a one-slot
  `Channel<T>` already expresses "start now, join later". The language adds no `spawn` / `await`
  keyword, so no part of the language surface depends on a `Task` type.
  **Planned, not implemented yet**: neither `TaskGroup` nor `Channel<T>` exists in the library or in
  StdLibSPEC §6.4 at this point — the sentence describes the intended library shape, and the language
  guarantees only that no syntax will be added for it.
- Tasks are not bound to threads (a task may resume on another carrier). Code must not rely on thread
  identity, and a C library that keeps `thread_local` state across calls must run in a `thread` section.

#### 3.17.2 `para` — fork/join

```c
para {
    { a(); }
    { b(); }
}
```

- Each brace-delimited section is a task. All sections start concurrently and `para` joins them all
  before it completes.
- A section may declare its own locals, read enclosing locals, and call functions that themselves
  contain `para`. Nesting is allowed, but the task count multiplies with the nesting width and depth,
  so keep the depth bounded.
- **Compile errors inside a section**: writing an enclosing local (including `++`/`--` and compound
  assignment), taking the address of an enclosing local, passing one as a `ref` argument, `return`, and
  `break`/`continue` that would leave the section. Loops declared inside the section are unaffected.
- Exceptions are classified by **where they are raised**:
  - An exception raised while `para for` is **producing** an item — a throwing `next()` /
    `operator[]` or a throwing source expression (3.17.3) — is an ordinary exception of the region:
    the tasks already started are joined before the enclosing scopes are left, and the exception
    propagates in the producing scope.
  - An exception that escapes a **section body** crosses the task boundary and **aborts the process**
    (the B2 thread-boundary rule; 3.17.7): it is not caught by any enclosing `try`, and the sections
    that already started may run to completion first.
- A section is not a function: it has no name, cannot be stored, and cannot escape.

#### 3.17.3 `para for` — concurrent iteration

```c
para for i in 0..n { out[i] = f(in[i]); }
para for ref v in a { v = norm(v); }
para for req in server.requests() { handle(req); }
```

- **One task per item**, started as the item is produced; the region joins when the source ends. For a
  finite source that means "all items produced"; for a cursor source (6.4.2) it may run indefinitely
  (an accept loop is written this way).
- The producer is a **single puller**: the source expression is evaluated once, and `next()` is called
  between item starts, so a blocking `next()` is where backpressure and the end condition live. The
  source, not the syntax, decides when the iteration ends.
- Sources: the built-in sequences of 6.4.1 (ranges, managed sequences `T[]` / `String` and containers that provide `length()` + `operator[]`) and user types
  satisfying the **cursor form** of 6.4.2 (`iterator()`, `next() -> bool`, `current() -> T`). A
  blocking `next()` is where backpressure and the end condition live; the source, not the syntax,
  decides when the iteration ends.
- Iteration order is unspecified. `for ref` follows 6.4.1 (distinct elements only).
- The body obeys the same restrictions as a `para` section; in addition `break`, `continue` and
  `return` are errors — a region ends when its source ends, or by cancellation.
- One task per item is the semantics. A large finite source is split by the user (an explicit index
  loop over smaller ranges) to reduce per-item overhead; nothing in the language does it automatically,
  and it does not change the semantics.

#### 3.17.4 `thread` — compute pool

```c
para {
    { conn.flush(); }
    thread { solve(matrix); }
}
para thread for i in 0..n { heavy(i); }
```

- `thread` is a **contextual keyword** in a `para` header or body. It selects the **bounded compute
  pool** instead of the task scheduler: the section/item runs on a dedicated OS thread, may block, and
  uses the OS thread stack instead of a small task stack.
- The pool size defaults to the number of cores and is a **runtime setting**, not a per-region
  parameter. A tighter per-region limit is expressed with a semaphore, not with syntax.
- It declares intent, it does not check it: a `thread` block that only waits wastes a pool thread, and
  a default section that burns CPU cannot be preempted.

#### 3.17.5 Cancellation and preemption

- Cancellation is **cooperative** and inherited: a cancelled region stops producing, asks its tasks to
  cancel, joins them, and propagates. Park points (IO, channel, sleep, join) are cancellation points;
  long CPU loops must call `task.yield()` / `task.cancelled()` (library).
- There is **no preemption** and **no timeout syntax**. A deadline belongs to the IO operation
  (`conn.read(buf, ms)`), which is the only place where cancellation can be enforced; `para`
  deliberately provides no `timeout` clause, because it could not honour it.

#### 3.17.6 Stacks and threads

- A task runs on a **fixed-size stack**; it does not grow. Deep recursion and large locals belong in a
  `thread` section, which uses the OS stack (the default sizes are runtime settings, see
  [docs/StdLibSPEC.md](file:///D:/dev/mu_lan/docs/StdLibSPEC.md) §6.4).
- Exceeding a task stack overflows it and aborts the process; this is not recoverable.
- Data races between tasks or between threads are undefined behaviour (8.2). The compile-time rule
  against writing enclosing locals is a narrow, syntactic guard, not a race checker.

#### 3.17.7 Implementation status

`para` (0.0.92) and `para for` over every source form (0.0.93–0.0.97) run on the **M:N task runtime**
(0.0.98). The language-level `thread` **statement/section** is not implemented; the `std.thread` *library*
(real OS threads, `thread_start` / `thread_join`) **is** implemented and specified in
[StdLibSPEC §6.5](StdLibSPEC.md). What the language guarantees, and what it does not:

- A task has its own fixed-size stack (3.17.6): deep recursion and large locals need a `thread`
  section or a runtime-configured larger stack.
- A started task always runs to completion (no cancellation yet); an exception that escapes a task
  body **aborts the process** (the B2 thread-boundary rule), while exceptions caught inside the task
  behave normally.
- **The same rule covers `std.thread` bodies** (decided 2026-11-18): the pending-exception slot is
  thread-local, so an exception that escaped a `thread_start` body would vanish with the thread and the
  joiner would never learn about it. The library aborts instead (`thread_start` is not a `try`), which
  makes "a thread body that throws" terminate the process exactly like a `para` task body.
- `this` and implicit field access inside a section are **compile errors** in v1 (pass the object as
  an explicit argument). Writing, taking the address of, or passing by `ref` an enclosing local are
  compile errors (3.17.2).
- `para for` has **one task per item; chunking is never automatic** — a user splits the work with an
  explicit index loop over smaller ranges. The producer is a single puller (3.17.3).
- A refused task start is an ordinary exception (`TaskLimitError`): the region releases the failed
  item, joins the tasks that already started, and propagates.
- An exception raised while **producing** an item is an ordinary exception of the region; the tasks
  that already started are joined before the enclosing scopes are left, so no task ever outlives the
  producer's frame.
- Carriers live for the whole process and are never shut down; there are no park points for IO yet,
  so the poller/channel/server layers (P4) and the compute pool are still missing.

Scheduler internals — carriers and handoff, fiber pools, task limits, exception-state migration,
`errno` behaviour — are library implementation detail, not language semantics (internal record:
`agents/stdlib/task.md`).

The section body is a hidden function that receives a frame of pointers to the caller's captured
locals; the caller is suspended at the join, so those slots outlive every section. An item body is the
same hidden function with a per-item context `{ frame, item }` (the element address for `for ref`).
The lowering calls `std.task.muc_para_start` / `muc_para_join`, so the language surface does not depend
on the execution engine.

### 3.18 Interface extension (`extend`, decided 2026-11-20, implemented in build 261)

```mu
public extend I with J;     // J : I
```

- `J` must derive from `I` (`interface J : I`). It may add **default methods only**: every abstract
  member of `J` must already be declared by `I`, and an extension cannot add fields (interfaces have
  none) or access fields.
- Effect: every type that **names `I` in its `:` list** (directly, or through a base class or a parent
  interface) gains `J`'s default methods. An extension is active where its declaring unit is used
  (`use`), not everywhere the interface is visible.
- Visibility: `public` / `private`; it is not transitive. A unit that does not `use` the declaring
  unit cannot call a method that only the extension provides: the call is a compile error naming the
  missing `use`.
- A type's own method (or an interface default it already has) wins over an extension. Two extensions
  providing the same signature for the same type are a **declaration** error naming both units.
- **`extend` does not create a subtype relation**: an implementor of `I` does not satisfy a `T: J`
  constraint and cannot be used as a `J` interface value because of an extension.
- The extension is materialized per concrete type during semantic analysis (the same machinery as
  interface defaults) and marked with the extension's unit, so call sites can enforce the visibility
  rule above; code generation is unchanged.
- The concrete-type form `extend S with I;` (declare that an already declared type satisfies an
  interface) is **planned for a later phase** and is not part of this rule.

### 3.19 Field method promotion (`promote`, decided 2026-11-20, implemented in build 260)

```mu
class Box<T> {
    private T _value;
    public Box(move T value) { this._value = move value; }

    promote _value;             // every public method of the field
    promote _value(f, g);       // optional whitelist form
}
```

- `promote` is a contextual keyword at the start of a member declaration; the field must be a member
  of the enclosing type and must have a `struct`, `class` or `interface` type (scalar, enum and
  variant fields are rejected).
- **Only public methods are promoted.** Fields, constructors, destructors, operators (including
  `operator[]`) and function values are never promoted. The field itself may be `private`: promotion
  grants a method-call surface, not access to the field.
- Resolution order for `obj.m(...)`: methods of the type itself, then base-class methods, then
  interface methods (including materialized defaults), then promoted fields in declaration order.
  A real method always wins. If two promoted fields provide the same method name, the enclosing type
  declaration is an error (not the call site).
- A promoted method does **not** participate in `override` and does **not** satisfy an interface; an
  interface must be implemented by the type's own methods.
- Promotion is **one level**: a promoted field's own promoted fields are not followed.
- `this` inside the promoted method is the field object, not the wrapper. Dynamic dispatch follows
  the field type's normal rules (a class field dispatches through its handle; a struct field is
  static).
- `const` rules are unchanged: a `const` receiver may call only the field's `const` methods.
- Inside a generic type the promoted set is resolved per instantiation (the public methods of the
  instantiated field type); diagnostics name the instantiated type.
- The promoted method must be visible from the caller's unit (the field type and the method must be
  visible there); promotion adds no exported symbol.

## 4. Declarations and scopes

- Declaration syntax: `Type name1 [= init1], name2 [= init2];`
  - Initialization forms:
    - No initializer: `int x;` (reading an uninitialized local value is UB; unit-level variables are zero-initialized by default).
      A class or interface variable is **not** allowed to omit the initializer (decided 2026-11-19): the type *is* the handle
      (3.8), so `Dog d;` would read like a stack construction while building nothing. Write `= null`, `= new Dog(...)`,
      or initialize from an existing reference.
    - Expression initialization: `int x = 1;`, `Point p = Point(1, 2);`, `Point q = p;`
    - Brace aggregate initialization: `Point p = {1, 2};`, `Point p = {x = 1, y = 2};`, `int[5] a = {1, 2, 3};`
    - Empty-brace zero initialization (aggregate types only): `Point p = {};`, `int[5] a = {};`, `int[] s = {};`, `string s = {};`
    - Scalars use their own zero constants: `int n = 0;`, `float f = 0.0;`, `bool b = false;`, `char c = '\0'`, pointers/class references `= null`
  - Do not write `T x(...)` in a declaration; the construction expression `T(...)` is only used in non-declaration positions or as the right-hand side of `=`.
  - `Point p = Point(1, 2)` is equivalent to `Point p = {1, 2}`; the prvalue on the right-hand side is constructed directly into `p`, with no extra copy.
  - Brace initialization appears only in the declaration-initializer position; it is not a general expression. In non-declaration positions always use `T(...)`.
  - Struct braces: every field must be given, either positional or named; the two cannot be mixed. An empty `{}` means all zeros.
  - **Braces may nest**: fields/elements may themselves be initialized with braces.
    - `struct Box { int[3] v; }` → `Box b = {{1, 2, 3}};`
    - `Pt[2] ps = {{4, 5}, {6, 7}};` (the array elements are structs)
    - The value of a named field may also be braces: `Rec r = {p = {1, 2}, name = "hi"};`
    - A fixed-size `char[N]` field can be initialized directly from a `string` literal: `{name = "hi"}` (padded with NUL and zeros).
  - Fixed-size array braces: elements are given by position; missing elements are padded with zeros; too many elements is an error; multidimensional arrays use nested braces.
  - **Range fill** (0.0.62): a positional list may contain `a..b`, which expands to `a, a+1, …, b-1` (half-open).
    The endpoints must be **compile-time integer constants**; `a >= b` expands to 0 elements; element types are limited to integer/`char`/`byte`.
    Example: `int[10] a = {1, 2, 5..9, 10};` (1,2,5,6,7,8,10,0,0,0), `Box b = {{0..3}};`.
    `..=` and `step` are **not supported** inside braces (to avoid diverging from the semantics of for-in); slice initialization does not support range fill.
    Dynamic array braces (one-dimensional only): a new array is allocated with the given element count.
  - Empty braces `{}` are a declaration-only "zero value" initialization, used only for aggregate types (struct, fixed-size array, slice, `string`): struct → fields recursively zeroed, fixed-size array → all zeros, slice → empty (null handle), `string` → empty string. Scalars (numeric/bool/char/pointer/class reference) cannot use `{}`; they use `0`/`0.0`/`false`/`'\0'`/`null` respectively. Aggregate types no longer support `= 0` (the old form is an error that suggests switching to `{}`); `{}` does not participate in ordinary expression conversion.
  - Multiple declarators may each use a different initialization form: `Point a = {1, 2}, b = {}, c;`, evaluated from left to right.
  - A type may be followed by multiple variable names, all sharing the preceding type (variables / fields / unit-level declarations only; function parameters do not follow this rule and must spell out their type one by one, see 7).
  - Pointer/array dimensions belong to the type and apply to every variable in the same declaration:
    ```c
    int* p, q;        // both p and q are int*
    int[5] a, b;      // both a and b are int[5]
    ```
    This differs from C's `int* p, q;` (where q is an int) and must be emphasized in the documentation.
- Parsing conventions: type names and value names must not conflict within the same scope; a type must be declared before use; a function definition uses `T name(params) { ... }`, external functions use `extern`, and no bare function prototypes are provided; nested functions are not allowed inside a function body; unit-level variables are declared directly (default `private`; `extern` denotes an external symbol). A declaration is always `Type name` followed by `=` or `;`, so it cannot be confused with a function declaration, completely avoiding C++'s most vexing parse.
- Block scope; a variable is visible after its declaration point.
- Duplicate declarations in the same scope are not allowed; **an inner scope may shadow an outer variable of the same name** (implemented).
- `const` applies to all variables in the whole declaration: `const int a = 1, b = 2;`.
- `auto` drops **top-level** const: `const auto x = expr;` keeps an explicit const, while `auto y = const_x;`
  deduces the unqualified type (a copy of a const value is mutable, as in C++). Low-level const is preserved
  (`auto p = const_ptr;` deduces `const T*`).
- Local type inference: `auto name = expr;`
  - Local variables only; an initializer is required; cannot be used for parameters, return values, fields, or unit-level variables.
  - Multiple declarators in the same declaration must deduce the same type: `auto a = 1, b = 2;` is legal; `auto a = 1, b = 2.0;` is an error.
  - Cannot be deduced from `null`, `{...}`, or a void expression; `auto x = 0` deduces to `int` (not an aggregate zero initialization).
  - `const auto name = expr;` is **implemented** (immutable + deduced); unit-level variables cannot use `auto` (an explicit type is required).
  - `auto` only takes the expression type directly; it does not perform Hindley-Milner unification.
- Except for `auto`, parameters, return values, fields, and unit-level variables must all have an explicit type.
- Unit-level variables are declared directly (`int g = 1;`) and are `private` by default (visible within the unit); use `public` across units; `extern` denotes an external symbol.
- **Initializers of unit-level variables** (decided 2026-11-17): an initializer that the constant evaluator can
  fold (3.7.2) is emitted as a compile-time constant; any other initializer is evaluated at run time by the
  unit initializer (9.1). The same rule applies to `const` and non-`const` globals.
  - Compile-time constants: literals, unary `-`/`+`/`not`, `as`, arithmetic/bitwise/comparison/`and`/`or`,
    references to **already-declared** constants (unit-level `const`, enum members) and `sizeof(T)` /
    `alignof(T)`; `const int B = A + 1;`, `int g = 1 + 2;` are compile-time constants. **Forward references
    are not allowed** (`const int A = B; const int B = 1;` is an error). Division by zero and a reference to
    a not-yet-evaluated constant are errors.
  - Runtime-initialized globals: `const string S = "hi";`, `const int x = f();`, and non-`const` globals with
    a call initializer. They are **`const` objects but not compile-time constants**: they cannot be used in
    `#if`, array lengths, `step`, `align(N)`, or `match` ranges.
  - Aggregate constants are written as literals or aggregate initialization
    (`const int[] G = {10, 20, 30};`).
- **Local `const`** may be initialized with a runtime expression (`const int x = f();` is legal); it simply cannot be assigned again after initialization.
- A `const` inside a type body is a **`const` field** (3.7): it must be initialized by every constructor
  (member initializer list or a default member initializer), occupies instance storage, and is read-only
  afterwards. There are no type-level constants.
- `static` has been removed (0.0.32): unit-level variables naturally have static storage duration; `static` local variables inside functions and `static` functions are not supported.
- Parameters are mutable local bindings (phase one, C semantics); whether they become immutable by default is pending.
- **Parameters are owning local variables** (since 0.0.47, consistent with 3.8): parameter values are retained on entry and released on exit;
  this applies to class/interface/`string`/slice parameters and to struct parameters that contain reference fields. A parameter may be freely rebound inside the function body,
  without affecting the caller; assigning an owned value (`new`/a call result) to a parameter does not leak. Arguments at the call site are still passed by value.
- `ref T` parameters (**0.0.62; scalars supported since 0.0.63**): borrow the caller's variable by reference; the callee may write (a whole-value assignment writes the caller's variable, for every value type), cannot rebind (there are no references to rebind) or store;
  every non-union value type may be borrowed — a borrow is scoped (it cannot be stored or rebound), so the
  callee reaches the object only through the language rules and the reference counting stays exact, even when
  the value owns a resource or holds reference-counted fields. `&` remains refused for such a type, because a
  raw pointer outlives the borrow. A `[ref x]` **capture**, which does escape the frame, keeps the narrower
  rule (scalars, enums, pointers, plain value structs); `extern` takes no `ref`; a **method** (struct or
  class) may take `ref` / `const ref` parameters — the borrow is part of its signature, the callee
  receives the caller's address, and the argument must be an addressable place of exactly the declared
  type (build 270). An **interface** member cannot: the itable slot carries the value signature only.
  A **constructor** takes a `ref` parameter only as the copy constructor `T(const ref T other)`.
- `ref T` returns (**0.0.63**): the borrow must originate from a `ref` parameter, `this`, global/static storage, or pointer·slice storage; the result cannot be stored,
  it can only be written through on the spot or passed as a borrow argument (see 3.8).
- `const ref T p` is a **read-only borrow**: it cannot be rebound, and fields/elements cannot be written through the borrow; the argument must be addressable (temporaries are not allowed).
- **`ref` is a binding mode, not a storable type**: it does not introduce `T&`; a borrow cannot be the type of a variable/field/element/array element
  (no `ref T x;`, no `T&[]`), sharing the "not storable" rule with `for ref` (see 3.8).
- **`move` is a binding mode too** (C++ `&&`; decided 2026-11-17): `move T` is a non-const rvalue reference.
  `const` and `move` cannot qualify the same parameter (`const move T` / `move const T` are errors), and
  `move` applied to a `const` object is an error. It appears in a move constructor (`T(move T other)`) and in
  a transfer position (`S b = move a;`, `b = move a`, `f(move x)`, `return move x;`). A `const` object can be
  copied but never moved.

### 4.5 Type aliases (`type`, 0.0.62)

```mu
type int32 = int;
type intArray = int[];
type Callback = bool;

type P = Point;
type PP = P;          // alias of an alias
```

- Form: `type NAME = <type>;`; **each declaration defines exactly one alias** (`type A = int, B = int;` is a compile error).
- `type` is a **contextual keyword**: only `type Ident =` at the start of a declaration is parsed as an alias; everywhere else it remains an ordinary identifier
  (protecting the `type` field in C interop structs).
- May only be written at **unit level** (an error with a hint inside struct/class/union bodies).
- Visibility follows `public`: visible within the unit by default; `public type` works across units (referenced by type name after `use`).
- An alias is **transparent** and does not create a new type: it is fully equivalent to the underlying type (`as`, overloads, `extern` signatures, `sizeof`/`alignof`
  all expand to the underlying type); it is not a newtype.
- A suffix applies to the alias as a whole: after `type A = int[];`, writing `A` means `int[]`; `const`/`weak` may modify the alias
  (checked against the underlying type after expansion).
- Forward references and arbitrary declaration order are supported: `type A = B; type B = int;` is legal; a cyclic dependency
  (`type A = B; type B = A;`) is a compile error; an alias with the same name as another declaration in the same unit is a conflict.
- Diagnostics prefer to show the alias name; `auto` deduces the **underlying type**.
- **`type` is alias-only** (decided 2026-11-20): a right-hand side that lists alternatives
  (`type V = A | B;`) is a compile error pointing at `variant` (10.1.1), and an interface extension
  (`type T += I;`) is a compile error pointing at `extend` (3.18). A concrete-type extension
  (`extend T with I;`) is planned, not implemented; the interface form `extend I with J;` is decided.

---

## 5. Expressions and operators

### 5.1 Precedence (high → low, proposed)

| Level | Operators | Associativity |
|---|---|---|
| 1 | `()` `[]` `.` `->` `as` | left |
| 2 | `-` `+` `&` `*` `~` `sizeof` `alignof` | unary (prefix) |
| 3 | `*` `/` `%` | left |
| 4 | `+` `-` | left |
| 5 | `<<` `>>` | left |
| 6 | `&` (bitwise and) | left |
| 7 | `^` (bitwise xor) | left |
| 8 | `\|` (bitwise or) | left |
| 9 | `<` `<=` `>` `>=` | left (not chainable) |
| 10 | `==` `!=` | left |
| 11 | `not` | unary (prefix) |
| 12 | `and` | left |
| 13 | `or` | left |
| 14 | `?:` | right (**settled, 0.0.46**: kept, see 5.2) |
| 15 | assignment `= += -= *= /= %= &= |= ^= <<= >>=` | statement, not an expression |

- Note: bitwise operators bind **tighter** than comparison (as in Rust, fixing C's `a & b == 0` trap).
- User-overloaded operators follow the precedence in this table; the operator semantics of built-in types are unchanged.
- Bitwise operators: binary `&`, `^`, `|`; unary `~` is **bitwise complement** (as in C). Logical negation is the keyword `not`.
  - `~` only accepts integer types (or a user type's `operator~` overload); using `~` on `bool` is a compile error (use `not`).
  - `!` is no longer used: `!x` is an error that suggests "use `not` for logical negation and `~` for bitwise complement".
- **`>>` follows the signedness of the operand** (the shift happens at the operand's declared bit width): a signed type sign-extends, an unsigned type shifts in zeros
  (`int -8 >> 1` = `-4`, `uint 0x80000000 >> 4` = `0x08000000`). This matches C and differs from the 0.0.1–0.0.128 semantics, where `>>` was always arithmetic
  (`uint 0x80000000 >> 4` came out as `0xF8000000`) — changed in 0.0.129 with issue #17.
  Compile-time constant evaluation and runtime code generation must stay consistent; both places changed together.
  `<<` is a bitwise left shift (overflow wraps in two's complement); a shift count that is negative or >= the bit width is undefined behavior (8.2), checked by `--bounds`.
- `%` is also legal for floating point (LLVM `frem`, synonymous with C `fmod`: the sign of the result follows the dividend); integer `%` is truncated remainder (the sign follows the dividend).
- **A float-to-integer `as` truncates toward zero and follows the target's signedness** (so
  `200.0 as byte` is 200). A value outside the target's range (or NaN) is UB at run time — reported by
  the `--bounds` check, rejected at compile time when the value is a constant.
- **No exponentiation operator is provided** (settled): `^` is reserved for bitwise xor, avoiding a clash with C users' intuition (`a ^ b` silently becoming exponentiation would produce silent bugs).
  - Exponentiation is provided through functions: floating point uses libc `pow` (`extern double pow(double, double);`); integer powers are implemented by user/library functions (such as `powi`).
  - `**` is explicitly excluded: `a * *b` (multiplying by a dereferenced value) would become `a ** b` under longest-match, a silent semantic error that cannot be fixed by whitespace rules.
- `not` is a **low-precedence** prefix unary operator, binding only tighter than `and`/`or` and looser than comparison (Python style):
  - `not a == b` parses as `not (a == b)`
  - `not a and b` parses as `(not a) and b`
  - `not a or not b` parses as `(not a) or (not b)`
  - To negate an entire `and`/`or` expression, use explicit parentheses: `not (a and b)`
- `and`/`or` short-circuit; `not` operates on `bool`.
- Comparison returns `bool` and cannot be mixed with integers.
- Assignment is a statement and cannot appear inside an expression (avoiding `if (x = y)`).
- `++`/`--` are only allowed as **standalone statements** (prefix or postfix), produce no value, and cannot appear inside expressions; the semantics are equivalent to `x += 1` / `x -= 1`.
  - Therefore `x = i++`, `f(i++)`, and `*(p++)` are all errors (even inside parentheses); `*p++` parses as `(*p)++`, i.e. incrementing the dereferenced result.
- No comma operator is provided.
- `sizeof(T)` / `alignof(T)` operate on types; `sizeof(expr)` is pending.
- `length()` is the accessor for managed sequences.
- Built-in functions: `alloc(bytes: isize) -> void*`, `realloc(p: void*, bytes: isize) -> void*`, `free(p: void*)`.
- Struct construction expression: `Point(1, 2)`, `Point(x = 1, y = 2)`, `Point()` (zero value); brace initialization is not an expression.
- `new T(...)` is an expression (`T` must be a class) and returns a class reference; there is no corresponding release statement (`del` has been removed; just set it to null).
- `0` in an expression is an `int`; zero-value initialization (aggregate `{}` / scalar constants) is only used in the declaration-initializer position. For a struct's zero value in an expression/assignment use `Point()`; you cannot write `p = {}`, `p = 0`, or `f(0)`.
- A string literal has type `const char*` (as in C); `String` is built from it through the implicit
  `String(const char*)` constructor and goes back with `s.cstr()`; `"abc"[0]` is legal.

### 5.2 Conditional expression `?:` (implemented in 0.0.46)

```mu
int m = a > b ? a : b;
string s = empty ? "none" : label;
Cat c = pick ? first : second;
```

- Form `cond ? a : b`; `cond` must be `bool` (consistent with `if`/`while`; there is no integer truthiness).
- Precedence is lower than `or` and higher than assignment (assignment is already a statement); it is **right-associative**:
  `a ? b : c ? d : e` is equivalent to `a ? b : (c ? d : e)`.
- **Only the selected branch is evaluated**; the other branch is not executed at all (the same nature as `and`/`or` short-circuiting).
- The result type is determined in the following order:
  1. Both sides have the same type → that type;
  2. Otherwise take the **implicit widening** direction (`int → long`, `T* → const T*`, `class → base class/interface`,
     `T[N] → T[]`, etc.; see 3.6);
  3. A `null` literal adapts to the pointer / class / interface / dynamic array on the other side;
  4. If none applies → compile error, requiring an explicit `as` conversion on one side.
- Literals adapt to the context: in `cond ? 1 : 2.5` both sides are `double`; in `cond ? 'a' : b` (where `b` is `byte`/`char`) they adapt as 8-bit integers.
- When both sides are `void` the result is `void`, usable only as an expression statement (`cond ? f() : g();`).
- The result is produced with **value** semantics (prvalue): a borrowed branch is built into the result exactly
  as `T x = <branch>` would build it — a type with a user copy constructor is copied **through that constructor
  once** (3.4), every other struct/string/fixed-size array is retained field by field;
  class/interface yields a new strong reference (a borrowed branch is +1, a `new`/call result transfers ownership directly);
  a dynamic array transfers/shares the handle (consistent with the allocation semantics of `T[N] → T[]`).
  Taking the result over (declaration initializer or assignment) copies it no further.
- The result is not an lvalue: `(cond ? a : b) = x` and `&(cond ? a : b)` are both compile errors.
- It does not participate in compile-time constant evaluation (consistent with `and`/`or`): `?:` cannot appear in a unit-level `const` initializer.
- GNU's `a ?: b` shorthand is not provided; both branches must be written.

---

## 6. Statements and control flow

### 6.1 Blocks and expression statements

- `{ ... }` blocks; empty blocks are legal; **a bare `;` empty statement is forbidden** (implemented; it is an error).
- Expression statements: function calls, assignment, compound assignment, `x++` / `x--` (increment/decrement produce no value and can only stand alone as a statement).

### 6.2 if / else

```c
if cond {
    ...
} else if cond2 {
    ...
} else {
    ...
}
```

- `cond` must be `bool`; there is no integer truthiness.
- Braces are mandatory.
- Parentheses around the condition and loop header are **optional** and omitted in canonical style; the C-style `if (cond) { ... }` is still legal (`(cond)` is just a parenthesized expression).
- Parentheses can still be used for grouping: `if (a or b) and c { ... }`.
- Struct construction uses `Point(...)`, so the ambiguity between `Type { ... }` in a condition and a block's opening `{` no longer arises.
- `elif` is syntactic sugar for `else if` (**implemented**; the two are equivalent).

### 6.3 while

```c
while cond { ... }
while true { ... }   // infinite loop, exited via break
```

- `continue` jumps to the condition check; `break` exits.
- The C-style `while (cond) { ... }` is still legal; canonical style omits the parentheses.

### 6.3.1 C-style for

```c
for (int i = 0; i < n; i++) { ... }
for (; i < n; ) { ... }          // empty init and step
for (int a = 1, b = n; a < b; a++) { ... }
for (;;) { ... }                  // infinite loop, exited via break / return
```

- **Implemented** (decided 2026-11-19; this supersedes the earlier "no C-style `for`" decision). The head is
  `( init ; cond ; step )` and **every clause is optional**: no init (`for (; cond; step)`), no condition
  (always true, `for (;;)`) and no step (`for (init; cond; )`) are all legal.
- `init` is either a declaration (one or more declarators, `const`/`auto` included) or exactly one
  expression statement (assignment, `++`/`--`, call). It is parsed like a statement, so it ends with `;`.
  A comma expression is not part of the language, so `for (a = 1, b = 1; ...)` is a syntax error — declare
  the variables instead (`for (int a = 1, b = 1; ...)`).
- `cond` must be `bool` when present (6.1). `step` is one expression statement; `++`/`--` are allowed there
  as statement forms (`i++`, `++i`), like anywhere else.
- **`continue` runs the step clause** (C semantics), not just the condition: `for (i = 0; i < n; i++) {
  if (skip) continue; ... }` always advances. `break` leaves the loop. Both bind to the innermost loop.
- `init`, `cond`, `step` and the body share one scope: a variable declared in `init` is visible in the
  condition, the step and the body, and is destroyed when the loop exits.
- A `for (;;)` (or `while true`) whose body has no `break` leaving **that** loop never falls out, so a
  non-void function does not need a trailing `return` after it. A `break` inside a nested loop
  belongs to that loop, not to the outer one; `match` has no `break` of its own.
- The parenthesized form is required for this shape; `for x in seq` (no parentheses) stays for-in. The two
  are told apart by the head: a C-style head starts with `;`, a declaration, or an expression, while a
  for-in head starts with `[const] [ref] name [`,` ...] in`.

`do-while` remains **not** part of the language (SPEC §0 table); write `while true { ... }` with a `break`.

### 6.4 for-in

```c
for x in seq { ... }            // x is the value binding for each iteration (copied by default)
for ref x in seq { ... }        // x is a mutable reference to the element
for const ref x in seq { ... }  // x is a read-only reference (usable on a `const` sequence, decided 2026-11-17)
for i, x in seq { ... }         // with index/ordinal (see 6.4.4)
```

- The parenthesized forms `for (x in seq)` / `for (i, x in seq)` / `for (ref x in seq)` are also accepted; canonical style omits the parentheses.
- The **C-style** head of 6.3.1 always needs its parentheses: `for i = 0; i < 3; i++` without them is
  an error (a bare `for` is the for-in form), and the diagnostic names both shapes.
- Built-in sequences: half-open range `a..b` and managed sequences (`T[]` and library containers). **Static arrays `T[N]` are C storage and are not iterable** (use an index loop). A `String` is iterated through the
  index form of 6.4.1 (`length()` + the library's member `operator[]`, byte by byte) — the compiler has no
  built-in knowledge of it.
- **`for-in` is not supported for multidimensional arrays** (both `T[m,n,...]` and `T[,]` count): use an index loop; for a static multidimensional array you can take a row first and then iterate.
- User types: lowered according to the **iteration forms** of 6.4.1–6.4.3 (**the index form is implemented; the cursor form is not implemented yet**);
  built-in sequences do not participate in form matching.
- `x` is scoped to the loop body only; immutable and bound by value by default; `x` is an **owning local variable** (0.0.47 semantics), so when the element is a
  class/interface it is retained on entry and released on exit of each round; the `ref` form can write to elements.
- **`for const ref x in seq`** (decided 2026-11-17) is read-only iteration: `x` is a `const ref` borrow, it can
  be read but not written, and it is the only `for ref` form accepted on a `const` sequence (`const T[]`,
  `const T[N]`, or a const container). `for x in` always copies, so a copy of a const element is mutable.
- `seq` is evaluated only once; its length is fixed at the start; elements must not be added or removed during iteration (UB or a runtime error).
  - A simple lvalue (a variable, a member access chain) is iterated directly; other expressions are evaluated by the compiler first, stored, and then iterated.
  - The sequence of `for ref` / `for const ref` must be a variable or member access (it must be addressable); `for ref v in arr[f()]` is an error.
- `continue` → the next iteration; `break` → exit.
- An empty sequence iterates zero times.
- Ranges: half-open `a..b` by default (includes `a`, excludes `b`); `..=` includes the upper bound; an optional `step n` (must be a non-zero compile-time constant).
  - Positive step: zero iterations when `a >= b` (or `a > b` with `..=`); a negative step automatically counts down, with zero iterations when `a <= b`.
  - Example: `for i in 0..10 step 2`, `for i in 10..=0 step -1`; `step 0` is a compile error.
  - In the index form `for i, x in a..b`, `i` is the **iteration ordinal** (starting at 0, independent of the step).
- Labeled `break label` is pending.

#### 6.4.1 Index form (random access, preferred; implemented)

```c
class Vec {
    isize n;
    int* data;
    isize length() { return n; }
}
int operator[](Vec v, isize i) { return v.data[i]; }   // free-function operator, see 3.11
```

- Conditions (all must hold):
  - a method `length() -> isize`;
  - `operator[](isize) -> T`, a **member operator** on the container (3.11 — the free-function form was
    removed in build 161).
- Signatures are matched **exactly** on `isize`: `operator[](C, int)` does not qualify (no narrowing conversion); on error the actual signature is reported.
- Lowering: `isize n = c.length();` (evaluated only once) → `for (isize i = 0; i < n; ++i) { T x = c[i]; ... }`.
- The element type is the return type of `operator[]` (if it returns `ref T`, the element type is `T`); **proxy/reference wrappers are not recognized**
  (a `vector<bool>`-style proxy design is not supported).
- `length()` must equal the number of valid indices of `[]`; a mismatch is a user error (debug bounds checks catch it).
- In `for i, x`, `i` is the real `isize` index.
- `for ref` requires `operator[]` to return `ref T` (the element is then addressable); when `[]` returns a value, use `for i, x` + `c[i] = ...`.
  `for const ref` accepts an `operator[]` returning `ref T` or `const ref T` and binds a read-only element.
- **Copy cost**: a subscript is **always a member operator** (6.4.1, build 161), whose receiver is the
  implicit `this`: the object address for a `struct`, the handle for a `class`. Both are borrowed, so an
  iteration is a plain indexed load with no copy and no retain/release.
  > The free-function form `T operator[](C c, isize i)` was **removed** (build 161). A class cannot take a
  > `ref` parameter, so that form copied the receiver's handle on every element access — one
  > retain/release pair per element, measured 7.5 ns vs 0.15 ns per element (50x) in
  > `agents/stdlib/containers.md`. Declaring it is now a compile error.
- A temporary container (`for x in makeVec()`) is constructed only once and cleaned up at loop exit according to U24.

#### 6.4.2 Cursor form (sequential access; implemented 0.0.96)

```c
struct Iter {
    bool next() { ... }      // advance; false = end
    int  current() { ... }   // current element
}
class List { Iter iterator() { ... } }
```

- Conditions: a method `iterator() -> It`, and `It` has the methods `next() -> bool` and `current() -> T`.
  The contract is declared explicitly in `libs/std/interfaces.mu` as `std.interfaces.IIterator<T>` (loaded
  implicitly); the compiler checks it structurally, and a cursor may also declare `: IIterator<T>` to
  have the definition site checked. `next()` may block (a stream: an accept loop), in which case it is
  a park/cancellation point inside a task; a cursor is consumed by one loop at a time and need not be
  thread-safe.- Lowering: `auto it = c.iterator(); while it.next() { T x = it.current(); ... }`; `iterator()` is evaluated only once.
- The element type is the return type of `current()`.
- The cursor is a struct returned by value: zero allocation, static dispatch, inlinable; with class/interface fields it does per-field RC according to U24,
  cleaning up at loop exit / `break` / `return`.
- A cursor may instead be an **interface value** (`IIterator<T>`, 0.0.108): the loop then dispatches `next()`/`current()`
  through the itable. This is the dynamic channel — it is what `IIterable<T>` returns, because an interface value
  cannot hold a struct (no boxing). The loop holds one RC-managed local; the element type is still
  `current()`'s return type.
- The cursor form does not support `for ref` (`current()` returns a value); mutable iteration goes through the index form (`[]` returning `ref T`) or `for i, x`.
- **How the compiler finds the iterator** (0.0.107): by the member **name** `iterator()` — the same
  name-based structural recognition the index form uses for `length()` / the subscript. No interface
  is required and none is consulted; the element type is the return type of the cursor's `current()`.
  Interfaces in `std.interfaces` make the contract explicit at the definition site but are optional:
  - the cursor may declare `: IIterator<T>` — checked against `bool next()` / `T current()`;
  - the container may declare `: IIterable<T>` — checked against `IIterator<T> iterator()`.
  `IIterable<T>` is an ordinary generic interface (0.0.108): a class implements it by returning a class
  cursor as `IIterator<T>`, `for-in` iterates that value, and `T: IIterable<int>` works as a constraint.

#### 6.4.3 Resolution order and diagnostics

1. Built-in sequence → built-in lowering (see the start of this section); it does not participate in form matching;
2. A user type with an `iterator()` method → **cursor** lowering (implemented 0.0.96; a type opts into
   streaming by providing `iterator()`, and the cursor contract is `std.interfaces.IIterator<T>`);
3. Otherwise satisfies the index form → index lowering (**implemented**);
4. Both could be satisfied → the **cursor** form wins (providing `iterator()` is an explicit choice);
5. Neither is satisfied → a compile error that names `std.interfaces.IIterator<T>` and the index-form
   members (`length()` + a subscript).

- `length()` is a normal method for user types and a compiler-lowered accessor for `T[]`; static arrays have no `length()`;
  user containers write `e.length()`.

#### 6.4.4 Index variable `i`

- `for i, x in seq`: `i` is an `isize` with the same scope as `x` and is read-only inside the loop.
- Built-in sequences: a sequential number starting at 0 (for a range, the offset relative to `lo`).
- Index form: the real index; cursor form: the iteration ordinal.
- Implemented (built-in sequences + index form); the iteration ordinal of the cursor form lands together with 6.4.2.

### 6.5 Miscellaneous

- `return expr;` / `return;` (void).
- Releasing a reference: `c = null;` (since 0.0.47 `del` has been removed; see 3.8 / 8.3).
- `do-while` and `goto` are not provided (C-style `for` is, see 6.3.1).
- `match` is covered in 10.1.2: no fallthrough, and every arm body is a brace-delimited block
  (statement form) or an expression (expression form).
- Exceptions use `try`/`catch`/`throw` (3.14); for fatal errors `abort()` (libc) is still available.

### 6.6 `switch` (removed 2026-11-20)

`switch` was folded into `match` (10.1.2): the constant-pattern form keeps its whole vocabulary —
comma lists, ranges `a..b` / `a..=b`, `else`, exhaustive enums, no fallthrough and brace-delimited arm
bodies — and adds the expression form and guards. The keyword stays **reserved**, so old code gets a
migration diagnostic instead of a confusing parse error:

```text
`switch` was folded into `match`: write `match value { pattern: { ... } else: { ... } }`
```

---

## 7. Functions

```c
int add(int a, int b) {
    return a + b;
}
```

- The return type must be written; with no return value, `void` must be written explicitly and cannot be omitted.
- `void` can only be used as a return type or as the base type of `void*`; `void` variables, fields, parameters, and array elements cannot be declared.
- An empty parameter list is written `()`, meaning no parameters; the `(void)` compatibility form is not supported yet (pending).
- `void` functions use `return;`; `return expr;` is not allowed; the result of a void function call has no value and cannot be used in an expression that needs a value (it may be an expression statement).
- A non-`void` function must guarantee a return value on all paths; falling off the end is a **compile error** (settled). `main` is the exception: falling off is equivalent to `return 0;`.
- **Definite initialization** (build 269): a **scalar** local (`int`/`enum`/pointer) declared without an
  initializer is uninitialized, and reading it before every path to that read assigns it is a compile
  error. The rule is flow-sensitive: `if`/`match`/`try` keep a local uninitialized if *any* path leaves
  it so, and an assignment inside a loop body does not escape the loop (the body may run zero times).
  A mutable `ref` argument counts as initialization (the callee may write an out-parameter at once).
  Aggregates are not tracked yet: a field or element write is not a whole-value initialization, so
  `P p; p.a = 1; use(p.a)` stays legal and undiagnosed. A *moved-from* name is a separate state (3.4)
  and is reported on its own.

- Parameter form `T name`; `const T* p` means read-only through p.
- **An array parameter is a pointer** (C99 6.7.5.3p7, 0.0.106): `int[5] xs` is `int* xs` — the bound is
  not part of the type, `sizeof(xs)` is the pointer size, and writes reach the caller's array. Only the
  outermost dimension decays, so `int[2][3] rows` is a pointer to `int[3]`. Arrays of `class`/`interface`
  elements keep value semantics (there is no pointer-to-reference type): such a parameter copies the
  handles, retaining per element. `ref T[N]` is rejected like any other array borrow.
- **Each parameter is declared separately**: every parameter must spell out the full `type name`, with parameters separated by commas.
  The C-style shared-type form is not supported; `int add(int a, b)` is a **compile error** (diagnostic: declare a type for each parameter).
  This differs from variable/field declarations — in a declaration one type may carry several names (see 4.1), but the parameter list has **no** such sugar.
- A type belongs to a single parameter: in `int f(int* p, int q)` only `p` is a pointer and `q` is an `int`; suffixes (`*` / `[N]` / `[]`) do not carry across parameters.
- A function definition is written `T name(params) { ... }`, external functions use `extern`; no bare function prototypes are provided; `static` has been removed. Nested functions are not allowed inside a function body. Variable declarations are always `T name = ...` / `T name;`, so they cannot be confused with the function declaration `T name(params)`.
- Function calls do not support named arguments for now; `f(x = ...)` is a struct construction only when `f` resolves to a type (see 3.4).
- Within the same unit, calling before the definition is allowed (two-pass parsing).
- Recursion is legal.
- Function overloading is allowed (rules in 3.11); type-level generics are supported (see 3.15); default arguments, capturing closures, and nested functions are not supported (non-capturing lambdas are provided, see 3.16); variadic parameters are limited to `extern` (see 7.1).
- Class methods, constructors, and `this` are covered in 3.11/3.12; methods are static by default, `virtual` opts into dynamic dispatch, and an override is marked `override`.
- Parameters are mutable bindings (phase one); whether they become immutable by default is pending.
- `extern` declaration:

```c
extern int puts(const char* s);
```

  Uses the C ABI; `const` does not affect the ABI.
- **The C ABI applies to `extern` and `export`, and to nothing else** (0.0.121). A purely internal call keeps
  the backend's own convention (aggregates in registers, no copy), which is faster; the C ABI is paid for
  only on the boundary. Because a symbol has exactly one convention, the **Mu-side call sites of an `export`ed
  function also use the C ABI** — so do not mark hot internal helpers `export`. The two kinds of function
  call each other normally in **both** directions (an `export`ed function's body may call internal helpers,
  and internal code may call exported ones): the convention is a property of the callee, and every call
  site is emitted from it.
- **Aggregates by value** (struct/union parameters and results) follow the platform rule, not the backend's:

  | Target | 1/2/4/8-byte aggregate | other aggregates |
  |---|---|---|
  | Windows x64 (MS ABI) | passed and returned **as an integer of the same size** (never XMM, even for `{float, float}` or `{double}`) | parameter: **pointer to a caller-made copy** the callee may modify; result: **hidden pointer** (first argument) |
  | SysV x64 (Linux/macOS) | ≤ 16 bytes: registers | > 16 bytes: memory + hidden result pointer |

  Consequences that are easy to get wrong: a 16-byte struct is *not* two registers; a 24-byte struct is not
  three; a `{int, int}` result is one 64-bit value in `RAX`, not `RAX`+`RDX`; and a struct parameter is a
  **private copy** (writing to it does not reach the caller). Over-aligned aggregates always travel by
  reference. SysV's per-eightbyte SSE classification is not implemented yet (the target machine is fixed to
  Windows; recorded in `agents/design-backlog.md`).
- Which types may cross is checked: a class/interface (reference counted), a `delegate` (owns an environment),
  a dynamic array `T[]` (RC header), or a `variant` (`{tag, union}` is an implementation detail) is a compile
  error in an `extern`/`export` signature, including when nested in an array element or a struct field.
  `ref T` parameters are refused too (a language borrow; write `T*`).
- A `c_abi` function whose aggregates travel differently cannot be used as a **function value** (a function
  value carries only the Mu signature): call it directly.
- `main`:
  - `int main()` or `int main(int argc, char** argv)` (**implemented**); the return value becomes the process exit code; reaching the end is equivalent to `return 0;`.
  - The parameterized form is passed in by the C startup code: `argc` is the argument count (`argv[0]` is the program name, so it is always ≥ 1), `argv` is `char**`
    and is **null-terminated** (`argv[argc] == null`).
  - The parameter names are arbitrary, but the types must match `(int, char**)` exactly: one parameter, `char*`, or a non-`int` return are all compile errors.
  - `char**` is used rather than `string[]`: `argv` is a C-side bare pointer array without RC; when a `string` is needed, construct it with
    `std.string.string(argv[i])` (which copies it).
- Function values and lambdas are specified in 3.16 (implemented in 0.0.84); `extern` variadic parameters are supported (see 7.1);
  the language **does not provide** user-defined varargs.

### 7.1 Variadic parameters (`extern` only, implemented)

```c
extern int printf(const char* fmt, ...);
```

- Only `extern` declarations may have `...`. Variadic functions **cannot be defined** in Mulan (`int f(int a, ...) {}` is a compile error):
  type-unsafe reads conflict with principle 4, "errors that can be caught at compile time are not left to runtime"; APIs in the C ecosystem that need varargs are almost all `extern` C functions.
- `...` must be written last in the parameter list, and at least one named parameter must precede it (`extern int f(...)` is illegal).
- Arguments are passed according to **C default argument promotions**:

  | Argument type | Promoted to |
  |---|---|
  | `float` | `double` |
  | `bool` / `byte` / `char` / `short` / `ushort` | `int` |
  | enum | the underlying integer type |
  | fixed-size array `T[N]` | **`T*`** (C99 6.3.2.1 decay, applied before the promotions) |
  | `string` | **not allowed**: it holds reference-counted data; pass `s.cstr()` (a `const char*`) instead |
  | pointer / POD struct / union | unchanged |

- Arguments to `...` do not participate in reference counting: types that contain RC (class/interface references, dynamic arrays, structs with reference fields or with a user destructor)
  are a **compile error** as variadic arguments.
- No runtime type checking: `printf("%d", 1.5)` is UB as in C (the caller guarantees that the format string matches the argument types).
- When a "variadic API internal to Mulan" is needed, use `Value[]` (a tagged union, see 10.1); do **not** introduce a `va_arg`-style mechanism.
- `va_list` / `va_start` / `va_arg` / `va_copy` are not provided: forwarding C's `vprintf` family or exposing variadic functions to C
  is a different class of feature; see backlog U31.

### 7.2 C ABI types (`std.ctypes`)

Mulan's integer widths are fixed: `long` is **always 64-bit**, while C's `long` is 32-bit on Windows (LLP64)
and 64-bit on POSIX (LP64). Declaring an `extern` with Mulan's names therefore silently changes the ABI.

- Rule: **every `extern` declaration uses the C-side names from `std.ctypes`** (`c_int`, `c_long`,
  `c_size`, `c_voidp` …), never Mulan's `long`/`usize` directly. The aliases are transparent
  (`c_long` *is* `int` on Windows), so no conversion or cost is introduced.

```mu
use std.ctypes;

extern c_long ftell(void* stream);                  // 32-bit on Windows, 64-bit on POSIX
extern c_size fread(void* buf, c_size size, c_size n, void* stream);
```

`c_voidp` is `void*`; struct by value, unions and function pointers interoperate as in C (3.16).

The full name table, the per-platform implementation (`libs/std/platform/<os>/std/ctypes.mu`) and the
binding policy are library content:
an internal record (`agents/stdlib/c-bindings.md`) §3.

---

## 8. Memory management

### 8.1 Built-in functions

| Function | Semantics |
|---|---|
| `alloc(bytes: isize) -> void*` | Allocates uninitialized memory; returns `null` on failure |
| `realloc(p: void*, bytes: isize) -> void*` | Resizes; returns `null` on failure, leaving the original block unchanged |
| `free(p: void*)` | Frees; `free(null)` is a no-op |
| `sizeof(T) -> isize` | Size of a type, **including an instantiated generic** (`sizeof(IIterator<int>)`, `sizeof(Box<int>)`) and an anonymous delegate/function type | 
| `sizeof expr -> isize` | Size of the operand's type; the operand is **not evaluated** (`sizeof *p`, `sizeof a[i]`, `sizeof(a)`) |
| `alignof(T) -> isize` / `alignof expr` | Alignment of a type / of the operand's type (not evaluated); generic instantiations as above |
| `offsetof(T, field) -> isize` | Byte offset of a struct member (0 for every `union` member; compile-time constant) |
| `x.length() -> isize` | Length of a managed sequence (`T[]` / `String` / user containers); `x.length(axis)` for a multidimensional sequence; static arrays use `sizeof(a) / sizeof(a[0])` |

- `alloc` returns memory aligned to at least 16 bytes (consistent with common malloc); this is a **lower-bound guarantee**, not an upper bound.
  For heap restrictions on over-aligned types see 3.4 (class objects are allocated by the compiler with the required alignment; dynamic array elements have `align ≤ 8`;
  a manually `alloc`ed struct instance with `align > 16` is UB). A general `alloc_aligned` is still pending (backlog U27).
- `uninit new T[n]` is an allocation modifier, not a function: it skips the zeroing of the element
  block (3.3), and the element type must be trivial. A negative `alloc`/`realloc` byte count is UB;
  the managed form aborts instead (6.6).
- No GC, no ownership, no borrow checking; leaking is the programmer's responsibility.

### 8.2 Undefined behavior (UB)

- Using freed memory, or double-freeing (`free(null)` excepted).
- Dereferencing `null`, out-of-bounds pointer arithmetic/dereference, dangling pointers.
- Reading uninitialized memory.
- Observing a unit-level variable before its unit has been initialized, or after that unit's cleanup has
  run (9.1): only `use` dependencies are tracked, so an indirect dependency (a function pointer, an
  interface call, an `extern` C symbol, or a later variable reached through a function body) can produce
  such an access.
- Misaligned access: a pointer/field whose dereference location does not satisfy the type's natural alignment (fields of a `packed` struct are such a case; see 3.4).
- Data races between tasks or between threads (concurrency exists, 3.17, but races are neither prevented
  nor diagnosed; the rule against writing enclosing locals in a `para` region is a narrow syntactic
  guard, not a race checker).
- Division by zero; **signed division/remainder overflow** (`MIN / -1`, `MIN % -1`). Reported at run time by the
  `--bounds` check when the divisor is not a compile-time constant (a constant one is an error, see 3.7.2).
- **A shift count that is negative or >= the bit width of the left operand** (decided 2026-11-18): UB
  here (the hardware's masking of the count is not a language guarantee); reported at run time by the
  `--bounds` check, and rejected at compile time when the count is a constant.
- **A float to integer `as` whose value is NaN or outside the target type's range** (decided
  2026-11-18): UB here; reported by the `--bounds` check and rejected when the value is a constant. Both
  checks throw `std.exception.NumericEdgeError { kind, value, width }` (`kind` 0 = shift count,
  1 = conversion); `--bounds=off` removes them.
- Signed integer overflow of `+` `-` `*`: **settled: defined as two's-complement wraparound** (avoids UB and simplifies
  optimization; the backend does not emit `nsw`/`nuw`). **Division is the one exception**: the CPU traps on `MIN / -1`,
  so it is UB exactly like division by zero; write `-x` (which wraps) when that value must be produced.
- Managed-sequence out of bounds: checked by default and throws `BoundsError`; out-of-range shift counts and float-to-integer `as` conversions are checked the same way and throw `NumericEdgeError`; integer division/remainder by zero and `MIN / -1` throw `ArithmeticError`; `--bounds=off` removes these checks (each is then UB, as listed above). Static arrays and `T*` are unchecked.
- A null class handle / null interface value that is dereferenced: checked by default and throws `std.exception.NullReferenceError`
  (`--null-check=off` removes the check, and the access is then UB, as listed above).
- A pointer obtained by decaying an array **prvalue** (a function result, or the result of `?:`) is only
  valid until the end of the enclosing statement: the temporary it points into is gone afterwards.
- Accessing a class reference that is `null`; continuing to use a handle after it has been set to null (equivalent to a null reference). Reported at run time by the `--null-check` check (on by default, throws `std.exception.NullReferenceError`); `--null-check=off` removes the check and the access is UB.
- Using `alloc/free` on a class object, or assigning memory obtained from `alloc` as a class handle (the two memory management schemes must not cross).

### 8.3 Reference counting, vtable, and weak for class

Object layout (current implementation):

```
object:        [ rc | class_desc* | fields... ]
class_desc:    { size, destroy, vtable*, itable*, base_desc* }
interface value: { obj*, class_desc* }        // 16 bytes; null = {null,null}
dynamic array T[]:   { i64 rc, i64 len, i64 cap, data... }
dynamic array T[,]:  { i64 rc, i64 d0, i64 d1, ..., data... }
```

- A dynamic array handle is just the header pointer; null means an empty array with `len` 0; copying the handle is +1, and when it reaches zero each element is released and then `free` is called.

- `vtable`: the array of method slots, ordered so that "slots already present in the base class come first, and new methods of the derived class are appended"; `override` reuses the base-class slot.
- `itable`: the global interface method slot table (deduplicated and numbered by whole-program interface method signature); the value is a thunk (the class implementation or the interface default implementation).
- Every class object carries an **atomic** strong reference count; after `new` creates it the count is 1.
  Increments are **relaxed**, decrements are **release**, and the thread that observes the count reaching zero
  executes an **acquire fence** before running the destructor (the standard Arc pattern). This is what makes
  "the destructor runs on the thread that releases the last reference" see every write published by the other
  threads.
- **+1**: copying a handle (initialization, assignment rebinding, storing into a field, storing by the caller after a return, storing into an interface value).
  Parameter passing: the callee retains one reference on entry (parameters are owning local variables) and releases it on exit; the caller passes by value.
- **-1**: the handle is overwritten, leaves scope, the field is destroyed with its object, the interface value is overwritten, or the handle is set to null.
- **Reaching zero**: `class_desc->destroy`: runs the destruction chain (user destructor → derived fields released in reverse order → base class destructor → …) and then `free`s the object.
- **Virtual calls**: class methods go through `desc->vtable[slot]`; interface methods go through `desc->itable[slot]`, and interface default methods likewise.
- **Interface values**: strong references; copying is +1, overwriting is -1; `null` is not counted; upcasting between interfaces does not change the value.
- `weak` fields are not counted and are not released during destruction; **they are automatically set to null when the target is destroyed** (a runtime weak registry):
  assignment registers {target, field}, unregisters on overwrite/the holder's destruction, and when the target is destroyed any weak field still pointing at it is cleared.
  Cycles must be broken with `weak`.
- Cycles: **not collected**; an unbroken cycle = a leak.
- Early release: `c = null;` immediately -1s and sets c to null; if the object is still held by other strong references it is not destroyed (`del` was removed in 0.0.47).
- Separate from raw memory: `alloc/free/realloc` manage raw memory and `new` is only for class; the two must not be used interchangeably.
- Objects may be shared across threads; the destructor runs on the thread that releases the last reference and must be thread-safe.
- Construction failure (malloc returns null) currently calls `abort()` directly.
- An optional `--single-thread` build emits reference counting without atomics (plain loads and stores,
  no acquire fence); that build rejects `para`, `para for` and `std.thread` / `std.task` at compile time,
  so no object is ever shared. The default build keeps atomic RC.
- There is no resource release mechanism other than a user destructor; destructors are covered in 3.13.

---

## 9. Modules and C interoperability

- One unit per file:

```c
unit example;
```

  It must be the first declaration in the file.
- `use other;` imports another unit and accesses it as `other.symbol`; cyclic dependencies are forbidden.
  One statement may import several units (`use std.cstdio, std.string;`, build 249): a trailing comma is
  accepted, and the whole statement is atomic — a missing or malformed item imports nothing.
- **Unit name to file mapping** (implemented): a unit name may contain dots; `unit std.string` maps to `libs/std/string.mu`
  (dots map to directory separators). The library search path is, in order: the directory of the entry file, directories given with `-I` (may be repeated),
  the `MUC_LIBS` environment variable, and `libs` and `libs/std` under the current directory and its ancestors.
- Visibility (**implemented (0.0.30)**; see `visibility-design.md` for details):
  - Two modifiers: `private` = visible within this unit; `public` = visible across units.
  - Defaults: top-level declarations `private`; struct members `public`; class members `private`.
  - struct / class bodies support `public:` / `private:` section labels, and subsequent members follow that default;
    a per-member prefix (`public int x;`) overrides the current section. Destructors cannot be modified.
  - After `use`, the target unit's `public` declarations can be used directly **without a prefix** (types/functions/constants/enums/struct construction);
    the lookup order is local → current unit → directly `use`d units → std.string; local declarations win;
    if several sources have the same name, an ambiguity is reported at the use site and a qualified name is required.
  - **The library boundary is the first segment of the unit name** (`std.string` → library `std`; a unit without dots belongs to the root library). A cross-library `use`
    requires the target unit to be declared `public unit`.
  - Cross-unit aggregate initialization requires all struct fields to be `public`.
  - The boundary of `private` is the unit (not the class body): operator free functions, same-unit inheritance/tests can access the internal representation.
  - `extern` is implicitly public; adding `public`/`private` is forbidden; non-`public` top-level symbols use `internal` linkage.
  - A `public` declaration must not leak a non-`public` type (recursive checks on signatures, field types, base classes, and interfaces).
  - **`export`** (0.0.103) is a top-level declaration modifier that marks a declaration as part of the
    **binary export surface**. It **implies `public`** (the declaration is nameable across units like a
    `public` one) and cannot be combined with `private`, `extern` or `use`; inside a type or interface
    body it is an error (export the type itself — that exports its public member functions plus the
    class descriptor, vtable and RC destroy entry). Whether an export surface is emitted depends on the
    target and on `--exports=export|public` (default `export` = only `export` declarations;
    `public` = every `public` declaration).
    Since 0.0.121 an `export`ed **free function** is a DLL entry point and therefore uses the **C ABI**
    (7.2): its Mu-side call sites use the C ABI too, and its signature obeys the same restrictions as
    `extern`. Exported *class members* keep the Mu convention — a class handle cannot cross into C anyway.
  - Backend: non-`public` top-level symbols use `internal` linkage (`public` / `extern` / `main` keep external linkage).
  - **Visibility is a compile-time API contract, not a security boundary** (`void*`/`as` can bypass it).
  - `public` does not promise that the symbol appears in the final binary: an **executable** build
    internalizes every definition outside the export surface (plus `--export` names) so that dead code
    can be removed. Library targets keep their exported surface visible (see `agents/toolchain-plan.md` §11).
- No preprocessor: no macros, no textual substitution, no `#include`. Module-level constants use
  top-level `const`; a platform-specific variant is selected by a compiler directive (`#if`, 9.2),
  which chooses code at build time and leaves no trace in the output.
- `extern` is used for C functions/global variables; C ABI; struct layout is compatible with C. `extern` functions cannot be overloaded.
- Building against C objects: `muc build <entry.mu> --link <obj> [--link <obj> …]` hands extra object files to
  the link step (compile them with the same `--cc` driver); `tests/abi/` uses this to check the C ABI of
  aggregates against a real C compiler in both directions.
- Class objects cannot be passed to C: the C side does not participate in reference counting; when crossing languages use a struct + `alloc`.
- A struct that needs cleanup (a destructor, or a reference-counted/owning field) cannot cross the C ABI **by value** in either direction, even when it is movable inside the language: a foreign caller cannot hand over or take back the resource the callee's destructor assumes. Use a pointer, or a plain C struct.
- A string literal is a NUL-terminated `const char*` and can be passed to C directly.
- Symbol mangling is injective (12 #88): `mu$<unit>$<name>$<param>...`, with the components separated
  by `$`, every literal `_` doubled and every other non-alphanumeric byte written `_XX` (uppercase hex).
  **Low-level `const` is part of a parameter's type code** (decided 2026-11-17), so `f(int*)` and
  `f(const int*)` get distinct symbols; top-level `const` is already dropped when the signature is formed.
  `extern` declarations keep the C name; `main` is special.

### 9.1 Unit-level initialization and cleanup (implemented 0.0.102)

Unit-level variables may have runtime initializers and are destroyed at exit. This section defines
the order and the rules; it lifts the three "no exit-time destruction" restrictions that used to
appear lower in this document (3.3 `String`, 3.4 reference fields, 3.4 user destructors).

- **No new syntax.** `T g = expr;` accepts any runtime expression, and cleanup is the type's `~T()`.
  A unit-level variable whose type needs cleanup (a class/interface handle, `String`, a managed
  sequence, a struct with reference fields or a user destructor) is destroyed at exit.
- **Dependency edges come only from `use`.** Cyclic `use` is already forbidden (9), so the units form
  a DAG. Five standard-library units are **loaded** without a `use` — `std.string` and `std.exception`
  (a string literal needs the type and every error needs its base), plus `std.task`, `std.thread` and
  `std.interfaces`, which the compiler and the `for-in` lowering reference. Only the first two are
  also **name-visible**: the other three load so the machinery exists, and naming one of their types
  still needs an explicit `use` (a name that appears by itself would be an implicit that the reader
  cannot see at the declaration).
- **Deterministic order.** Starting at the **entry unit** (the unit that declares `main`), visit `use`
  declarations in source order, depth-first, and append each unit in **post-order** on first visit.
  That sequence initializes the units; cleanup runs the **exact reverse** of the same sequence. The
  order depends only on the source — never on link order, file names or the order units are compiled in.
  **The same sequence also orders compile-time constant evaluation and `#if` selection (9.2/3.7.2)**: a
  constant may reference a unit that was already completed in the sequence, i.e. one that is `use`d before
  the reference point. There is exactly one order in the language.
- **Within a unit**, variables initialize in declaration order and clean up in reverse declaration
  order; together with the previous rule, cleanup is the strict reverse of construction. Constants are
  evaluated in the same declaration order.
- **Who participates.** Compile-time `const`s, `extern` declarations, and variables without an
  initializer whose type has no constructor stay in the static data area and run no code (a struct
  with a parameterless constructor is constructed by the sequence, like a local `S s;`). An
  initializer the constant evaluator can fold (3.7.2) also stays static. A `const` whose initializer is
  **not** foldable (e.g. `const string S = "hi";`) is still a `const` object but is initialized at run
  time by this sequence, and cannot be used in `#if`/array lengths/`step`/`align(N)`. A variable whose
  type needs cleanup is registered for cleanup even when it is zero-initialized, because the program may
  assign it later.
- **What an initializer may observe.** Its own unit's earlier declarations and `public` declarations of
  units it `use`s — which are exactly the units already initialized, because a `use` edge is what makes
  them nameable. A direct reference to a variable declared **later in the same unit** is a compile
  error. Dependencies that cannot be written as a `use` edge — a function pointer, an interface call, an
  `extern` C symbol, or a call that reaches a later variable through a function body — are not tracked;
  if the order depends on one, the program is undefined. An initializer does not receive `argv`.
- **Failure.** An exception escaping an initializer aborts the process **without** running cleanup: the
  object graph is incomplete and destroying part of it is more dangerous than dying. This matches an
  uncaught exception in `main`.
- **Entry and exit.** Each unit's init hook is idempotent (a per-unit guard), initializes its `use`
  dependencies first, then its own variables, and finally registers its own cleanup with `atexit`
  (LIFO, so cleanup is the reverse of construction). The compiler emits a C `main` wrapper that calls
  every unit hook in loader order, then the user `main` (renamed internally) and returns its value.
  `exit()` therefore also runs the cleanup; `_exit`/`abort` do not. An exception escaping cleanup
  aborts the remaining cleanup.
- **Order relative to the C runtime** (ABI promise): initialization runs after C/C++ static
  constructors (`.init_array`) and before the user `main`; cleanup runs after the user `main` returns
  or `exit()` is called, and before C/C++ static destructors (both paths are `atexit` LIFO).
- **Threads.** The initializer sequence completes before any thread or task exists; an initializer
  must not start `para` sections or `thread`s (a rule, not a diagnostic: the callee is an arbitrary
  function). Cleanup begins after every thread has joined.
- Each unit's initializer and cleanup are **separate symbols**, so a future split-compilation mode can
  assemble the same sequence at link time; only the entry unit materializes the call sequence today.
  A build without an entry unit (a library check) generates no sequence at all.

### 9.2 Compiler directives: `#if` (implemented, build 158; evaluation model redesigned 2026-11-17)

`#if` is a **compiler directive**, not a preprocessor: it selects which code *exists* in this build.
There are no macros, no textual substitution and no `#include`.

```c
use std.comptime;                              // required before using os/arch/...
#if os == OS.Windows and arch == Arch.X86_64 {
    extern void* CreateFileW(const ushort* name, uint access, uint share,
                             void* sec, uint disp, uint flags, void* tmpl);
    public type File = void*;
}
#elif os == OS.Linux {
    extern int open(const char* path, int flags, ...);
    public type File = int;
}
#else {
    #error "no file backend for this platform"
}
```

- **The selected arm is spliced** into the enclosing list. The braces delimit the directive; they do
  **not** introduce a scope, so a variable declared in an arm stays visible after the directive.
- **Only one arm is compiled.** The other arms are parsed (their syntax is checked) but never
  name-resolved, type-checked or emitted; a nested `#if` inside a dropped arm is not evaluated. An arm
  may therefore mention symbols that do not exist on this platform.
- **Positions**: a declaration list (unit top level, struct / class / interface body) or a statement
  list (function body, block). A single-statement position is an error (`if (c) #if …`), and the
  `unit` declaration itself cannot be conditional.
- **Syntax**: `#` must be the first non-blank character of its line; an arm is `#if <cond> { … }`,
  continued by `#elif <cond> { … }` / `#else { … }`. There is no `#endif`. `#error "message"` fails the
  compilation when it is reached.
- **When it is evaluated** (decided 2026-11-17, refined 2026-11-18): in the same pass as compile-time
  constant evaluation, following the single sequence of 9.1 — entry unit DFS post-order over `use`,
  declaration order inside a unit. In a **declaration list** (unit top level, struct / class / interface
  body) the condition sees exactly the constants visible at that source position:
  - the `std.comptime` builtins and functions (9.3) after `use std.comptime`;
  - earlier unit-level `const`s of this unit that are compile-time constants (3.7.2);
  - `public const`s of the units this unit `use`s.
  A constant declared later in the same unit, a `const` that is not a compile-time constant, a local
  variable, a runtime value, `sizeof`/`alignof`/`offsetof`, and ordinary function calls are errors. Calls
  to `std.comptime` builtin functions are allowed (9.3).
- **Inside a function body** the `#if` is expanded after the unit's declarations have been collected, so
  it follows the same name resolution as the surrounding code: every compile-time `const` of the unit is
  visible wherever it is declared (a later `const` too), and the order of `use` declarations inside a
  unit is not significant — a `use` is visible in its whole unit, as it is for ordinary code.
- **One name-resolution rule.** `#if` conditions use the same name resolution as a constant initializer:
  cross-unit names require a `use` of the unit (and `use std.comptime` for the builtins), a later
  is not visible in a declaration list, and ambiguity is reported as usual. Dependencies exist only
  through `use` edges — a constant reference does not create an implicit unit dependency.
- **`use` inside an arm** is allowed, and only the units of selected arms join the dependency graph.
- `muc dump-config` prints every builtin value; that output plus the platform spec is the reproduction
  key of a build.
- **Deliberately not supported** (decided 2026-11-16): `#if` **inside** an enum member list, between
  the alternatives of a `variant` declaration, or inside a single parameter list. A conditional enum
  member would silently shift the implicit values of every later member; a variant alternative would
  split a single `variant` declaration; a conditional parameter would need the comma list split. The
  granularity is too fine to pay for. Wrap the whole declaration instead — a declaration list is a
  supported position. The diagnostic names the rule and points here.

### 9.3 The `std.comptime` unit and `builtin` declarations (decided 2026-11-17)

`std.comptime` is an **ordinary unit** shipped with the toolchain: its position, visibility, import and
reference rules are exactly those of any other unit. What is special is the `builtin` modifier: it binds a
name to the compiler's built-in table.

```c
// std.comptime unit
public enum OS { Windows, Linux, MacOS }
public enum Arch { X86_64, AArch64 }
public enum Abi { Msvc, Ucrt, Gnu, None }
public enum Endian { Little, Big }

public builtin const OS     os;
public builtin const Arch   arch;
public builtin const Abi    abi;
public builtin const int    ptr_bits;
public builtin const Endian endian;
public builtin const bool   debug;
public builtin const bool   bounds;
public builtin const bool   null_check;
public builtin const bool   single_thread;

public builtin bool is_windows();
public builtin bool is_linux();

// platform-specific symbols: recommended home (library policy, not a language restriction)
#if os == OS.Windows {
    public type File = void*;
    public extern File open_file(const char* path);
}
#elif os == OS.Linux {
    public type File = int;
    public extern File open_file(const char* path);
}
```

- **Declaration form**: `builtin` constant = `builtin const T name;` (no initializer); `builtin` function =
  `builtin T name(params);` (no body). The compiler supplies the value/implementation; the "a `const`
  variable must be initialized" rule (3.7) does not apply to a `builtin` constant.
- **Positions are unrestricted**: any unit may write a `builtin` declaration. The compiler checks the name
  against its built-in table: a name not in the table is an error
  (`builtin 'X' is not provided by this compiler`); the same name may be declared **once across the whole
  program** (a duplicate is an error). A type/signature mismatch is an error.
- **Visibility and reference** follow ordinary units: `public` is needed across units; after
  `use std.comptime` the names are usable **without a qualifier** (`os`, `is_windows()`), and the qualified
  form (`std.comptime.os`, `std.comptime.is_windows()`) is also accepted. The unit is `std.comptime`; the
  short spelling `comptime` is not a unit name (`use comptime;` fails, and the loader suggests the full
  name).
- **Explicit import**: a unit (including its `#if` conditions) must `use std.comptime` before the names
  become visible. There is no implicit visibility of the builtins, and a `#if` in a declaration list only
  sees a `use` written before it (9.2).
- **Types**: builtin constants and function parameters/returns are limited to scalars, `bool` and enums;
  pointers, strings and aggregates cannot be `builtin`.
- A `builtin` declaration produces **no symbol and no ABI**: it is resolved and folded by the compiler, and
  cannot be `extern`.
- **A builtin constant is a compile-time value, not a run-time object** (decided 2026-11-17): it has no
  storage, cannot have its address taken, does not participate in layout/ABI, and is frozen once the
  compilation starts. Every reference is folded to the value.
- **Runtime builtins are not part of `std.comptime`.** Platform symbols and functions that exist at run
  time (a `File` handle type, `open_file`, …) belong to a separate unit supplied by the library/platform
  layer; `std.comptime` only carries compile-time constants and compile-time functions.
- **`#if` treats these names like any other constant**: there is no special "build constant" syntax or
  resolution. After `use std.comptime`, the names are ordinary visible constants; before the `use`, they
  are not visible.
- **Contract**: the `std.comptime` unit is the language's contract with the compiler — every name it declares
  must be provided with the declared type and semantics, and the compiler may not silently omit one.
  Adding, removing, or changing the type of a builtin is a breaking change and is recorded in
  `docs/versions.md`.
- **Runtime use**: calling a builtin function outside a compile-time context is folded to its value (it has
  no runtime implementation); the same holds for a builtin constant.
- `std.target` has been **removed**: its target enums and constants are now declared by `std.comptime`.
  The old "compiler rewrites the initializer of `std.target`" mechanism is gone; `builtin` constants are
  bound by the compiler (9.3).

### 9.4 Capacity limits (decided 2026-11-18)

The compiler must fail with a diagnostic instead of being blown up by a generated or hostile program.
The values are the informative minima of C++ [implimits] where C++ has one (C and C++ agree on the small
ones: 127 [C 5.2.4.1] vs 256 parameters), and every one of them is far above real code — the standard
library's largest unit is a few thousand lines and its whole `use` closure a few dozen units, and no
hand-written program comes anywhere near 256 levels of nesting or 4096 fields.

| Input | Limit | C / C++ / Rust | Diagnostic |
|---|---|---|---|
| One source file | 16 MiB | no limit (bounded by memory) | `source file … is N bytes; the limit is …` |
| Tokens in one file | 2M | no limit | `a source file must not contain more than 2000000 tokens` |
| Units in one program (the standard library's own closure included) | 1024 | no limit | `this program uses more than 1024 units` |
| Parenthesised expression nesting | 256 | C++ §1.4: 256 | `expression nesting too deep (limit 256 levels)` |
| Block / statement / initializer nesting | 256 | C++ §1.1: 256, C 127 | `block nesting too deep (limit 256 levels)` |
| Pointer / array declarators per type | 256 | C++ §1.3: 256, C 12 | `type is too long (limit 256 …)` |
| One operator / member chain | 2048 | no minimum (implementation detail) | `… is too long (limit 2048 …)` |
| Nested `#if` chains | 256 | C++ §1.2: 256, C 63 | `` `#if` chains are nested too deeply `` |
| Parameters of one function | 256 | C 127, C++ §1.11: 256 | `a function cannot declare more than 256 parameters` |
| Enum members of one enum | 4096 | C++ §1.22: 4096, C 127 | `enum … has more than 4096 members` |
| Fields of one struct / class | 4096 | C++ §1.20: 16384 (4096 per member-specification) | `… declares more than 4096 fields` |
| Patterns of one match | 16384 | C++ §1.19: 16384 case labels per switch | `this match has more than 16384 patterns` |
| Generic instances per program | 4096 | no minimum (Rust: `recursion_limit` 128 per chain) | `this program needs more than 4096 generic instantiations` |
| Nested generic instances (one chain) | 256 | C++ §1.9: 1024, Rust 128, GCC 900 | `recursive generic instantiation … does not terminate` |
| By-value type nesting (a `struct` inside a `struct` …) | 512 | no minimum (Rust `type_length_limit` 1048576) | `… nests more than 512 levels by value` |
| Class inheritance chain | 256 | no minimum (GCC/Clang accept thousands) | `… is more than 256 levels deep in its inheritance chain` |
| Interface inheritance chain | 256 | no minimum | `… inherits more than 256 interfaces in one chain` |

A program that reaches a limit exactly is accepted: the check runs when the next item is added, so
`k` items are allowed and `k + 1` are rejected.

Two properties hold for every row:

- **A bound is a stack guard, not just a work bound.** The parser, the semantic passes and the code
  generator walk the tree recursively, so a depth without a limit is a stack overflow waiting for input
  (120k nested `#if` used to crash with `0xC00000FD`).
- **A truncated walk is never kept.** The internal walkers stop at the same bound as the enforced limit
  (class / interface relations, `type_size`), so a program that compiles cannot carry a silently wrong
  layout, subclass relation or inherited member. That covers every composite type the checker can size:
  a `struct`/`class`, the synthesized layout of a `variant` (tag + payload union), and every generic
  instance. A type that contains itself by value — through a `variant` payload as well as directly — has no
  finite layout and is reported as a cycle rather than as a deep chain.

The interface and base-class graphs are DAGs: a diamond makes any memo-less walk visit shared ancestors
once per path, which is exponential in the number of levels. Every such walk (relation tests, default
method lookup, abstract-method collection) keeps a visited set, so a generated diamond is linear.

## 10. Phase-one non-goals

- Multiple inheritance (class allows single inheritance only)
- Container classes (`List`, `Map`, ...): library-level collections are not provided; dynamic arrays `T[]` / `T[,]` are built in (3.3)
- Platform unwinding tables / personality routines (exceptions use the B2 TLS-flag lowering, 3.14)
- Capturing closures (non-capturing lambdas and function values are in scope, see 3.16)
- GC/ownership/borrow checking
- Unrestricted operator overloading (only the limited set in 3.11 is allowed)
- Exponentiation operator (`^` is reserved for bitwise xor; use the `pow` function for exponentiation)
- Preprocessor/macros
- RTTI/reflection
- **Most of language-level concurrency.** What *is* provided is 3.17 (`para` / `para for` / `thread`).
  Explicitly not provided: detached tasks / fire-and-forget; `async` / `await` and colored async
  functions; `spawn` syntax (deferred join is a library task group); preemptive scheduling of CPU-bound
  tasks; timeouts or concurrency caps in `para` syntax; engine tags (`io{}`, `fiber`); actor runtimes /
  supervision trees, STM, reactive streams; a `shared` type qualifier or data-race checking.
- Language-level dynamic typing (`dynamic`)
- A complete standard library (only built-in functions + libc)

### 10.1 Recommended route for heterogeneous values

- **Phase one**: `void*` + manual tag (a low-level escape hatch).
- **Language-level tagged unions** landed in 0.0.115 as "sum types"; the declaration keyword is now
  `variant` (decided 2026-11-20) and `match` is the dispatch construct. Layout, construction and value
  semantics are unchanged. A library `Option<T>` / `Result<T,E>` becomes ordinary code once `match`
  is implemented.
- `dynamic` is only reserved as the name for future "runtime dynamic typing" and does not enter phase one; even if needed later, it should be an independent runtime layer rather than a core language feature.
- `any` is reserved as a static top type/library type, distinct from `dynamic`.

#### 10.1.1 `variant` declarations (§12 row 55)

```c
variant Value {
    Int int;
    Pair int, int;
    Text const char*;
    None;
}

Value a = Value.Int(3);
Value b = Value.Pair(4, 5);
Value c = Value.Text("hi");
Value d = Value.None;        // a unit alternative is a plain value
```

- **Declaration** is a `variant` declaration (decided 2026-11-20) with a block of two or more
  alternatives: `variant Name { Alt T; Pair T, U; None; }`. Each alternative is `;`-terminated; a
  single payload is written without parentheses (`Some T;`), several payloads are comma-separated
  (`Pair T, U;`), and a unit alternative is just the name. Alternative names are unique within the
  declaration; a name that collides with a visible type is an error in this phase. `type` no longer
  declares a tagged union (4.5): an old `type V = A | B;` is rejected with a diagnostic pointing here.
- **Generic variants** (0.0.117): `variant Option<T> { Some T; None; }` takes type parameters and
  instantiates per concrete argument, exactly like a generic struct — `Option<int>` and
  `Option<const char*>` are two different types with their own layout and alternatives. Construction
  and `match` use the same forms: `Option<int>.Some(3)`, `Option<int>.None`,
  `match o { Some(v): ... None: ... }`.
- **Alternatives** have 0..n payload types (types only, no names, as with any anonymous function
  type). Duplicate alternative names and a single alternative are errors.
- **Layout**: `{ i32 tag; union payload }` — the payload area behaves like a `union` (every
  alternative starts at the same offset, `sizeof` is the largest, alignment is the largest), so
  `sizeof(Value)` above is 16 (tag padded to 8 + an 8-byte payload).
- **Value semantics**: assignment and argument passing copy the value; the active payload is
  retained, overwriting or leaving scope releases it. A payload whose type needs cleanup
  (class/interface handle, `String`, slice, struct with reference fields) is therefore safe to store:
  the variant owns one reference. Payload types with a **user destructor** are rejected (they cannot be
  copied by value, same rule as array elements).
- **Construction**: `Type.Alt(values...)` for a payload alternative, `Type.Alt` (no parentheses) for a
  unit alternative, and — when the target type is already known — the **bare alternative name**. The
  target is known in every target-typed position: a declaration or assignment (`Option<int> o = Some(3);`,
  `o = None;`), a call argument (`take(Some(3))`, whose target is the parameter type), a `return`, both
  arms of a ternary, and either operand of `==` / `!=` with a variant value (`if o == None`,
  `if o == Some(3)`, `if Some(3) == o`). A user function with the same name wins at a call site that has
  no such target, so existing code is unaffected (0.0.119; argument and comparison positions restored
  2026-11-19).
- **`is` is an ordinary boolean expression unless it binds**: `o is Some`, `o is None` and
  `a is Cat` are `bool` values, so they compose with `and` / `or` / `not`, sit in any condition
  position (`if`, `while`, the condition of a C-style `for`, the `?:` selector) and may be stored.
  The binding form (`o is Some(v)`) is the one exception: the bound name exists only inside that
  branch, so the `is` must be the **whole** condition (`if o is Some and flag` is fine,
  `if o is Some(v) and v > 0` must be nested).
- **Testing and extracting without a full `match`**: `is` takes a pattern —
  `if o is Some { ... }` (tag only) or `if o is Some(v) { ... }` (binds the payload; the name is a
  local of that branch only; binding is allowed only when the `is` is the **whole** condition, so
  `if o is Some and flag` composes but `if o is Some(v) and v > 0` must be nested). `is` also tests a
  **class/interface** value: `if a is Cat c { ... }` (the binding is the downcast; `Cat` must be a
  subtype of `a`'s type). Any other scrutinee type is rejected — `int`/`struct`/`enum` have no runtime
  discrimination, so `is` there would be a constant.
- **Value equality** (0.0.119): `==` / `!=` compare the tag and, when it matches, the payload fields.
  Only alternatives whose payload types have value equality take part (scalars, enums, pointers,
  function values); a class/interface/`String`/slice/struct payload is rejected with "no value
  equality (read it with `match`)" — reference payloads would need an identity rule.
- **Not allowed in C signatures**: a variant (or any struct/array containing one) is rejected in an
  `extern` parameter or return position — its `{tag; union}` layout is a compiler implementation
  detail, not a public interface. To talk to C, declare a plain `struct { tag; union }` on both sides
  and convert explicitly.
- **`para` / `thread`**: nothing special. A variant is an ordinary value type: a section may read and copy
  it, a delegate may capture it by value, and a reference-counted payload follows the usual RC rules.
- Equality (`==`, `!=`) compares the tag and then the payload fields (0.0.119); only payload types with
  value equality take part.
- C ABI: the layout is the classic `{ tag, union }` shape, but the type **never** crosses into C (see
  the `extern` rule above): it is a language built-in whose representation is not a public interface.

#### 10.1.2 Dispatching a variant: `match` (implemented in build 258)

`match` is the **only** dispatch construct: a `variant` is matched by alternative patterns, an
**integer, `char`, enum or `bool`** by **constant patterns** — the vocabulary `switch` used to provide
(6.6).

```c
int x = match v {
    Int(n): n;
    Pair(a, b): a + b;
    Text(s): 1;
    None: 0;
};
```

- **Expression form**: every arm body is an expression; arms end with `;` and the whole `match`
  produces one value. **Statement form**: every arm body is a brace-delimited block; the `match` is a
  statement. The two forms are not mixed in one `match` (a mix is a compile error).
- An arm is `pattern [if guard] : body`. The pattern is one alternative (`Alt`, `Alt(x)`,
  `Alt(x, y)`); the binding count must equal the payload count. Bindings are ordinary locals of the
  arm. A reference-typed payload is retained for the arm and released when it ends.
- Bindings are by value by default; `ref x` is a mutable borrow of the payload and `const ref x` a
  read-only borrow (they cannot escape the arm body). `match move v { ... }` consumes the whole
  variant, and `v` cannot be used afterwards.
- A guard is evaluated after the bindings; when it fails, matching continues with the next arm. An
  arm with a guard does **not** cover its alternative for exhaustiveness.
- **A match expression accepts a target type**: in `Option<int> o = match r { Ok(v): Some(v); Err(e): None; };`
  the declared type is handed to every arm, so bare alternative names resolve (build 263).
- **Exhaustiveness is mandatory** (unless the match has an `else`): every alternative must be
  covered, and a missing one is a compile error naming it. There is at most one `else`, and it is
  last; an `else` after a fully covered match is unreachable and warns.
- **Constant patterns** (integer, `char`, enum or `bool` scrutinee): an arm lists one or more
  patterns separated by `,`; a pattern is a compile-time constant (literal, enum member, or a constant
  expression the same evaluator folds) or a range `a..b` (half-open) / `a..=b` (inclusive).
  Overlapping or duplicate patterns are errors; an enum match must cover every member unless it ends
  with `else`. Guards apply here too, and a failed guard continues with the next arm. In the
  **expression** form every input must produce a value, so an integer match needs `else` (an
  exhaustive enum match needs none). The matched value is evaluated once.
- **`bool`** patterns are `true` / `false` (or a unit-level `const bool`), never `1` / `0`; ranges do
  not apply. A bool match has a closed value set, so it must cover both values unless it ends with
  `else` (an exhaustive bool match needs none in the expression form).
- **Or-patterns**: `A(x) | B(x):` matches any of several alternatives with one body. Every
  alternative must bind the same names in the same order, with the same binding modes and the same
  types; a repeated alternative in one pattern is an error. Coverage counts each alternative (an
  unguarded or-pattern covers all of them). Alternatives **without a payload**, and constant
  patterns, use the comma list (`None, Empty:` / `1, 2:`) — `|` stays the bitwise-or operator there.
- The first version still supports **one pattern level only**: nested patterns, type
  (class/interface) patterns and public-contract branches are not provided yet (10.1.3).
- `is` keeps its current rule (a binding only when the `is` is the whole condition, 10.1.1); guarded
  cases are written with `match`, not with `is`.
- A `match` over a variant counts as a terminating path for the return-value check when every arm
  returns, so a function over a variant needs no trailing `return`.
- Implementation: a variant match lowers to the existing tag dispatch; a constant match reuses the
  former constant-dispatch lowering — an LLVM `switch` instruction (jump table) for single values, a
  comparison chain for ranges,
  and an ordered chain when any arm has a guard (a guard makes source order observable).

#### 10.1.3 Later phases for `match`

- Type patterns (class/interface branches, replacing `is` chains) and nested patterns.
- Public-contract branches (an arm covering the remaining alternatives whose payload satisfies a
  constraint), with element-wise and interface-view forms.
- **`is` does not grow.** A binding `is` must stay the whole condition; guards, or-patterns and
  nesting are the job of `match` (10.1.2). Growing both would give the language two pattern languages,
  and the one without exhaustiveness would be the easier one to reach for.
- A POD `variant` crossing the C ABI: still rejected in this phase (10.1.1); any future ABI rule must
  be written into the specification first.

---

## 11. Main differences from C

| Topic | C | Mulan |
|---|---|---|
| Mutability | `const` qualifier | **C++-model cv-qualified types** (top-level + low-level const, `const` methods, `const` fields); no `constexpr`/`consteval`/`constinit`, no `mutable`/`const_cast` (use `as`) |
| Variadic parameters | `...` + `va_arg` (user-definable) | Only `extern` declarations may have `...`; `va_list` is not provided |
| Declarations | Declarators mixed with types | Type first; multiple variables share the type (including pointers/arrays) |
| No return value | The return type may be omitted (C89) | `void` must be written explicitly |
| Initialization | `T x = 1;` | Declarations use `=` only: `T x = expr`, `T x = {...}`, `T x = {}` (aggregate zero value), scalars `= 0`/`= null`; `T(...)` is expressions only |
| Local type inference | — | `auto x = expr;`; local only, an initializer is required; multiple declarators must have the same type |
| Dynamic typing | None at language level (void*/variant) | None in phase one; `dynamic` is reserved only |
| Unit-level variables | May be declared directly | Declared directly; `private` by default, `public` across units |
| Zero-value initialization | — | Declarations: aggregates `T x = {}`, scalars `= 0`/`= null`; expressions `T()` — including a primitive (`int()`, `bool()`, `double()`) and a **type parameter** inside a generic body, where the value depends on the instantiation; no more `T x()` |
| Brace initialization | Anywhere | Declaration-initializer position only |
| Loops | `for(;;)`, `while`, `do-while` | `while`, `for-in` |
| Increment | `++`/`--` | `x++` / `x--` (statement only, no return value) |
| Conditions | Any scalar truthiness | Must be `bool` |
| Braces | May be omitted | Mandatory |
| Condition parentheses | Required | Optional, omitted in canonical style |
| Conversions | Many implicit | Only widening that loses no range; otherwise `as` |
| Arrays | Degenerate to pointers; array objects are not assignable | Same decay and non-assignability (C99, 0.0.106); `T[N]` parameters are `T*`; remaining differences: an array-typed `return` copies, arrays of `class`/`interface` handles are copied element by element, and `T[N]` → `T[]` allocates and copies |
| Strings | `char*` | a literal is a `const char*`; `string` is a library value type (UTF-8, RC-backed; `.cstr()` for C, `strlen()` for C-string length) |
| Type casts | `(T)x` | `x as T` |
| Function pointers | `int (*f)(int)` declarators | `(int) => int`; one word, no `*`, no declarator nesting; lambdas are non-capturing (3.16) |
| Logic | `&&` / `\|\|` / `!` | `and` / `or` / `not` |
| Bitwise complement | `~` | `~` (`not` is reserved for logical negation) |
| Exponentiation | `pow()` library function | Also only `pow()`, **no operator provided** |
| Bitwise precedence | Lower than comparison | Higher than comparison |
| `not` precedence | Unary, highest tier | Low precedence, only higher than `and`/`or` |
| Preprocessing | Yes | No |
| goto | Yes | Not provided |
| switch | Fallthrough, `case` labels | Replaced by `match`: no fallthrough, no `case`, `pattern list : { }`, enums must be exhaustive (10.1.2) |
| Struct construction | `(struct S){...}` | Declaration `S s = {x=..., y=...}`; expression `S(...)` |
| Heap objects | `malloc/free` | `new` + reference counting (no GC); early release is written `x = null` |
| References | Pointers + `->` | `class` handles, auto-dereferenced; structs use `ref` parameters |
| Cycles | Manual | Break cycles manually with `weak` fields; no cycle collector |
| Inheritance | Yes | Single inheritance + virtual methods by default; no multiple inheritance |
| Interfaces | Abstract classes / interfaces | `interface`: multiple implementations, interface multiple inheritance, default methods |
| Overloading | Yes | Functions/methods/constructors/operators; bounded resolution (3.11) |
| Operator overloading | Members/free functions | Free functions only, limited set (3.11) |

---

## 12. List of Open Questions

| # | Question | Suggested default |
|---|---|---|
| 1 | Declaration syntax | Decided: C# style, type first, multiple variables share the type |
| 2 | Whether condition parentheses are mandatory | Decided: optional, omitted in the spec; C style remains legal |
| 3 | Fixed-width type naming (C names vs `i32`) | Use C names first |
| 4 | Layout of `string` and conversion to `byte[]` | Decided (0.0.89): `string` is a **pure library type**; the compiler knows neither its layout nor any of its operations (`src/` no longer has a `TypeKind::String`). A literal is a `const char*` (3.3); the library provides `implicit string(const char*)`, `length()`, `operator[]`, `operator==`, `cstr()`, `strlen()`. Unit-level `string` variables are allowed since 9.1; a `const` one is allowed too (runtime-initialized, not a compile-time constant) |
| 5 | Whether arrays implicitly decay to pointers | **Decided (0.0.106): yes, as in C99.** An array in value position decays to a pointer to its first element (initializer, assignment, argument, struct-field access, row of a static multi-dimensional array); `sizeof`/`alignof` do not decay. The array object itself is **not assignable** (`a = b`, `int[N] b = a`, `s.v = a` are errors) and a `T[N]` parameter is `T*` (6.7.5.3p7). Kept as Mu extensions: an array-typed `return` copies, and a prvalue array (function result) can initialize an array. Arrays of class/interface elements keep value semantics (no pointer-to-reference type exists) |
| 6 | Whether dynamic arrays are included in phase one | Decided: included. `T[]` / `T[,]` / `T[,,]` are **built-in types** (2026-11-16, superseding the earlier "sugar for `Array<T>`" rule): the compiler owns the representation `{rc,len,data}` (no capacity field — a block never grows, so its addresses stay valid), the built-in members are only `length()` / `operator[]` / `ptr()`, and `new T[n]` zeroes the elements unless written `uninit new T[n]` (trivial element types only). The `buffer<T>` name is removed. A library container (`Array<T>` and friends) wraps a `T[]` and adds methods |
| 7 | Bounds checking strategy | Managed sequences checked by default (`--bounds=on` / `--bounds=off`), a violation throws `BoundsError`; static arrays and `T*` unchecked |
| 8 | Signed overflow | Decided (implemented): `+`/`-`/`*` wrap in two's complement (the backend emits no `nsw`/`nuw`); signed division/remainder overflow (`MIN / -1`, `MIN % -1`) is UB like division by zero, a constant expression reports it, and the runtime `--bounds` check throws `ArithmeticError` for a non-constant divisor |
| 9 | `switch`/`match` | **Superseded 2026-11-20** (rows 137/138): `switch` is constant dispatch over integers/enums (no fallthrough, `pattern: { }`, multiple values, half-open ranges, exhaustive enums); `variant` is dispatched with `match` (expression/statement, one-level patterns, bindings, guards, exhaustive with `else`). The old "match is not retained" decision is withdrawn. **2026-11-20 (build 274): the two constructs are one — `switch` was removed and `match` takes constant patterns for integers, `char` and enums (6.6, 10.1.2).** |
| 10 | `goto`/`typedef`/function pointers/varargs | `goto` is not done (phase one); function pointers and lambdas are implemented in 3.16 (0.0.84); the `typedef` syntax is not done, type aliases are written `type A = B;` (implemented in 0.0.62, see 4.5); varargs are limited to `extern` |
| 11 | Indexed form of `for-in` / `ref` / default copy | **Decided (the indexed form is implemented)**: value binding by default; `for i, x` provides an `isize` index/ordinal (6.4.4); user types use the iteration forms of 6.4.1/6.4.2; **`for ref` works for user types in the index form when the container's `operator[]` returns a mutable `ref T`** — either spelling of the operator (member or free function, 6.4.1); it is unavailable for ranges, `string`, the cursor form, class/interface elements and `const` sequences (a `const` sequence uses **`for const ref`** for read-only iteration); **multi-dimensional arrays forbid `for-in`** |
| 12 | `..=` / `step` / reverse order for ranges | Implemented (0.0.62): `..=` includes the upper bound; `step` only follows a sequence expression (`0..10 step 2`) and must be a non-zero compile-time constant; reverse order relies on an **explicit negative step** (`a..b` does not reverse automatically); match constant-pattern ranges support `..=` and **do not support `step`** |
| 13 | Local type inference | Decided: `auto x = expr;`; locals only, an initializer is required; all declarators must have the same type |
| 14 | Default mutability of parameters | Mutable (C semantics) |
| 15 | `main` with parameters | **Implemented**: `int main(int argc, char** argv)`, `argv` is null-terminated; other signatures are errors |
| 16 | Error handling convention | Exceptions were introduced in 0.0.81–0.0.82 (3.14): unchecked, `Exception` base class, `throw`/`try`/`catch`/`finally`; return codes/`null` remain for library APIs |
| 17 | Module visibility / linkage modifiers | Implemented (0.0.30): `private`=within a unit, `public`=across units; struct members are public by default, class members are private by default; `public:`/`private:` section labels are supported; the library boundary = the first segment of the unit name, crossing libraries requires `public unit`; after `use`, public symbols can be used directly without a prefix; non-public symbols use `internal` linkage (see `visibility-design.md`) |
| 18 | Struct alignment/packed | Implemented (0.0.42): `packed` (no arguments, equivalent to `pack(1)`) and `align(N)` (a power-of-two constant, covering overall alignment and tail padding) are **keyword modifiers**, orthogonal and composable; `N` is compared against the natural alignment, and `N < natural alignment` is an error. See 3.4 for details |
| 19 | Details of the conversions between `string` and `byte[]`/`char[N]` | `string` is an immutable library type; initializing a `char[N]` from a literal is a language rule (byte copy + NUL padding, 3.3); literal → `string` is the `implicit` constructor; the remaining conversions are library APIs |
| 20 | Struct constructors / `this` | class constructors and `this` are covered in 3.11/3.12; structs still use aggregate construction |
| 21 | Whether `elif` is syntax sugar | Decided (implemented): supported, equivalent to `else if` |
| 22 | Storage and lifetime of dynamic array literals | `T[] a = {...}` allocates a new array (RC); borrowed views are not supported |
| 23 | Signedness and naming of `byte`/`char` | `byte`=u8 (the only unsigned 8-bit name, no alias); `char`=i8 (C-style signed) |
| 24 | Whether `const` requires a compile-time constant | **Redesigned (2026-11-17)**: local `const` may be a run-time value; a unit-level `const` whose initializer folds is a compile-time constant, otherwise it is a run-time-initialized `const` object (9.1). Only compile-time constants may appear in `#if`, array lengths, `step`, `align(N)` and `match` ranges. `constexpr` is not introduced |
| 25 | How to read `const int* p` | **C++ model (2026-11-17)**: `const int x` = a const object (top-level); `const int* p` = pointer to a read-only int (low-level); `int* const p` = read-only pointer (top-level, supported); `const int* const p` = both; `T** → const T**` has no implicit conversion (as in C++, `as` can force it) |
| 26 | Whether a parameter list allows multiple variables to share a type | Decided: **not allowed**; each parameter writes its own `type name` (`int f(int a, b)` is an error), see 7 for the implementation |
| 27 | Struct aggregate initialization | Declaration `S s = {x=..., y=...}`; expression `S(...)`; positional and named forms cannot be mixed; all fields must be given |
| 28 | Named-argument syntax `=` vs `:` | Use `=` first; `:` as an alternative |
| 29 | Zero-value construction `S()` / `S s = {}` | Decided: zero-initialization (all fields cleared to zero) |
| 30 | A type name colliding with a function/variable name | Conflicts are forbidden in the same scope |
| 31 | Whether function calls support named arguments | Not supported in phase one; struct construction only |
| 32 | Whether `T x(...)` is allowed in a declaration | Not provided; declarations use only `=`; aggregate zero values use `{}`, scalars use their own constants |
| 33 | Relationship between declaration initialization and expression construction | `T x = T(...)` and `T x = {...}` are equivalent; prvalues are copy-elided |
| 34 | Array/slice initialization | Declarations use `= {...}` / `= {}`; there is no literal in expressions |
| 35 | Copy elision for prvalue initialization | Constructed directly in terms of semantics; there are no user-defined copy constructors/destructors |
| 36 | A type must be declared before use | Required in phase one; forward declarations may be added later |
| 37 | The `f(void)` compatibility form | Not supported for now; `()` means no parameters |
| 38 | Return checking on all paths of a non-`void` function | Decided: mandatory (falling off the end is a compile error; `main` is an exception) |
| 39 | Unit-level variable declarations | Decided: declared directly, private by default; `public` across units; `extern` for external symbols. `static` has been removed |
| 40 | `static` functions | Removed (use `private`/`public` to control visibility) |
| 41 | `static` local variables inside a function body | Removed (not supported) |
| 42 | Unit-level variables are zero-initialized by default | Yes (static storage duration) |
| 43 | Meaning of zero-initialization for `string`/slices/arrays | Empty string / empty slice / all-zero elements |
| 44 | Scope of zero-value initialization | Aggregate types use `{}`, scalars use their own constants (`0`/`0.0`/`false`/`'\0'`/`null`); only at declaration-initialization positions |
| 45 | Brace initialization rules | Declarations only; structs must give all fields (an empty `{}` means all zeros); arrays are zero-padded when short; slices are sized by element count |
| 46 | Construction at non-declaration positions | Only `T(...)`; `{...}`, `{}` and `= 0` are not allowed |
| 47 | The empty braces `{}` | Decided: zero-initialization for aggregate types (struct/array/slice/string); writing `{}` for a scalar is an error, use its own constant |
| 48 | Copy initialization in a declaration `T x = expr` | Allowed; lvalues are copied, prvalues are copy-elided |
| 49 | `0` in expressions vs zero-value initialization in declarations | In an expression it is an int; in a declaration, aggregates use `{}` and scalars use constants |
| 50 | Language-level dynamic typing | Not a goal of phase one; `dynamic` is only reserved |
| 51 | The purpose of `any` | Reserved as a static top-level type/library type; distinguished from `dynamic` |
| 55 | Heterogeneous value scheme | **Variant types** (decided 2026-11-20; landed in 0.0.115 as "sum types"): declared with `variant Value { Int int; Text const char*; None; }` and dispatched with `match` (10.1.2); the `{ i32 tag; union payload }` layout, payload-aware value semantics and argument-checked construction are unchanged. `Option<T>`/`Result<T,E>` follow once `match` is implemented. `any`/`dynamic` stay reserved |
| 56 | The relationship between `any` and `dynamic` | `any` is for static top-level/library types; `dynamic` is for runtime dynamism; they are not mixed |
| 52 | `auto` and `const` | Decided (implemented): `const auto` is allowed |
| 53 | Multiple declarators with `auto` | All variables must have the same inferred type |
| 54 | Whether `auto` can be used at unit level | Not allowed; unit-level declarations require an explicit type |
| 57 | Bitwise NOT operator | Decided: `~` (same as C); `not` is reserved for logical negation |
| 58 | `not` precedence | Decided: low precedence, higher only than `and`/`or` |
| 59 | Set of bitwise operators | Decided: `&` `\|` `~` (NOT) `^` (XOR) |
| 60 | Exponentiation operator | Decided: no operator is provided; bitwise XOR has exclusive use of `^`; use `pow` (libc)/a library function for exponentiation |
| 61 | If `^` were the exponentiation operator, what would XOR use | Deprecated: `^` is not the exponentiation operator, XOR keeps `^` |
| 62 | `string` comparison semantics | Decided (0.0.89): library `operator==` compares byte values, with overloads for `string` and `const char*` (a literal); `!=` is derived by negating; comparing a struct value with `null` is a compile error; the zero value equals `""` |
| 63 | Floating-point literal narrowing | Decided: literals adapt to the context (`float f = 1.5;` is legal), but variables still require `as` |
| 64 | Integer ↔ enum conversions | Decided: no implicit conversion in either direction, `as` is required (an enum is not a widening target for an integer, and an integer is not an implicit source for an enum) |
| 65 | Upper limit of integer literals | Decided: `2^63-1` without a suffix; a `ul` suffix allows the full unsigned 64-bit range (`u` = uint, `l` = long); the `2^63` magnitude is accepted only directly after unary `-` (the `long` minimum) |
| 66 | Writability checks for `const` objects / const views / `string` | Decided: the compiler must reject writes (see 3.7) |
| 67 | Object model | Decided: struct values / class references; RC + weak; no cycle collector (see 3.8 / 8.3) |
| 68 | Scope of `weak` | Class fields only; locals/parameters/return values are forbidden |
| 69 | Semantics of `del` | **Removed (0.0.47)**: `del x;` is equivalent to `x = null;`, it has no separate semantics and has been deleted from the language (the keyword is kept only for migration diagnostics) |
| 70 | Ownership of class parameters | Changed (0.0.47): the parameter is an **owning local variable** (+1 on entry, -1 on exit); it can be rebound inside the function body; it can be optimized later with borrow/escape analysis |
| 71 | Interop between class and C | Passing to `extern` is forbidden; use a struct + alloc across languages |
| 72 | Arrays of class | Decided: both static `Cat[N]` and dynamic `Cat[]` are supported (RC per element) |
| 73 | `sizeof`/`alignof` on a class | Decided (implemented): a class takes the handle size (8), an interface takes the interface value size (16); the object size is not exposed |
| 74 | Multithreaded reference counting | Decided (see 94): atomic RC (relaxed increments, release decrement + acquire fence), objects may be shared across threads; the `--single-thread` opt-in build drops the atomics and rejects concurrency constructs (`para`, `para for`, `std.thread` / `std.task`) |
| 75 | User destructor `deinit` | Not provided under that name: user destructors are written `~T()` and are implemented (see 3.13 / #92); fields are released in reverse declaration order |
| 76 | Inheritance model | Decided: single inheritance + multiple interface implementations; no multiple inheritance (see 3.9/3.10) |
| 77 | Virtual methods | **Changed (build 160): static by default.** `virtual` opts a base class method into dynamic dispatch; a replacement is written `override` and requires the base declaration to be virtual (overriding a non-virtual method is an error, not silent shadowing). Rationale: methods were virtual by default, so a plain value-free class paid 3 dependent loads + an indirect call per call site, and LLVM could not devirtualize inside loops; the closed-world analysis alone (build 159) already recovers most of it, but an explicit `virtual` also lets a class member operator stay static and makes the cost visible at the declaration. Constructors are never virtual; destructors always dispatch through the class descriptor. |
| 78 | Abstract classes | Decided: having unimplemented methods makes a class implicitly abstract and it cannot be `new`ed |
| 79 | Interface default methods | Decided: allowed; on conflict the class must explicitly override (a default on a derived interface refines the base default); a `struct` implementing an interface materializes the default as an ordinary method (`this` = the struct), see 3.10 |
| 80 | Constructors | Decided: overloading is allowed; the base constructor is called from the initializer list (`: Base(args)`, 3.12, unified 2026-11-18 — the old `super(args)` first-statement form is gone); C++-style dispatch; fields are cleared to zero first |
| 81 | Overload resolution | Decided: exact > implicit widening; base-class overloads are merged; ambiguity is an error (see 3.11) |
| 82 | Operator overloading | Decided: free functions only; arithmetic/unary `-`/comparison/bitwise; a class's `==` is fixed as identity comparison |
| 83 | Object layout | Decided: `[rc][class_desc*]`; the desc contains the vtable and itable directories (see 8.3) |
| 84 | Interface value layout | Decided: `{obj*, class_desc*}`, two words; `null` is supported |
| 85 | Arrays/dynamic arrays of class | Supported; the Widget tree still recommends `first_child/next_sibling` |
| 86 | `sizeof`/`alignof` on a class | Decided (implemented): same as #73 (class=8, interface=16) |
| 87 | Multithreaded RC | Decided (same as 94): atomic RC (relaxed increments, release decrement + acquire fence); the thread-local heap scheme has been rejected; `--single-thread` is an opt-in non-atomic build that forbids concurrency (see 74) |
| 88 | Name mangling | Decided (injective): `mu$<unit>$<name>$<param>...`; components are separated by `$`, every literal `_` is doubled and every other non-alphanumeric byte is `_XX` (uppercase hex), so no two declarations share a symbol; **low-level `const` is part of the parameter type code** (2026-11-17), while top-level const is dropped from the signature; `extern` keeps the original name; `main` is special |
| 89 | Unary `~` overloading | Allowed (`operator~`, 1 parameter) |
| 90 | Static arrays of class | Decided: allowed (forbidden for structs); retain/release per element |
| 91 | `[]` overloading | Decided: only `operator[]`; return `ref T` for a writable subscript (`operator[]=` was removed in 0.0.101) |
| 92 | User destructors | Decided: `~T()`, executed when the RC reaches zero; derived runs before base |
| 93 | Exception model | Implemented (0.0.81–0.0.83): unchecked, `Exception` base class, class catch, `finally`, module-local may-throw optimization; see 3.14 |
| 94 | RC thread model | Decided: atomic RC (relaxed increments, release decrement + acquire fence), objects may be shared across threads |
| 95 | Generics roadmap | Decided + implemented (0.0.64–0.0.72): monomorphization, `<>` syntax, structural constraints, `Self`; see 3.15 for details |
| 96 | Reserved words | Decided: see 2.5 |
| 97 | Exception unwinding ABI | Resolved by B2 (0.0.81): no platform unwinding ABI; a thread-local pending slot + call-site checks; no `.eh_frame`/`.pdata`/personality dependency |
| 98 | `const` class views / `const` methods | **Redesigned (2026-11-17)**: `const` methods are provided (`ret f() const`, `this` is `const Self`, `f()` / `f() const` overload); a `const` object is deeply read-only (only const methods may be called, fields cannot be written); `const struct` / `const class` makes every field/method implicitly const without putting cv on the type name — the object's content is fixed once constructed, it can be copied and moved, and a variable of such a type can be reassigned as a whole (destroy-and-construct). `const_cast`/`mutable` are not provided (use `as` for an explicit const removal). **Refined 2026-11-18 for `const class` — see 132.** **2026-11-20: the `const struct` / `const class` modifier was removed (3.7); `const` methods and `const` fields are unchanged.** |
| 99 | `ref` parameters / return values | Implemented (0.0.62 for parameters; extended in 0.0.63): `ref` is a binding mode rather than a storable type — `ref T p` / `const ref T p` / `ref T f()`, and no `T&` is introduced; every non-union value type can be borrowed (build 267; `[ref x]` capture keeps the narrower rule); a returned borrow must originate from a `ref` parameter, `this`, a global, or pointer/slice storage, and the result is not storable (it can only be written through on the spot / passed as a borrow / used with `for ref`); borrowing a local, a by-value parameter, or a temporary is a compile error; methods support it (build 270) as part of the signature, constructors only as the copy/move form, interface members and `extern` do not (see 3.8) |
| 100 | Downcast from an `interface` to a derived `interface` | Decided: not supported in phase one (only upcasts are allowed; `as` is an error) |
| 101 | Non-ASCII character literals | Decided: not supported in phase one (a literal is a single-byte `byte`; use `'\xNN'` or `string`) |
| 102 | Enum comparison | Decided: only between the same enum; `== != < <= > >=` are supported, the order is the order of the underlying integer values; cross-enum or integer comparisons require `as` (see §3.5) |
| 103 | Qualified references to enum members | Decided: `Color.Red` is equivalent to the unprefixed `Red`; across units write `unit.Color.Red`; members are constants (not assignable, and their address cannot be taken) |
| 104 | Conditional expression `?:` | Decided (0.0.46): `cond ? a : b`, right-associative; the condition must be `bool`; only one branch is evaluated; the two sides must have the same type / implicit widening / `null` adaptation, otherwise `as` is required; the result is not an lvalue; it is one owned value (a user copy constructor runs once for a borrowed branch, build 247); see 5.2 |
| 105 | `const` on field declarations | **Redesigned (2026-11-17)**: `const` fields are allowed (`const int x;`), must be initialized by every constructor (member initializer list `C(...) : x(v) {}` or a default member initializer `const int x = 5;`), and are read-only afterwards. Type-level constants are removed; `const` inside a type body means a field |
| 106 | Floating-point `%` | Decided (implemented): legal, the semantics are LLVM `frem` / C `fmod` (the sign follows the dividend) |
| 107 | Unit-level `const` referencing other `const`s/enum members | **Redesigned (2026-11-17)**: the evaluator supports references to **already declared and imported** constants (unit-level `const`, enum members); **forward references are gone** (`const int A = B; const int B = 1;` is an error). `const int B = A + 1;` and `const int C = sizeof(int) as int;` are legal; division by zero and a not-yet-evaluated reference are errors (3.7.2) |
| 108 | Type-level `const` constants (`const` members) | **Removed (2026-11-17)**: there are no type-level constants (`Type.K` is gone); `const int K = 3;` inside a type body is a `const` instance field (3.7). Constants live at unit level; enum members keep `Color.Red` |
| 109 | Bit-fields | Decided (0.0.62): **explicitly not done**. No C-style `int x : 3;` is provided, nor is any Zig/Rust-style explicit bit-width syntax; the allocation order and cross-unit boundaries are implementation-defined, which conflicts with predictable layout. Alternatives: mask+shift / `byte[N]`+offset / `packed`+accessors / a C shim (see 3.4) |
| 110 | A generic interface as a value type / a value type as an interface value | **Split (0.0.108).** (a) *Generic interface as a value type*: **implemented** — `IIterator<int> x`, `class C : IIterator<int>`, `class Src<T> : IIterable<T>`, interface-valued returns/fields/locals, `T: IIterable<int>` constraints, `as` downcasts; two words `{obj, desc}`, itable slots keyed by the concrete signature, no boxing. (b) *Value type → interface value*: **still explicitly not done** (0.0.75 decision stands) — a struct would have to be boxed, so `Animal a = cat;` is a compile error and `struct S : I` is a compile-time contract only |
| 111 | `const` members in generic types / generic interface inheritance | **Generic interface inheritance implemented (0.0.108)**: `interface Derived<T> : Base<T>` instantiates per level and both levels are interface values of their own type. **`const` fields in generic structs/classes are ordinary fields since 2026-11-17** (type-level constants are gone), so no special wiring is needed |
| 112 | Type alias `type A = B;` | Decided (0.0.62), narrowed 2026-11-20: `type` is a **contextual keyword** (it is parsed as an alias only at the start of a declaration, `type Ident =`; elsewhere it is still an identifier, which protects the `type` field of C-interop structs); each declaration declares one alias; aliases are transparent (not newtypes) (see 4.5). `type` **declares only an alias**: a tagged union is a `variant` (10.1.1) and an interface extension is `extend` (3.18); the old `type V = A \| B;` and `type T += I;` forms are rejected with a migration diagnostic |
| 113 | Range extensions `..=` / `step` / reverse order | Decided (0.0.62): `..=` includes the upper bound; `step` only follows a sequence expression (`0..10 step 2`); reverse order relies on an **explicit negative step**, `a..b` does not reverse automatically; `step 0` is a compile error; the step size is limited to compile-time constants (see 6.4) |
| 114 | Range expansion in array initialization | Decided (0.0.62): in `int[10] a = {1, 2, 5..9, 10};` the `5..9` expands to 5,6,7,8; the endpoints must be compile-time constants; only `..` is supported inside braces (it does not take `..=`/`step`); after expansion it participates in "zero-pad when short / error when too long" (see 4.1) |
| 115 | `switch` extensions | **Superseded 2026-11-20** by the `match` decision (row 137): `switch` remains C-style constant dispatch over integers and enums, with no expression form/guards/bindings/or-patterns/type branches/destructuring/`case`/fallthrough; a `variant` scrutinee is rejected and points at `match` (6.6, 10.1.2). **2026-11-20 (build 274): superseded again — `switch` was removed; its vocabulary lives in `match`'s constant patterns, which do have the expression form and guards.** |
| 116 | Function type syntax | Decided and implemented (3.16, 0.0.84): `(T1, T2) => R`; a one-word pointer value with no `*` and no declarator nesting; `sizeof` = pointer size; `=>` has lowest precedence and is right-associative; `*`/`[]` bind to the return type; block-/unit-level declarations never begin with an anonymous function type and a return type is never anonymous (use `auto` or a `type` alias), while fields and parameters may |
| 117 | Lambdas | Decided and implemented (3.16, 0.0.84): `(int a) => expr` or `(int a) => { ... }`; parameter types are always written; the return type is inferred from the body (all `return` types must be identical; no `return expr` means `void`); **no captures**; each lambda is a hidden `private` function (C ABI, no allocation); stateful callbacks use `class` + single-method `interface` |
| 118 | Free function / method values | Decided and implemented (3.16, 0.0.84): a free function name is already a function value (no `&`, and `&f` is an error); overloaded names need a target type; generic function names cannot be used as values; `u.f` and `extern` functions are values; **methods are not values** |
| 119 | Capturing closures | Decided: phase-one non-goal; if added later, a separate two-word `delegate` type (`{code, env}`), never a fatter `fn`. **The type landed in 0.0.109** (3.16.1a): layout, calling convention `code(env, args...)`, the `fn → delegate` thunk, RC of the environment and the `extern` prohibition are in place; capture analysis and the synthesized environment object are the remaining steps (backlog U41) |
| 120 | Concurrency model | Decided (design only, 3.17): **structured** — M:N tasks on carrier threads, stackful fibers, fixed task stacks, cooperative cancellation, no preemption, no detached tasks. Language surface: `para` / `para for` / `thread`; deferred join is library-only |
| 121 | `async` / `await` | **Decided: not provided.** Colored async functions are rejected; a blocking call parks the task, so no call-site marker is needed. Deferred join is a library task group plus a one-slot `Channel<T>` |
| 122 | Timeouts / concurrency caps in `para` | **Decided: not provided.** A timeout can only be enforced at a cancellable IO operation (`conn.read(buf, ms)`); a concurrency cap is an engine resource (pool size) or a semaphore, never a per-region syntax parameter |
| 123 | Detached tasks (`nowait`) | **Decided: not provided.** A task with no joiner breaks the frame-lifetime rule that makes capture-by-static-link sound; long-lived work is a service task driven by a channel |
| 124 | `thread` in `para` | Contextual keyword (2.3): run the section/item on the **bounded compute pool** — a dedicated OS thread, may block, OS stack. It is an intent hint, not a checked property |
| 125 | Single-task `para` (`s = para f()`) | **Decided: not provided.** In a stackful model the call itself is the suspension, so "start then immediately join" is pure overhead; use the library task group when the join must be deferred |
| 126 | Unit-level initialization and cleanup | **Implemented (9.1, 0.0.102).** Runtime initializers are allowed; the order is a DFS post-order over `use` edges from the entry unit (declaration order inside `use` lists), cleanup is its exact reverse, declaration order inside a unit. **The same sequence orders compile-time constant evaluation and `#if` selection (decided 2026-11-17, 9.2).** Only `use` dependencies are tracked; indirect dependencies (function pointer, interface dispatch, `extern` C symbol, a later variable reached through a function body) are undefined. An exception escaping an initializer aborts without cleanup; `atexit` makes `exit()` clean up. Lifted the three "no exit-time destruction" restrictions (3.3 `String`, 3.4 reference fields, 3.4 user destructors). Deferred: lazy first-use initialization, explicit `init(N)` priorities, debug-mode lifecycle assertions, link-time sequencing for split compilation |
| 127 | `export` and the binary export surface | **Decided and implemented (0.0.103).** `export` is a top-level declaration modifier that implies `public` and marks the declaration as part of the binary export surface; it cannot be combined with `private`/`extern`/`use` or used inside a type body (export the type, which exports its public methods and class metadata). `--exports=export` (default) keeps only `export` declarations externally visible, `--exports=public` keeps every `public` declaration; `--export <name>` still adds individual names. In an executable everything outside the surface is internalized and dead-code-eliminated; library targets keep the surface visible (toolchain plan §11) |
| 127 | Member promotion (Go-style embedding) / mixin | **Revised 2026-11-20**: field method promotion is provided in the explicit, method-only form `promote field;` / `promote field(f, g);` inside a type body — only the field's **public methods** are forwarded, only one level, after the type's own/base/interface methods, never participating in `override` or satisfying an interface, with conflicts reported at the type declaration (3.19). The general `mixin`/`embed` member form (which also promotes fields and falls back for every call) remains **not done**: it cannot satisfy an interface without an offset thunk (identity and `this` change meaning), and the callable set would depend on another type's evolution across units, violating the "information must be locally expressible" criterion. Reuse for other cases: `struct S : I` with explicit forwarding methods (3.10, 3.15.7) or field composition. Background: `agents/design-backlog.md` U40 |
| 128 | Signed or unsigned index / length type | **Decided (2026-11-16): `isize`.** `length()`, subscripts, `for i, x` ordinals, `find` offsets, `sizeof`/`alignof`/`offsetof` and the `alloc`/`realloc` byte counts are all `isize`; `usize` is kept for unsigned arithmetic and for C-facing sizes (`cstd.*`, `c_size`), where an explicit `as` is required. Rationale: `length() - 1` on an empty sequence must not wrap to `2^64-1`, descending loops need a signed counter, `find` already returns `-1`, and the emitted bounds check (`icmp uge` on the 64-bit index) catches a negative index with no extra instruction |
| 129 | `const` redesign (C++ model) | **Decided 2026-11-17** (for `const class` refined 2026-11-18 — see 132): cv is part of the type at every level; top-level const is ignored in signatures (and a top-level `const` return type is accepted but ignored), low-level const participates in overloads and mangling; `const` fields and `const` methods; `const struct` / `const class` is a declaration modifier without cv on the type name (content fixed after construction; copy/move allowed; whole-variable reassignment allowed); `T* const` is supported; `T** → const T**` has no implicit conversion (`as` can force it, as in C++); a const object is deeply read-only; `const_cast` is replaced by `as`; no `constexpr`/`consteval`/`constinit`, no `mutable`/`volatile` (3.7). **2026-11-20: the `const struct` / `const class` declaration modifier was removed; everything else in this decision stands.** |
| 130 | `move` is `&&` | **Decided 2026-11-17**: `move T` is a non-const rvalue reference; `const` and `move` cannot qualify the same parameter; `move` on a const object is an error (no silent copy); used in move construction and transfer positions (3.4, 3.8, 4.4) |
| 131 | `std.comptime` unit / `builtin` | **Decided 2026-11-17, unit renamed to `std.comptime` 2026-11-18**: an ordinary unit shipped with the toolchain; `builtin` declares compiler-provided constants/functions with no initializer and no body; any unit may declare one, but only names in the compiler's built-in table are accepted and a name may be declared once; explicit `use std.comptime` (no implicit visibility); replaces `std.target` (9.3) |
| 132 | `const class` inheritance and views | **Superseded 2026-11-20: `const class` was removed (3.7); this row records the model as it stood.** **Decided 2026-11-18**: a `const class` is a **leaf type whose content is fixed**, reached only through read-only views. It can never be a base class (`class E : D` and `const class D : const Base` are both errors), but it **may** derive from a mutable class base (`const class D : Base`); that subobject is written by the base constructor and by `D`'s own constructors/destructor — **construction is the only writable phase** (after a constructor returns, neither outside code nor the class's methods write). Every expression of the type is a read-only view (a class handle is already a shared reference), and converting it to any mutable view (base handle, interface handle, `ref`) needs `as` (writing through it is UB). Methods are implicitly `const`; a non-`const` method exists **only as an override slot** for a non-`const` virtual of the base — read-only body, not part of the public surface (`private` by default), not callable by name from outside, only from `D`'s constructors/destructor. Consequence accepted: an inherited non-`const` virtual then has **no caller** (external code holds only read-only views, method bodies cannot mutate, and a base constructor's own virtual call does not reach a derived override) — the override still keeps the vtable complete and turns a would-be write into a no-op, which is what makes inheriting a base with mutating virtuals usable at all. Rejected: banning class bases outright (loses implementation reuse for no extra safety), `const class D : const Base` (an extra constness-agreement rule for a case composition covers), and letting internal method bodies keep a mutable `this` (that makes the type only *outward* read-only — a second, weaker promise than `const struct`). Related: 3.7 |
| 133 | Construction prologue: base call, delegation, field initializers | **Decided 2026-11-18**: one place for every construction step — the initializer list after the parameter list. An item is `Base(args)` (base constructor), `CtorName(args)` (delegating constructor) or `field(value)`; execution order is fixed as **delegate → base → members → field initializers in declaration order → body**, independent of the order the items are written in. `super(...)` as a constructor call is **removed** (it was a body statement that had to be hoisted to the prologue anyway, which forced a "must be the first statement" rule and made "base constructor with arguments + own `const` field" unwritable); `super.m(...)` stays as the qualified base-method call. Delegation cannot also name the base or a field, cannot target itself, and chains must not cycle (3.12) |
| 134 | Null references: local initialization and the null check | **Decided 2026-11-19**: (a) a class/interface local must be initialized (4) — the type *is* the handle (3.8), so `Dog d;` must not read like a stack construction that builds nothing; (b) dereferencing a null handle (method call, field read, field write) throws `std.exception.NullReferenceError`, and `--null-check=off` drops the check, making the access UB (8.2); `null` itself stays a legal value. Rejected: leaving an uninitialized handle silently null (the declaration looked like construction and the failure moved to an unrelated line), and leaving the dereference purely UB with no diagnostic (a segfault instead of a catchable error) |
| 135 | C-style `for (init; cond; step)` | **Decided 2026-11-19: implemented** (6.3.1), superseding the earlier "no C-style `for`" decision — an index loop is the common case and `while` + a manual step reads poorly. Semantics are C's: every clause is optional (`for (;;)`), `cond` must be `bool`, and **`continue` runs the step clause**. `init` is a declaration or one expression statement; since the language has no comma operator, `for (a = 1, b = 1; ...)` is rejected — declare both in `init` instead. `do-while` remains not part of the language |
| 136 | Declaration split: `type` / `variant` / `extend` | **Decided 2026-11-20, implemented in build 258 for `type`/`variant`; `extend` pending**: `type` = transparent alias only (4.5); tagged unions = `variant` (10.1.1); interface extension = `extend I with J;` (3.18). Old `type V = A \| B;` and `type T += I;` are rejected with migration diagnostics |
| 137 | `match` (first version) | **Decided 2026-11-20, implemented in build 258**: `match` dispatches a `variant`; expression and statement forms; one-level patterns `Alt(x, y)`, value/`ref`/`const ref`/`match move` bindings, guards, mandatory exhaustiveness with an optional last `else`. Type patterns, nested patterns, or-patterns and public-contract branches are later phases (10.1.3) |
| 138 | `switch` domain | **Decided 2026-11-20, implemented in build 258**: integers and enums only; a `variant` scrutinee is a compile error pointing at `match`. Whether integer/enum dispatch eventually folds into `match` stays open and is re-evaluated after `match` ships. **Resolved 2026-11-20 (build 274): it folded into `match`; `switch` was removed.** |
| 139 | Interface extension `extend I with J;` | **Decided 2026-11-20, implemented in build 261**: `J : I`, default methods only; applies to implementors of `I` in units that can see the extension; conflicts are ambiguity errors; it is not a subtype relation (3.18). The concrete-type `extend T with I;` form is planned |
| 140 | Field method promotion `promote` | **Decided 2026-11-20, implemented in build 260**: only public methods, one level, fixed precedence (own → base → interface → promoted), conflicts at the type declaration, no `override`/interface satisfaction, generic resolution per instantiation (3.19) |

---

## 13. Examples

See [`examples/example.mu`](../examples/example.mu) and [`examples/math.mu`](../examples/math.mu). The examples correspond to this draft and are not guaranteed to compile.

---

## 14. Implementation Roadmap (non-normative)

- **Phase A**: generate C99 source, compile it with clang/cl, and quickly validate the semantics and the runtime.
  - Status: implemented on 2026-11-08 and used for validation; **removed as of 0.0.2** (the maintenance cost of two backends is too high),
    the code is kept in the git history at `cf3af48` and can be retrieved for differential comparison if needed.
- **Phase B**: generate LLVM IR; on Windows use COFF + `lld-link`, gradually getting rid of the C toolchain.
  - Status (2026-11-08): implemented (`src/codegen_llvm.cpp`, the LLVM C++ API directly produces COFF object files),
    and it is the only backend at present.
  - gcc is still used for linking; the optimization pipeline, debug info and `lld-link` are not hooked up yet.
- **Object model** (class/inheritance/interfaces/overloading/constructors/operators) has completed its design, front-end semantics and **LLVM code generation**:
  - 2026-11-09: type model, registration/inheritance/field flattening, method/constructor/destructor checks, `this`/`super`, `new`/`del`,
    overload resolution, operator dispatch (including unary and `[]`/`[]=`), interface default method conflicts, abstract class deduction, inheritance cycle checks, `weak` fields.
  - 2026-11-09 (0.0.3–0.0.13): object header/desc/vtable/itable, construction and destruction chains, atomic RC, cleanup of local references,
    per-element RC for reference arrays, `as` downcasts (desc base-class chain), release of temporaries; 41 end-to-end test cases.
  - **Not supported in phase one**: `const_cast` / `mutable` / `volatile` (use `as` for an explicit const removal, 3.7), `ref` parameters, downcasts from an `interface` to a derived interface (see 12 table 98–100);
    cross-thread RC stress validation will follow.
  - See `object-model-plan.md` for the feature matrix and the deviations.
- **Exceptions**: `throw`/`try`/`catch`/`finally`, class matching and RC cleanup are implemented in 0.0.81–0.0.82 using the B2 lowering (3.14); 0.0.83 adds a conservative module-local may-throw optimization with external/unknown targets always conservatively checked. No platform EH ABI is involved.
- **Generics**: monomorphization is implemented (3.15, 0.0.64–0.0.72). Instantiation = clone the AST + substitute the type parameters + canonicalize the name,
  so the backend does not need to be aware of generics; the remaining item is generic interface inheritance.
- Manual memory maps directly onto libc `malloc/free/realloc`; the runtime only needs a small amount of entry/support code.
- There is no need for GC or stack maps; exceptions use a TLS pending flag rather than platform unwind tables, so the backend keeps its libc-only portability.
- `for-in` degradation: ranges → index loops; arrays/slices → index loops with a cached length; the target of `continue` must point at the "advance point".
- `const` single assignment and `const` not participating in code generation are both front-end/optimization-friendly facts.
- Declaration parsing: after `Type name` only `=` or `;` follows; a function is `Type name(params) { ... }`, so there is no need to distinguish whether `T x(...)` is a variable or a function.
- Initialization degradation: `= expr` copies/directly constructs; `= {...}` performs aggregate initialization; `= {}` compiles to the zero value (structs/arrays/slices/`string` are recursively cleared/emptied).
- prvalue initialization constructs the target directly, avoiding an extra copy.
- Unit-level variables are zero-initialized by the loader/startup code; unit-level initialization and
  cleanup (9.1) are a compiler-generated call sequence, not a `.init_array`/linker decision.
- `auto` only takes the expression type directly in the front end; multiple declarators require identical inference results; no runtime overhead.
- Dynamic typing is not in phase one; heterogeneous value tagging uses the language's `variant` + `match`; `dynamic` acts as a separate runtime layer rather than a core type.
- **Concurrency** (3.17): design settled, not implemented. Order: (0) RC memory ordering (release decrement
  + acquire fence before destruction) and the weak-registry lock ordering; (1) runtime primitives (threads,
  mutex/condvar, monotonic clock); (2) `para` / `para for` with one OS thread per section first, plus the
  cursor form of `for-in`; (3) the M:N task runtime (carriers, park/unpark, handoff);
  (4) pollers (IOCP / epoll / kqueue) and the server library. The `para` lowering targets runtime
  primitives, so switching from OS threads to M:N does not change the source.





