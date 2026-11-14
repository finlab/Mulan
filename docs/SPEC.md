# Mulan Language Specification (Draft v0.2)

> Date: drafted on 2026-11-07; the object model (class / single inheritance / interface / overload / constructor) was added on 2026-11-09;
> 2026-11-14: aligned with the current implementation — string literals are `const char*` (0.0.89), `len` is
> removed and static arrays are C storage (0.0.101), unit-level init/cleanup (9.1), `export` and the export
> surface (0.0.104), arrays decay to pointers (C99, 0.0.106).
> Status: draft, current through version 0.0.106. The text distinguishes "decided", "recommended", and "undecided"; items that are not frozen can be discussed one by one.
> Positioning: a compact, modern, C-like small language; it retains the core capabilities of C; manual memory management + reference counting for heap objects.
> Non-goals for phase one: multiple inheritance, closures, GC, a preprocessor, reflection. (Generics have landed, see 3.15; exceptions have landed, see 3.14; destructors are specified in 3.13.)

---

## 0. Summary of Discussion Conclusions

| Topic | Conclusion |
|---|---|
| Loops | Only `while` and `for-in` are kept; C-style `for` and `do-while` are not kept |
| Iteration | `for x in seq { }` (parentheses optional); ranges `a..b` / `a..=b` (optionally `step n`) and managed sequences (`Array<T>` written `T[]` / `buffer<T>` / `String`) are supported; static arrays are C storage and are not iterable; `continue` proceeds to the next iteration |
| Mutability | C#-style: the type is declared before use; mutable by default; `const` is a C-model prefix qualifier: on named types it is a read-only binding marker, on pointers/slices it makes the pointee read-only |
| Declaration initialization | Declarations use only `=`: `Point p = {1, 2}`, `Point p = {}`, `Point p = Point(1, 2)`; in non-declaration positions use `Point(...)` |
| Type inference | Local variables may use `auto x = expr;`; local only, and an initializer is required; `dynamic`/`any` are reserved names only |
| Dynamic types | Not done in phase one; heterogeneous values prefer library-level Variant/sum types; `dynamic`/`any` are reserved only |
| Scope | Declarations do not write `T x(...)`; `Point p` is uninitialized, `= {}` zero-initializes; unit-level variables are declared directly and are `private` by default |
| Unit initialization | Unit-level variables may be initialized at runtime and are destroyed at exit (9.1, implemented 0.0.102); the order is a DFS post-order over `use` edges from the entry unit, cleanup is its exact reverse, declaration order inside a unit; only `use` dependencies are tracked |
| Object model | Values and references are separated: `struct` is a value type; `class` is a reference type (single inheritance + interfaces; virtual methods by default; atomic RC + weak; no GC/cycle collector) |
| Strings | `string` (a value type provided by the std library: an immutable UTF-8 byte sequence; for layout and API see the library documentation) |
| Generics | **Monomorphization** (one copy of the code per combination of type arguments); `<>` syntax; constraints use `interface` + `&`/`\|`; `Self` refers to the implementor; constraints are matched **structurally**, and built-in types satisfy them at zero cost; see 3.15 for details |
| Memory | Pointers + `alloc/free/realloc` (raw memory); `class` uses `new` + atomic reference counting (write `x = null` to release early); no GC, no borrow checking |
| Control flow | `if/else`, `while`, `for-in`, `break/continue/return`; conditions must be `bool`; braces are mandatory |
| Conditional expression | `cond ? a : b` (right-associative; the condition must be `bool`; only one branch is evaluated; both sides use the same type / implicit widening / `null` adaptation) |
| Condition parentheses | Optional; the canonical form omits them (`if cond { }`); C-style `if (cond)` is still legal |
| Struct construction | `Point(x = 1, y = 2)` with named arguments, or `Point(1, 2)` with positional arguments; the two cannot be mixed |
| C capabilities | Pointers, structs, unions (`union`), arrays, functions, enums, C ABI interop; `goto` is not provided; varargs are `extern`-only; function pointers/lambdas per 3.16 |
| Backend (non-normative) | The LLVM C++ API generates COFF directly; the C99 backend was removed in 0.0.2; Windows x64 is a first-class target |

---

## 1. Design Principles

1. **Keep the hard capabilities of C**: value semantics, pointer arithmetic, manual memory, struct, C ABI.
2. **Drop C's historical baggage**: no preprocessor, no implicit narrowing, no integer truthiness, no `do-while`, no C-style `for`; `++/--` keep only the statement form (no value).
3. **Explicit beats implicit**: use `as` for type conversions; use `const` for mutability; use a read-only `interface` for read-only API boundaries.
4. **Small and predictable**: generics only do monomorphization (no erasure, no runtime generics), no capturing closures, no hidden allocations; overloading is limited to the bounded rules in 3.11; errors that can be caught at compile time are not deferred to runtime.
5. **Leave room for optimization without sacrificing semantics**: single assignment for `const`, a structured `for-in`, and future alias annotations.
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
  the portable unit. Inside a shadow directory the file keeps the **full** unit name, so a library
  ships its own per-OS implementations next to its code and stays vendorable as one directory:
  `libs/std/platform/windows/std.thread.backend.mu`, `ms/xxlib/platform/linux/ms.xxlib.io.mu`.
  `platform` is reserved for this: a unit name may not start with it, and a `platform.<os>` segment
  pair is rejected. Two files for the same unit in one directory is an error (ambiguous unit).
  `muc check --os <os>` resolves the program against another platform's units, on any host.

### 2.2 Identifiers

- `[A-Za-z_][A-Za-z0-9_]*`, case-sensitive; non-ASCII identifiers are not allowed in phase one.

### 2.3 Keywords

```
unit use public private export const auto struct enum extern class interface new weak packed union
if else while for in switch ref break continue return
try catch throw finally
true false null as sizeof alignof offsetof alloc realloc free
void bool byte char short ushort int uint long ulong
float double usize isize
and or not override operator super this
```

> `del` was removed in 0.0.47 (it is kept as a keyword only to give a migration diagnostic): to release a reference, write `x = null;`.

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
where  move  typeof
```

- Generics: `where` (reserved; constraint syntax currently uses `<T: I>`, see 3.15).
- Others: `move` / `typeof`.
- **Released from the reserved list**: `async` / `await` are decided **not** to be provided (see 10); `shared` / `atomic` are not language features (sharing is explicit and atomics are a library intrinsic, see 3.17.1); `spawn` is released as well — deferred join is a library facility (`TaskGroup` + a one-slot `Channel<T>`), not syntax; `para` became a **keyword** in 0.0.92 (3.17.2); **`unsafe` is released**: there is **no unsafe mechanism** (principle 7 in 1) — every facility is available to every programmer, so the marker has nothing to protect and would only be decoration; the C-level surface stays documented UB (8.2).
- `try` / `catch` / `throw` / `finally` (exceptions) are **implemented keywords** listed in 2.3.
  > `match` is no longer a reserved word: the only dispatch construct kept is `switch` (see 6.6). **Pattern-matching style extensions are decided not to be done**
  > (expression form / guards / bindings / or-patterns / type branches / destructuring, see 6.6); if sum types later need dispatch, a separate construct will be discussed then.

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
| `usize` / `isize` | pointer width | platform-dependent; used for sizes and indices |
| `string` | defined by the library | value type of the std library: UTF-8 byte sequence, immutable; for layout/`sizeof`/API see the library docs |
| `void` | — | no value; usable only as a return type or as the base type of `void*` |

- Phase one introduces no `wchar`, `decimal`, or `i8/u8`-style aliases; `decimal` comes later as a library implementation.
- All type widths are fixed; `usize/isize` is the only platform-dependent type; library types (such as `string`) do not promise a fixed `sizeof` (see the library docs).
- `void` cannot declare variables, fields, parameters, or array elements; `void*` is legal; `sizeof(void)` / `alignof(void)` are illegal.

### 3.2 Pointers

- Types: `T*`; `void*` is the untyped pointer; `T**` is legal.
- Operations: `&expr`, `*p`, `p[i]`, `p + n`, `p - n`, pointer comparison, pointer subtraction, and the
  member shorthand `p->f`, which is exactly `(*p).f` (one dereference level; `p->f` and `p->f[i]` chain like
  postfix operators). `p.f` on a pointer is an error — write `p->f`, or `(*p).f` explicitly.
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
- **Dynamic arrays**: `T[]` / `T[,]` / `T[,,]` are **syntax sugar for the container types**
  `Array<T>` / `Array2d<T>` / `Array3d<T>` (rank 1–3; decided 2026-11-14, containers themselves
  still to be written — backlog U38). The sugar names a *library* container; it is not a distinct
  runtime type.
  - **Implementation status (0.0.106)**: until `Array*` lands, `T[]` and `buffer<T>` resolve to the
    **same** built-in reference-counted sequence (`{ i64 rc; i64 len; i64 cap; data... }`), so every
    rule below describes that shared representation. When `Array` lands, `T[]` means `Array<T>`
    (a library type over `buffer<T>`) and `buffer<T>` keeps its own spelling.
  - `new T[n]` creates one (`n` is a runtime integer, elements zeroed); `T[] s = {}` means empty (null handle).
  - Assignment/argument passing copies the **handle** (O(1)), sharing the same array; destroyed when the reference count reaches zero;
    if the element type holds class/interface references, release element by element.
  - Element types: scalars, enums, pointers, structs (including reference fields), class/interface references;
    structs with user destructors are not yet allowed as elements (element construction/destruction rules are undecided).
  - `s.length()`: 0 for a null handle; `s[i]` indexing (bounds-checked, see below); `for x in s` iterates over the elements.
  - `for ref x in s` can write to a shared array; **not allowed when the element is a class/interface** (the element is a handle, iterate with value binding);
    `string` also cannot use `for ref` (immutable).
  - Null value: `T[] s = null;` and `T[] s = {};` are equivalent (both null handles); but **a slice cannot be compared with `null`**
    (use `s.length() == 0`).
  - Releasing a dynamic array: setting the handle to empty (`s = {}` / `s = null`) releases it once; `del` was removed (0.0.47).
  - `sizeof(T[])` / `sizeof(T[,])` = 8 (handle size); the size of library types is defined by the library (for `string` see the library docs).
  - Aggregate initialization `T[] s = {1, 2, 3};` allocates a new array and initializes the elements.
  - `T[N]` → `T[]` implicit conversion: **allocates and copies** (dynamic arrays no longer provide a borrowed view).
  - Sub-slicing/borrowed-view syntax (`s[1..]` etc.) is not provided yet.
- **`buffer<T>` is the built-in safe memory block** — the low-level infrastructure the `Array`
  family is built on, not a user-facing container. Zero-initialized, RC-shared, element-wise RC for
  class/interface elements, bounds-checked subscript, `b.ptr` for C interop. It stays lowercase
  because it is compiler-provided (style.md §1); `T[]` sugar never denotes it.
- **`String` is the canonical name** of the library string type; `string` is a `type` alias (0.0.101).
- **Static rectangular multi-dimensional arrays**: `T[m,n,...]` (`m,n` compile-time constants), contiguous C storage.
  - `int[2,3] a = {{1, 2, 3}, {4, 5, 6}};` (nested braces, missing elements filled with zero).
  - Indexing `a[i,j]`, equivalent to the chained form `a[i][j]`; a partially indexed `a[i]` is a row
    **lvalue** of type `T[n]` that decays to `T*` (it is not copied; assignment of the row is an error).
  - The dimensions are derived with `sizeof`: `sizeof(a)/sizeof(a[0])` is dimension 0, `sizeof(a[0])/sizeof(a[0][0])` is dimension 1, and so on.
  - May contain class/interface/struct reference elements (RC element by element); `sizeof` is the total byte count.
- **Dynamic rectangular multi-dimensional arrays**: `T[,]` (only the rank is written; the size is determined by `new`) —
  sugar for `Array2d<T>` (see the dynamic-array bullet above), same for `T[,,]` / `Array3d<T>`; 4+ dimensions
  have no sugar (use `Array<T>` with a manual index).
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
- Indexing `x[i]` / `x[i,j]`; the index type is an integer, `usize` recommended.
- Managed sequences answer `x.length()` (`usize`); **static arrays are C storage and have no `length()`** - derive the count with `sizeof(a) / sizeof(a[0])`.
- **Bounds checking**: managed sequences (`T[]` / `buffer<T>`) are checked at the subscript; `--bounds=on|off` (default on) controls it, and a violation throws `std.exception.BoundsError { index, length, axis }`. Static arrays and `T*` are unchecked (C semantics).
- All-zero/null values use `{}`: `int[5] a = {};`, `int[] s = {};`, `string s = {};` (aggregate types no longer support `= 0`).
- **`String` is a value type provided by the std library** (`string` is a lowercase `type` alias; not a language built-in type; for layout, buffer management, and the full API see
  `lib-std-string.md`). **The compiler knows nothing about its layout** (0.0.89):
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
  - Size: `sizeof(string)` is defined by the library (currently 24 bytes: `byte* p; usize n; byte[] owner`);
    to pass strings across the ABI use `const char*`.
  - **A `string` may be a unit-level variable** (9.1): it is a struct with reference-counted fields
    and is destroyed by the exit sequence. A `const` unit-level `string` is still an error, because a
    unit-level `const` must be a compile-time constant; write `string G = "hi";` (mutable binding),
    or use `const char* G = "hi";` and convert at the use point.
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
- Can declare **type-level compile-time constants**: `const int K = 3;`—occupies no instance storage, does not affect layout, accessed as `Point.K`, see 3.7.1.
- Can declare **constructors/destructors**:
  - `S(...)` constructors can be overloaded; `S s;` calls the parameterless constructor; `S s = S(args);` constructs in place;
    with a constructor present, non-empty aggregate initialization is forbidden; use `{}` for the zero value (no constructor call, just zeroing);
  - `~S()` is called automatically at scope exit and on `return`/`break`/`continue` (local variables);
- No inheritance, no virtual functions, no anonymous struct/union **members** (unions themselves see 3.4.1).
- Recursion only through pointers: `struct Node { int v; Node* next; }`.
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
    **structs with user destructors are the exception**—they can only be used through pointers anyway (see the "user destructors" item below).
  - Structs with reference fields may be unit-level variables (they are destroyed by the exit
    sequence, 9.1);
  - `extern C` signature recursive check: structs containing class/interface cannot be C parameters/return values;
  - Structs with **user destructors** still cannot be copied/assigned/passed/returned by value, nor be array elements
    or fields of other structs/classes (the interaction between user destructors and automatic field RC is to be implemented later); pass them through pointers in those positions.
    They may be unit-level variables (9.1);
- `sizeof(S)` is available; pure-value struct assignment has byte-copy semantics, and copies of structs with reference fields get RC operations inserted by the compiler.
- Fields also support multiple names sharing a type: `int x, y;`; the type-first fixed-size array form is also allowed, e.g. `char[20] name;`.
- **Layout modifiers `packed` / `align(N)`** (**implemented, 0.0.42**):
  - Both are **declaration modifiers** (in the same class as `public`/`private`/`const`), written before the `struct` declaration (equally applicable once `union` lands,
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
- Named constants only, not a sum type; `switch` performs a compile-time exhaustiveness check on enums (see 6.6).
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
  - `T*` → `void*`; `T*` → `const T*`.
  - `T[]` → `const T[]`; the reverse (removing const) requires an explicit `as`.
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

### 3.7 Mutability: `const` (C model)

- `const` is a **prefix qualifier**; the position where it applies determines its meaning:
  - `const` on a named type (scalar / enum / struct / class) is a **read-only binding marker** (constrains the binding, does not enter the type):
    `const Cat c`, `const S s`, `const int n` all make **this binding** read-only;
  - `const` on pointers / dynamic arrays qualifies the **pointee**: `const T*`, `const T[]` are read-only views;
  - A read-only pointer binding itself (C's `T* const`) is **not provided**, see below.
- Mutable by default; `const` bindings must be initialized.

```c
int x = 0;              // mutable
const int y = 1;        // y cannot be assigned
const int* p = &x;      // *p is not writable; p can be retargeted
const int[] a = ...;    // a[i] is not writable; a can be reassigned
const int** s;          // **s is not writable; *s (the intermediate pointer) is writable
```

- `const` applies to the **base type (innermost)**: `const int**` reads as "pointer to pointer to const int".
- Only relaxing, never tightening: `int* → const int*` is implicit; `const int* → int*` requires an explicit `as` (unsafe).
- A `string` no longer converts to a pointer implicitly: use `s.cstr()` (3.3).
- `const` on a named-type binding constrains **this binding** only; it is not a read-only view and does not change the type:
  - Local / parameter: cannot be rebound (`c = null` is an error), and fields or elements cannot be written directly through it (`c.a = 1`, `s.x = 1` are errors);
  - a `class` handle can still be copied out and written through as usual; through a `const` binding **methods that modify the object can also be called**—the language provides no `const` methods (see 3.11, 12 table 98);
  - when a truly read-only API boundary is needed, express it with a read-only `interface` (split the read-only methods into an interface and let the class implement it).
- Passing a struct by value is a copy: a `const S` parameter protects the copy inside the function body and does not affect the caller.
- **Instance fields cannot be declared `const`**: there are no "read-only fields", and no "`readonly` members assigned only in the constructor" are provided.
  Inside a type body `const` has only one meaning—**type-level compile-time constant** (see 3.7.1);
  a form without an initializer such as `const int x;` is an error (``const member `x` must be initialized``).
  Earlier versions silently accepted it with a half effect (`&s.x` unexpectedly became `const int*`, while `s.x = 2` was still legal); converged together since 0.0.59.
- Enforced checks: assigning to a `const` binding (local / parameter / global constant), writing fields or elements directly through a `const` binding,
  writing the target through `const T*` / `const T[]`, using `for ref` on a `const` slice.
- **`T* const` (read-only pointer binding) is not provided**: it is only a defensive constraint, used rarely, and offers no protection to the caller when passing by value; when a fixed pointer is needed, use a local read-only variable or a naming convention. A read-only intermediate level in a multi-level pointer is likewise not expressible.
- `const` does not participate in layout/ABI and is **not an optimization promise**; there are no backdoors such as `const_cast`/`mutable`.
- `const` does not require compile-time evaluation; when a true compile-time constant is needed, use a **type-level `const` member** (3.7.1) or a constant expression (3.7.2),
  no `constexpr` keyword is introduced.

### 3.7.1 Type-level compile-time constants (`const` members; implemented 0.0.59)

```c
class Cat {
public:
    const int MaxAge = 20;
    const int Twice  = MaxAge * 2;      // references other constants of the same type
    const int W      = sizeof(int) as int;
    int age;
    int limit() { return MaxAge + W; }  // can be written bare inside method bodies
}
int x = Cat.MaxAge;                     // access qualified by the type name
```

- Syntax: `const T NAME = <const-expr>;` inside a `struct` / `class` body—must be initialized, one name at a time
  (`const int a = 1, b = 2;` is not supported; use two declarations).
- Semantics: **type-level constants that occupy no instance storage**—they do not enter the field table and do not affect `sizeof` / `alignof` / layout / ABI,
  do not participate in construction, destruction, or RC; and generate no runtime initialization code.
- Types: scalars (`bool` / `byte` / `char` / `short` / `ushort` / `int` / `uint` / `long` / `ulong` /
  `float` / `double` / `usize` / `isize`) or enums. `string` / struct / class / pointers / slices / fixed-size arrays are not supported.
- The initializer must be a **compile-time constant expression** (3.7.2); it cannot call functions or reference local variables or non-const global variables.
- Access:
  - `Type.NAME` (the same resolution as the enum member `Color.Red`)—across units write `unit.Type.NAME`;
  - inside the methods / constructors / destructors of the type that declares it, `NAME` can be written bare;
  - lookup follows the base class chain: both `Derived.NAME` and a bare `NAME` inside a derived-class method body resolve to base class constants;
  - `obj.NAME` and `this.NAME` **are compile errors** (constants occupy no instance memory), with a hint to use `Type.NAME` instead.
- Read-only: cannot be assigned, cannot have its address taken, cannot be a `ref` / `for ref` target.
- Visibility: follows the `public:` / `private:` sections and the member prefix (class defaults to private, struct defaults to public);
  accessing a private constant across units is an error.
- Name clashes: sharing a name with a field / method / constructor / another constant is a compile error.
- Not allowed inside `union` / `interface`.
- "Read-only member variables assigned only in the constructor" are explicitly **not supported**, and there is no `readonly` keyword; when per-instance read-only is needed,
  use a `const` binding (`const S s = ...;`) or encapsulation (private fields + methods).

### 3.7.2 Compile-time constant expressions (implemented 0.0.59)

Usable for unit-level `const`, type-level `const` members, and enum member values:

- Literals: integer, floating point, character, boolean;
- Unary `-` `+` `not`; binary `+ - * / %`, `& | ^ << >>`, `< <= > >= == !=`, `and` `or`;
- `as` conversions (the same truncation / sign extension / integer↔float conversion semantics as at runtime);
- References to other constants: unit-level `const` of this unit, enum members (`Color.Red` or a bare `Red`),
  type-level `const` members (`Type.NAME`); **forward references and arbitrary declaration order are supported** (`const int A = B; const int B = 1;`);
- Built-in `sizeof(T)` / `alignof(T)`: the result is `usize`; assigning it to `int` and similar types requires an explicit `as`
  (`const int W = sizeof(int) as int;`; `const int W = sizeof(int);` still fails under the strict typing rules);
- Errors: division by zero / modulo by zero, signed division/remainder overflow (`MIN / -1`), circular dependency (`circular constant definition involving ...`), nesting depth exceeding 512 levels;
- In constant expressions `and` / `or` **evaluate both sides** (at runtime they short-circuit); therefore `false and (1 / 0 == 0)` reports division by zero in a constant context;
- Despite forward references and arbitrary declaration order, constants of the same name coming from `use` still **report ambiguity** under the normal name-resolution rules (they do not silently take the first);
- Not supported: function calls (there are no `constexpr` functions), `?:` conditional expressions, pointers/strings/aggregate values participating in evaluation.
  Aggregate constants follow the old rules (`const int[] G = {10, 20, 30};` keeps working). **A `string` cannot be a
  unit-level variable** (it holds reference-counted data, 3.3), so `const string S = "hi";` is a compile error —
  write `const char* S = "hi";` and convert at the use point.
- Enums are nominal types: enum members still require an explicit `as` to participate in arithmetic/comparison (`(Color.Red as int) * 2`).
- **Initializers of unit-level variables** (including non-`const` globals such as `int g = 1 + 2;`, `int G = Cat.MaxAge;`) also go through this evaluator:
  if a constant can be computed it is emitted to the global data area as a constant; otherwise it falls back to the old literal/aggregate rules, and if that still fails it reports "must be a compile-time constant".

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
- They may also declare **type-level compile-time constants** such as `const int K = 3;` (no instance storage, accessed via `Cat.K`, see 3.7.1); class members are private by default.
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
- Identity comparison: `c == d` compares whether both denote the same object; value comparison of classes is not supported (compare field by field when needed).
- Member access auto-dereferences: `c.name`, `c.parent.name`; writes are constrained by `const`.
- **Early release** (`del` was removed as of 0.0.47): just set the handle to null; `c = null;` releases one strong reference and sets `c` to null.
  The object is destroyed when the strong reference count reaches zero; for a shared object only your own reference is decremented. The target must be a writable class/interface
  reference lvalue (a `const` binding cannot be assigned); function parameters and `for`/`for-in` loop variables are likewise owning bindings,
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
- `const Cat c` (implemented, 0.0.61): a **read-only binding marker** — `c` cannot be rebound, and fields cannot be written directly through `c` (`c.a = 1` is an error).
  It is not a deep read-only view: a copied-out handle is still writable, and calling a method that modifies the object through `c` is also legal (there are no `const` methods, see 3.11).
- **`ref` is a binding mode, not a storable type** (since 0.0.62; extended in 0.0.63): borrows appear in exactly two places, **parameters** and **return types**,
  written as the prefix keyword `ref T p` / `ref const T p` / `ref T f()`; **no `T&` type suffix is introduced**. The reason: `&` in this language already is
  the unary address-of operator and the binary bitwise-and operator; modifiers in type position uniformly use a lowercase prefix (`weak`, `const`, `ref`).
  A borrow cannot be the type of a variable, field, or array element (`ref T x;` and `T&[]` are both errors, and the compiler emits a clear diagnostic).
- The borrow family shares one rule: **not storable** — the results of `for ref x in s`, `ref T p`, and `ref T f()` must not be assigned to another variable or field,
  must not have their address taken, and must not outlive the expression that produced them.
- `ref T` parameters (0.0.62; scalars supported since 0.0.63): `void f(ref Point p)` / `void inc(ref int x)` borrow the caller's variable;
  the callee may write to it; it cannot be rebound.
  - Borrowable types: primitive types, enums, pointers, and structs that contain **no reference-counted fields, have no destructor, and are not a union**.
  - The argument must be an **addressable lvalue**; `ref` parameters perform no conversion (they do not accept temporaries).
  - Assignment to a struct borrow is still a "rebinding" error (as in 0.0.62); assignment to a scalar/enum/pointer borrow **writes through** to the caller's variable.
  - Method/constructor parameters do not support `ref` yet; operator parameters do (see 3.11); `extern` does not.
- `ref const T p` (read-only borrow, 0.0.62): symmetric with the `const` rules of `for ref`; the callee can neither
  rebind the parameter itself nor write fields/elements through the borrow; the argument must be an **addressable lvalue** (temporaries are not accepted).
  Difference from a by-value `const T p`: the latter protects the **copy** inside the function body, whereas the former restricts **writes to the caller's object**.
  A mutable `ref T` does not accept a `const` argument ("cannot bind a readonly variable to a mutable `ref` parameter").
- **`ref T` returns** (0.0.63): `ref T f()` returns a borrow of storage that the caller already holds.
  - The root of the returned expression must be: a `ref` parameter, `this`, a global/static variable, or storage reached through a **pointer/slice** on the path (heap memory).
    Borrowing a local variable, a by-value parameter (including its inlined fields), or a temporary is a compile error.
  - The result is **not storable** (`ref T r = f();` is an error); but it can be written through in place: `f() = v;`, `f() += 1;`, `f().field = v;`,
    and it can also serve as the argument of a `ref` parameter, or be bound in a `for ref`.
  - A `ref const T` position cannot be returned as a mutable `ref`; the result of `ref const T f()` can only be read.
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
  - `const` **methods / deep read-only views** (decided not to do, see 3.11 and 12 table 98).
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
    Button(Rect b, string s) {
        super(b);
        label = s;
    }
    override void paint(Canvas c) { ... }
}
```

- **Only single inheritance is allowed**: `class D : B`; `B` is a class (for interfaces see 3.10).
- Field layout: base class fields come first, derived fields after; field names must be unique along the whole inheritance chain (shadowing is not allowed).
- Methods are **virtual by default**; overriding a base class virtual method must use `override`, and the signature must match exactly.
- Same name with a different signature → overload (see 3.11), not an override.
- A class with unimplemented methods (declared only, without a body) is an **abstract class** and cannot be `new`ed; no `abstract` keyword is introduced.
- An abstract class may have constructors, for derived classes to call via `super(...)`.
- A base class handle may point to a derived object (implicit upcast); converting a base class to a derived class requires an explicit `as`, which yields null on failure.
- Virtual calls during construction are dispatched C++-style (see 3.12).
- `super.m(...)` **only resolves along the class inheritance chain**; interface default methods cannot be called with `super` (error).
- No multiple inheritance and no `final`; a derived class may itself be inherited from.

### 3.10 Interfaces `interface`

```c
interface IEnumerable {
    IEnumerator get_iterator();
}

interface IList : IEnumerable, IObject {      // an interface may inherit multiple interfaces
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
- A default method may only call interface methods and cannot access fields; when two interfaces provide default implementations with the same signature, the class must explicitly override them, otherwise it is a compile error (a default declared on a derived interface refines the base interface's default and wins).
- An interface value is laid out as two machine words `{object pointer, class descriptor pointer}`; it may be `null`.
- Conversions: `class → interface` is implicit; `interface A → interface B` (A inherits B) is a static conversion;
  `class → derived class` and `interface → class` use `as` for a runtime check (along the desc base class chain), yielding `null` on failure;
  the downcast `interface → derived interface` is not supported in phase one (error).
- Interface references are strong references: copying is +1, overwriting is -1; `weak` is only supported for class fields, and **interfaces do not support `weak` (not done in phase one)**.
- An interface cannot be used as a parameter of an `extern` C function.
- **Generic interfaces are real interfaces** (0.0.108): `interface ISeq<T>` instantiates on use, and an instantiated
  interface (`IIterator<int>`, `IEnumerable<int>`) can be implemented by a class (`class Cursor : IIterator<int>`),
  implemented by a generic class through its parameter (`class Src<T> : IEnumerable<T>`), used as a **value type**
  (16-byte `{obj, desc}`, dispatch through the itable), a return type, a field/local type, a constraint
  (`T: IEnumerable<int>`) and an `as` downcast target. Interface inheritance may be generic
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
  - Methods whose signature mentions `Self` are never materialized: a value type cannot spell itself in that position,
    so such a method must be implemented explicitly.
- `for-in` does not depend on interfaces for the **static** path: a container with `iterator()` and a struct cursor
  is iterated with static dispatch and zero allocation (6.4.2). A cursor that is an **interface value**
  (`IIterator<T>`, the shape `IEnumerable<T>` returns) is iterated through the itable — same loop, dynamic dispatch,
  one RC-managed 16-byte local.

### 3.11 Methods, Overloading and Operators

- Class method: `ret name(params) { ... }`; `this` is a reference to the current object (for a class, the object handle; for a struct, an lvalue alias of the object itself),
  and is implicitly available; write `this` explicitly when disambiguation is needed; `this` is not a pointer and its address cannot be taken.
- Fields can be accessed directly inside a method; `this` cannot be rebound.
- **No `const` methods are provided** (decided not to do, see 12 table 98): methods have no read-only/mutable distinction, `this` is always writable;
  use a read-only `interface` when read-only must be expressed at an API boundary.
- Methods are virtual by default; `override` must have a signature identical to the base class/interface method.

**Overloading** (free functions, methods, constructors, operators):

1. Filter candidates by name + number of arguments;
2. Argument conversion rank: exact match (including literals adapted to the parameter type) = 0; implicit widening = 1; incompatible candidates are eliminated;
3. Pick the unique candidate that is "no worse than every other candidate and better in at least one respect";
4. No unique best → ambiguity error;
5. No user-defined conversions, no default parameters, and no variadic parameters; the return type does not participate in overload resolution.

- Method overloads **merge the same-named methods of the base class** (the derived class's overload set includes the base class's overloads).
- The same signature must use `override`; a different signature is an overload.
- `extern` C functions do not participate in overload resolution; neither does `main`.

**Operator overloading** (free functions, plus member operators on a `struct`):

```c
// free function form
Point operator+(Point a, Point b) { ... }
bool  operator==(Point a, Point b) { ... }

// member form: only inside a `struct`; the receiver is the implicit `this` (a borrowed pointer)
struct Vec {
    int n;
    Vec operator+(Vec o) { ... }       // binary: 1 parameter
    int operator[](usize i) { ... }    // read:   1 parameter
    bool operator==(Vec o) { ... }
}
```

| Operator | Allowed | Description |
|---|---|---|
| `+ - * / %` | Yes | Binary; may return any type |
| Unary `-` | Yes | |
| `< <= > >=` | Yes | Must return `bool` |
| `& \| ^ << >>` | Yes | Bitwise operations |
| `[]` | Yes | **Read and write through one operator**: `T operator[](C c, I i)` reads; `ref T operator[](C c, I i)` returns a borrow, so `c[i]` is a writable lvalue (`c[i] = v`, `c[i] += v`, `for ref` all work). **`operator[]=` was removed (0.0.101)**: a computed setter with no storage to borrow is no longer expressible |
| `== !=` | struct only | A class's `==` is fixed as identity comparison and cannot be overloaded; a struct that does not define `==` is not comparable |
| `()` `=` `&& \|\|` `as/new` | No | Conflicts with the built-in semantics |

- An operator must have at least one struct/class operand; built-in types cannot be redefined.
- **Member operators** (`implemented 0.0.87`) are allowed **inside a `struct` only** and are always static-dispatch (never virtual): a class operator would be virtual by default, which would break `==` identity and add dispatch cost, so classes keep the free-function form (a member operator written in a class body is a compile error). The receiver is the implicit `this`, so `s[i]` / `s == t` cost no copy and no retain/release; the parameter list is the free form minus the receiver (binary 1, unary 0, `[]` 1, `[]=` 2) and a member operator wins over a free function with the same signature.
- An operator function is an ordinary function and may be placed in any unit; a duplicate definition or a resolution ambiguity → compile error.
- When `!=` is undefined it is derived as `not (a == b)`; for a class this means identity inequality.
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
    Button(Rect b, string s) {
        super(b);          // must be the first statement
        label = s;
    }
}
```

- A constructor has no return type and its name equals the class name; it may be overloaded; `new T(args)` is resolved by the rules of 3.11.
  **Named arguments are not supported for constructor/`new` parameters** (`new C(v = 1)` is a compile error); named arguments are only used for struct construction (3.4).
- **`implicit`** (implemented 0.0.88, struct only): `implicit string(const char* c) { ... }` marks a single-parameter
  constructor as the implicit conversion path (see 3.6). It must be `public`, and writing it in a class body or
  union is a compile error. The marker is contextual: `implicit` is a modifier only in front of a constructor name.
- On allocation: the object's memory is fully zeroed, `rc = 1`, and the vtable points to the topmost base class.
- `super(args)` must be the first statement; if omitted, the base class's zero-argument constructor is called (an error if it does not exist).
- Construction order: layer by layer `super` → derived field assignments (in the constructor body); on entering each constructor layer, the vtable is updated to that layer.
- Virtual calls during construction are C++-style: they dispatch only to the current construction layer and above, and never enter a derived implementation that has not been constructed yet.
- A class that declares no constructor keeps the zero-initialization of `new T`; once a constructor is declared, `new T` is legal only if a zero-argument constructor exists.
- An abstract class may have constructors, but cannot be `new`ed.
- There are no user destructors (phase one); destruction is performed by the destroy entry of the vtable (see 8.3).

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
    MyError(string msg, int c) { super(msg); code = c; }
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
- Uncaught → `abort` at `main` (or at a thread/foreign-call boundary).
- Exceptions do not cross the C ABI and do not cross threads.
- A constructor may throw: the partially constructed object is zero-initialized and released through its destructor, then the exception propagates.
- **Implementation (B2)**: the function ABI is unchanged. A thread-local pending slot (`flag` + object pointer) is checked after every mu_lan call. On `throw`, the function releases its active scopes and returns a dummy value with the flag set; callers propagate it until a matching `catch`. `extern "C"` calls are not checked. No platform unwind tables, personality functions or unwinding runtime are needed; only libc and TLS are used, keeping Windows/Linux/macOS on one lowering.
- **May-throw optimization (0.0.83)**: a conservative module-local analysis marks functions whose bodies are visible and cannot return with a pending exception. Calls to them skip the pending check; every external/precompiled/static-library/dynamic-library/unknown-indirect target is treated as may-throw, so the optimization never constrains the future library ABI. Cross-library summaries and `nothrow` contracts are not implemented.
- `finally` is emitted by duplicating its body on each exit path, so it needs no platform EH support. Internal design notes: `agents/eh-b2-design.md`.

---

### 3.15 Generics (implemented in 0.0.64–0.0.72)

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
interface ISeq<T> { usize length(); T at(usize i); }
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
Pair<int, long> p = Pair<int, long>(3, 9);   // generic struct value construction
Box<int> b = new Box<int>(7);    // generic class construction
```

- **Type arguments must be given explicitly**; no deduction is performed (`max2(1, 2)` is an error). `auto` may be used to receive the result.
- In an expression, `name<...>(` is resolved as a generic call/generic construction: it first tries to resolve as a type argument list,
  and it only holds if the resolution succeeds and is immediately followed by `(`; otherwise it falls back to being a comparison expression (`a < b`).
- The `>>` of nested arguments is treated as two `>`.

#### 3.15.4 Instantiation rules

- **Lazy**: `Vec<int>` is instantiated on demand when it is encountered; the same `(template, argument list)` reuses the same instance.
- **Canonical name**: `Type::name` is `Vec<int>`, `Vec<Vec<int>>` (one space after the comma),
  which keeps the string comparison of `same_type` stable.
- **Symbol name**: the instance name is sanitized when it enters a symbol (`Vec<int>` → `Vec_int_`), see `mangle`.
- **Recursive instantiation** (needing itself again during instance expansion with no cached instance) is an error: `recursive generic instantiation`.
- An instantiated class follows the vtable/`class_desc`/RC path of an ordinary class; an instantiated struct follows the ordinary struct layout.
- Generic inheritance `class D<T> : Base<T>` supports a single base class: the base class is instantiated first, and the fields are flattened with "base class first";
  `super(...)`, method inheritance, and vtable slot reuse are the same as with ordinary inheritance.

#### 3.15.5 Constraint semantics: type set + member set

Each type name defines two things:

| Branch | Type set | Member set |
|---|---|---|
| Scalars such as `int` | Single element | Built-in operators |
| `enum` | Single element | Comparison, `as`, switch |
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
- Operator members: an operator that accepts `(T, T)` and returns `T` exists — a **built-in operator or an `operator` free function of a visible unit**.
- Therefore **built-in types satisfy interfaces at zero cost**: `int` satisfies `Self operator+(Self, Self)` thanks to the built-in `+`.
  The ready-made contracts (`IAddable`, `IOrdered`, `INumber`, …) live in `std.interfaces` (3.15.9).

**Exact matching, no implicit conversion**: constraint checking performs no implicit widening and no overload "best candidate" selection.
`Money operator+(Money, long)` does not satisfy `Self operator+(Self, Self)`.
(Consistent with Rust / C# / Go / Swift; the C++ concept "expression validity" model is not adopted.)

**Open branches do not enumerate implementors**: when checking interface members, only the declared signatures of the interface are used; all implementing types are not traversed.

**Members of `|`**: the body of a function with `T: int | long` may only use operations common to both (arithmetic, comparison, assignment);
when an open branch (`IComparable`) is mixed with a concrete type, the member set is usually empty; this is a semantic necessity, not a defect.

The **diagnostic** must state what is missing, for example:

```
type `Money` does not satisfy `IAddable` for type parameter `T`: no `operator+` accepting `(Money, Money)`
```

#### 3.15.6 `Self`

- `Self` is **valid only inside an `interface` body** and denotes the "implementor type".
- As a constraint (`T: I`), `Self` is replaced by the type argument; as a dynamic interface type, methods containing `Self` do not enter the vtable
  (object safety), so such interfaces cannot be used as value types.
- `Self` may only be used bare (`Self x`); nesting is not supported yet (`Self[]`).
- `auto` is not used as a substitute: `auto` is a deduction placeholder and `Self` is a dependent type; the semantics of the two are orthogonal.

#### 3.15.7 Current limitations (0.0.72)

| Limitation | Description |
|---|---|
| A generic interface is **only usable as a constraint** | `ISeq<int>` may be used in a `T:` constraint; it **cannot be used as the type of a variable/field** (explicitly not done; the rationale is given below) |
| A generic struct/class does not support interface base classes | `struct S<T> : I` is currently an error, pending U33 and M5b |
| A generic struct/class does not support `const` members | Type-level compile-time constants are not wired up yet |
| A generic interface does not support inheritance | `interface IList<T> : ISeq<T>` is currently an error |
| Not supported | Method-level generics, type argument deduction, associated types, specialization, generic `extern`, generic type aliases |

#### 3.15.8 Why there is no "value type as an interface value"

In a language without borrow checking there is only one implementation path from a value type to an interface value: **boxing** (copying to the heap + reference counting). Its cost is
comparable to `new`ing a `class` object (one object header saved, one extra copy), yet it loses the explicit lifetime of a `class`, the `as` downcast,
and `weak`. Therefore this language explicitly does not provide:

- Runtime polymorphism → `class` (single inheritance + interfaces + vtable + RC);
- Compile-time capability checking → `T: I` (3.15.5) and `struct S : I` (U33), at zero runtime cost;
- Heterogeneous value collections → sum types (10.1), rather than interface boxing.

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

`libs/std/interfaces.mu` (unit `std.interfaces`, loaded implicitly) declares the contracts the
compiler and the library share. They are ordinary `interface` declarations — usable as `:` targets
and as constraints in generic code; nothing is added to the runtime (structural check only).

| Interface | Members | Satisfied by |
|---|---|---|
| `IIterator<T>` | `bool next()`, `T current()` | a cursor for `for-in` (6.4.2): a struct (static) or a class (interface value, dynamic) |
| `IEnumerable<T>` | `IIterator<T> iterator()` | a container; a class returning a class cursor, or a struct returning one (a struct cursor is still iterated statically by `for-in`) |
| `IIndexable<T>` | `usize length()` (+ a structural subscript, 6.4.1) | containers; the subscript half is not declarable in an interface |
| `IEquatable` | `==`, `!=` | numbers, pointers, a value type with those operators |
| `IOrdered : IEquatable` | `<`, `<=`, `>`, `>=` (plus equality) | numbers, pointers, enums, a value type with those operators |
| `IAddable` / `ISubtractable` / `IMultipliable` / `IDivisible` / `IModulable` | the binary `+ - * / %` returning `Self` | numbers (`%` is `frem` for floats) |
| `INegatable` | unary `-` | numbers |
| `INumber : IAddable, ISubtractable, IMultipliable, IDivisible, INegatable, IOrdered` | the arithmetic bundle (C# generic-math shape) | numbers; **not** enums (they do not take part in arithmetic) |

- There is deliberately **no bitwise contract**: the constraint checker recognises only `+ - * / %`
  and the comparison operators as built-in operators, so `IBitwise` could not be satisfied by the
  built-in integer types today.
- User value types satisfy these contracts by declaring the corresponding `operator` members
  (or free `operator` functions in a visible unit); `class` equality stays identity, so a `class`
  does not satisfy `IEquatable`.
- A type can also declare the contract explicitly (`struct Money : IAddable`), which checks the
  same shape at the definition site.

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
- **Status (0.0.111)**: the type, the layout, the calling convention, the thunk, the RC plumbing and
  capturing are in place. A capture list `[x]` / `[ref x]` is **mandatory for every outer variable the
  body uses** (implicit capture is an error, so the copying cost is visible at the definition site):
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
    `[ref ...]` of a reference-containing type, by-value capture of a struct with a destructor, and
    **returning a `[ref ...]` lambda** of frame-owned storage (the variable always dies with the
    call). `[ref]` of caller-owned storage — `this` of a struct method, a `ref` parameter, a
    `for ref` binding — may escape; its lifetime is then the caller's responsibility.

  Rationale: "declaration vs expression statement" stays a single-token decision (a declaration starts with a keyword
  or an identifier), and `(int) => int f` would read like a cast.
- `const` cannot qualify an **anonymous** function type: there is no pointee to protect, and a declaration cannot begin
  with `(` anyway. Name the type with `type` when a read-only binding is needed (`type F = (int) => int; const F f = ...`).

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

Not implemented: capturing closures (the `delegate` type exists since 0.0.109 — 3.16.1a — but its environment
is always null until capture analysis lands), method values, generic function values
(`max2<int>` as a value), and anonymous function types in declaration positions (a unit-level variable, a local
declaration and a return type must name the type with `type` first).

### 3.17 Concurrency: `para`, `thread`

> **Status: `para` and `para for` are implemented over every source form, including the cursor form
> (0.0.92–0.0.97), and run on the M:N task runtime since 0.0.98 (SPEC 3.17.7); `thread` is not.**
> The runtime layer (scheduler, task stacks, pollers, channels) is a **library**; this section fixes
> only the language surface. The decision record, invariants and phasing live in
> `agents/task-runtime.md`; the library contract is in `docs/lib-std-task.md`.

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
- The first exception from a section **stops the region**: the remaining sections are asked to cancel,
  all are joined, and that exception is rethrown in the enclosing scope (`para for` additionally stops
  producing items). If the region is already unwinding, later exceptions are discarded.
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
- Sources: the built-in sequences of 6.4.1 (ranges and managed sequences `T[]` / `buffer<T>` / `String`) and user types
  satisfying the **cursor form** of 6.4.2 (`iterator()`, `next() -> bool`, `current() -> T`). A
  blocking `next()` is where backpressure and the end condition live; the source, not the syntax,
  decides when the iteration ends.
- Iteration order is unspecified. `for ref` follows 6.4.1 (distinct elements only).
- The body obeys the same restrictions as a `para` section; in addition `break`, `continue` and
  `return` are errors — a region ends when its source ends, or by cancellation.
- One task per item is the semantics. A chunked view (`a.chunks(k)`, library) is how a large finite
  source reduces per-item overhead; it does not change the semantics.

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

- A task runs on a **fixed-size stack** (a small runtime default, tens of KiB); it does not grow. Deep
  recursion and large locals belong in a `thread` section, which uses the OS stack.
- Exceeding a task stack overflows it and aborts the process; this is not recoverable.
- Data races between tasks or between threads are undefined behaviour (8.2). The compile-time rule
  against writing enclosing locals is a narrow, syntactic guard, not a race checker.

#### 3.17.7 Implementation status

`para` (0.0.92) and `para for` over every source form (0.0.93–0.0.97) run on the **M:N task runtime**
(0.0.98); `thread` is not implemented. Current behaviour and limitations:

- **Tasks, carriers, stacks**: one task per concurrent unit, each with its own **fixed-size fiber stack**
  (default 64 KiB commit / 256 KiB reserve, `std.task.set_task_stack`); carriers are OS threads (one per
  core, plus handoff ones, capped at 4x cores) that run one task at a time and switch stacks in O(1).
  A parked task releases its carrier; a finished task's stack goes back to a pool and is reused.
- **Joining**: a task that joins parks (its carrier runs other work, including nested `para` children);
  `main`, a `std.thread` thread or a C thread that joins waits on a condition variable. A region joins
  its own tasks in start order.
- **Blocking calls pin a carrier**: `thread_sleep`, a blocking mutex/condvar or a blocking C call is not
  a park point in this version (park points arrive with the IO poller). A monitor thread watches carrier
  heartbeats and adds a carrier when work is queued and every carrier is stalled (handoff, design 6.4),
  so blocking does not starve the queue; the runtime cannot distinguish blocking from CPU work.
- **Exception state travels with the task**: the pending-exception slot is saved on switch-out and
  loaded on switch-in, so a task may resume on another carrier without leaking pending state.
- Still no cancellation: a started task always runs to completion; an exception that escapes a task body
  **aborts the process** (the B2 thread-boundary rule), while exceptions caught inside it behave normally.
- `this` and implicit field access inside a section are **compile errors** in v1 (pass the object as an
  explicit argument). Writing, taking the address of, or passing by `ref` an enclosing local are
  compile errors as specified in 3.17.2.
- `para for` has **one task per item; chunking is never automatic** — a user or library splits the work
  (`chunks(k)` or an explicit index loop). The producer is a single puller: it evaluates the source once
  (so any expression is allowed as the source), calls `next()` between item starts (a blocking `next()`
  is where backpressure lives) and stops at the end of the source. The loop variable keeps its ordinary
  binding (value or `ref`, with element retain/release for reference-counted elements).
  `break`/`continue` at the body level are errors and `for i, x` is not supported yet.
- The scheduler enforces a **maximum number of live tasks** (default 10000, `std.task.set_max_live_tasks`)
  and throws `std.task.TaskLimitError` when a start is refused. A failed start is an ordinary exception:
  the region releases the failed item, joins the tasks that already started, and propagates.
- An exception raised while **producing** an item (the source expression, `iterator()`, `next()`,
  `current()`, `length()`, `operator[]`) is an ordinary exception of the region: the tasks that already
  started are joined before the enclosing scopes are left, so no task ever outlives the producer's frame.
- Carriers live for the whole process and are never shut down; `thread` (the compute pool) is not
  implemented yet, and there are no park points for IO, so the poller/channel/server layers (P4) are
  still missing.

The section body is a hidden function that receives a frame of pointers to the caller's captured
locals; the caller is suspended at the join, so those slots outlive every section. An item body is the
same hidden function with a per-item context `{ frame, item }` (the element address for `for ref`).
The lowering calls `std.task.muc_para_start` / `muc_para_join`, so the language surface does not depend
on the execution engine.

## 4. Declarations and scopes

- Declaration syntax: `Type name1 [= init1], name2 [= init2];`
  - Initialization forms:
    - No initializer: `int x;` (reading an uninitialized local value is UB; unit-level variables are zero-initialized by default)
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
- Local type inference: `auto name = expr;`
  - Local variables only; an initializer is required; cannot be used for parameters, return values, fields, or unit-level variables.
  - Multiple declarators in the same declaration must deduce the same type: `auto a = 1, b = 2;` is legal; `auto a = 1, b = 2.0;` is an error.
  - Cannot be deduced from `null`, `{...}`, or a void expression; `auto x = 0` deduces to `int` (not an aggregate zero initialization).
  - `const auto name = expr;` is **implemented** (immutable + deduced); unit-level variables cannot use `auto` (an explicit type is required).
  - `auto` only takes the expression type directly; it does not perform Hindley-Milner unification.
- Except for `auto`, parameters, return values, fields, and unit-level variables must all have an explicit type.
- Unit-level variables are declared directly (`int g = 1;`) and are `private` by default (visible within the unit); use `public` across units; `extern` denotes an external symbol.
- **Initializers of unit-level variables**: must be a **compile-time constant expression** (rules in 3.7.2; the same requirement applies to both `const` and non-`const` globals) —
  literals, unary `-`/`+`/`not`, `as`, arithmetic/bitwise/comparison/`and`/`or`, and **references to other constants**
  (unit-level `const`, enum members, type-level `const` members) as well as `sizeof(T)` / `alignof(T)`;
  forward references are supported; `const int B = A + 1;`, `int g = 1 + 2;`, `int G = Cat.MaxAge;` are all legal. Cyclic dependencies and division by zero are errors.
  Aggregate constants are still written as literals or aggregate initialization (`const int[] G = {10, 20, 30};`).
  A `const` unit-level `string` is still not allowed (a unit-level `const` must be a compile-time constant):
  write `string S = "hi";` for a mutable global, or `const char* S = "hi";` when a compile-time literal is enough.
- **Local `const`** may be initialized with a runtime expression (`const int x = f();` is legal); it simply cannot be assigned again after initialization.
- A `const` inside a type body is a **type-level constant**, must be initialized on the spot, and must be a compile-time constant; see 3.7.1.
- `static` has been removed (0.0.32): unit-level variables naturally have static storage duration; `static` local variables inside functions and `static` functions are not supported.
- Parameters are mutable local bindings (phase one, C semantics); whether they become immutable by default is pending.
- **Parameters are owning local variables** (since 0.0.47, consistent with 3.8): parameter values are retained on entry and released on exit;
  this applies to class/interface/`string`/slice parameters and to struct parameters that contain reference fields. A parameter may be freely rebound inside the function body,
  without affecting the caller; assigning an owned value (`new`/a call result) to a parameter does not leak. Arguments at the call site are still passed by value.
- `ref T` parameters (**0.0.62; scalars supported since 0.0.63**): borrow the caller's variable by reference; the callee may write, but cannot rebind or store;
  primitive types/enums/pointers/pure value structs may be borrowed; methods/constructors and `extern` are not supported.
- `ref T` returns (**0.0.63**): the borrow must originate from a `ref` parameter, `this`, global/static storage, or pointer·slice storage; the result cannot be stored,
  it can only be written through on the spot or passed as a borrow argument (see 3.8).
- `ref const T p` is a **read-only borrow**: it cannot be rebound, and fields/elements cannot be written through the borrow; the argument must be addressable (temporaries are not allowed).
- **`ref` is a binding mode, not a storable type**: it does not introduce `T&`; a borrow cannot be the type of a variable/field/element/array element
  (no `ref T x;`, no `T&[]`), sharing the "not storable" rule with `for ref` (see 3.8).

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
- **`>>` is always an arithmetic right shift** (using the innermost bit width of the operand): signed types sign-extend normally; **unsigned types are also interpreted as two's complement for an arithmetic right shift**
  (`uint 0x80000000 >> 4` = `0xF8000000`). This differs from C's "unsigned right shift is a logical shift" and is the semantics the current implementation has settled on;
  compile-time constant evaluation and runtime code generation in the compiler backend must stay consistent; if this ever changes to "signed arithmetic / unsigned logical", both places must change together.
  `<<` is a bitwise left shift (overflow wraps in two's complement); a shift count ≥ the bit width is still undefined behavior (8.2).
- `%` is also legal for floating point (LLVM `frem`, synonymous with C `fmod`: the sign of the result follows the dividend); integer `%` is truncated remainder (the sign follows the dividend).
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
- Built-in functions: `alloc(bytes: usize) -> void*`, `realloc(p: void*, bytes: usize) -> void*`, `free(p: void*)`.
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
- The result is produced with **value** semantics (prvalue): structs/strings/fixed-size arrays are copied (reference fields are retained field by field);
  class/interface yields a new strong reference (a borrowed branch is +1, a `new`/call result transfers ownership directly);
  a dynamic array transfers/shares the handle (consistent with the allocation semantics of `T[N] → T[]`).
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

### 6.4 for-in

```c
for x in seq { ... }       // x is the value binding for each iteration (copied by default)
for ref x in seq { ... }   // x is a mutable reference to the element
for i, x in seq { ... }    // with index/ordinal (see 6.4.4)
```

- The parenthesized forms `for (x in seq)` / `for (i, x in seq)` / `for (ref x in seq)` are also accepted; canonical style omits the parentheses.
- Built-in sequences: half-open range `a..b` and managed sequences (`T[]` / `buffer<T>`). **Static arrays `T[N]` are C storage and are not iterable** (use an index loop). A `String` is iterated through the
  index form of 6.4.1 (`length()` + the library's member `operator[]`, byte by byte) — the compiler has no
  built-in knowledge of it.
- **`for-in` is not supported for multidimensional arrays** (both `T[m,n,...]` and `T[,]` count): use an index loop; for a static multidimensional array you can take a row first and then iterate.
- User types: lowered according to the **iteration forms** of 6.4.1–6.4.3 (**the index form is implemented; the cursor form is not implemented yet**);
  built-in sequences do not participate in form matching.
- `x` is scoped to the loop body only; immutable and bound by value by default; `x` is an **owning local variable** (0.0.47 semantics), so when the element is a
  class/interface it is retained on entry and released on exit of each round; the `ref` form can write to elements.
- `seq` is evaluated only once; its length is fixed at the start; elements must not be added or removed during iteration (UB or a runtime error).
  - A simple lvalue (a variable, a member access chain) is iterated directly; other expressions are evaluated by the compiler first, stored, and then iterated.
  - The sequence of `for ref` must be a variable or member access (it must be addressable); `for ref v in arr[f()]` is an error.
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
    usize n;
    int* data;
    usize length() { return n; }
}
int operator[](Vec v, usize i) { return v.data[i]; }   // free-function operator, see 3.11
```

- Conditions (all must hold):
  - a method `length() -> usize`;
  - `operator[](usize) -> T`, either a **member operator** on the container or a free-function operator `operator[](C, usize) -> T` (3.11).
- Signatures are matched **exactly** on `usize`: `operator[](C, int)` does not qualify (no narrowing conversion); on error the actual signature is reported.
- Lowering: `usize n = c.length();` (evaluated only once) → `for (usize i = 0; i < n; ++i) { T x = c[i]; ... }`.
- The element type is the return type of `operator[]` (if it returns `ref T`, the element type is `T`); **proxy/reference wrappers are not recognized**
  (a `vector<bool>`-style proxy design is not supported).
- `length()` must equal the number of valid indices of `[]`; a mismatch is a user error (debug bounds checks catch it).
- In `for i, x`, `i` is the real `usize` index.
- `for ref` requires `operator[]` to return `ref T` (the element is then addressable); when `[]` returns a value, use `for i, x` + `c[i] = ...`.
- **Copy cost**: with the free-function form, `C` is copied on every iteration (reference fields retained/released field by field); write it as
  `ref C` or, better, define the operator as a **member** (`T operator[](usize i)`), whose borrowed `this` makes every iteration a plain indexed load.
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
  through the itable. This is the dynamic channel — it is what `IEnumerable<T>` returns, because an interface value
  cannot hold a struct (no boxing). The loop holds one RC-managed local; the element type is still
  `current()`'s return type.
- The cursor form does not support `for ref` (`current()` returns a value); mutable iteration goes through the index form (`[]` returning `ref T`) or `for i, x`.
- **How the compiler finds the iterator** (0.0.107): by the member **name** `iterator()` — the same
  name-based structural recognition the index form uses for `length()` / the subscript. No interface
  is required and none is consulted; the element type is the return type of the cursor's `current()`.
  Interfaces in `std.interfaces` make the contract explicit at the definition site but are optional:
  - the cursor may declare `: IIterator<T>` — checked against `bool next()` / `T current()`;
  - the container may declare `: IEnumerable<T>` — checked against `IIterator<T> iterator()`.
  `IEnumerable<T>` is an ordinary generic interface (0.0.108): a class implements it by returning a class
  cursor as `IIterator<T>`, `for-in` iterates that value, and `T: IEnumerable<int>` works as a constraint.

#### 6.4.3 Resolution order and diagnostics

1. Built-in sequence → built-in lowering (see the start of this section); it does not participate in form matching;
2. A user type with an `iterator()` method → **cursor** lowering (implemented 0.0.96; a type opts into
   streaming by providing `iterator()`, and the cursor contract is `std.interfaces.IIterator<T>`);
3. Otherwise satisfies the index form → index lowering (**implemented**);
4. Both could be satisfied → the **cursor** form wins (providing `iterator()` is an explicit choice);
5. Neither is satisfied → a compile error that names `std.interfaces.IIterator<T>` and the index-form
   members (`length()` + a subscript).

- `length()` is a normal method for user types and a compiler-lowered accessor for `T[]` / `buffer<T>`; static arrays have no `length()`;
  user containers write `e.length()`.

#### 6.4.4 Index variable `i`

- `for i, x in seq`: `i` is a `usize` with the same scope as `x` and is read-only inside the loop.
- Built-in sequences: a sequential number starting at 0 (for a range, the offset relative to `lo`).
- Index form: the real index; cursor form: the iteration ordinal.
- Implemented (built-in sequences + index form); the iteration ordinal of the cursor form lands together with 6.4.2.

### 6.5 Miscellaneous

- `return expr;` / `return;` (void).
- Releasing a reference: `c = null;` (since 0.0.47 `del` has been removed; see 3.8 / 8.3).
- `do-while`, C-style `for`, and `goto` are not provided.
- `switch` is covered in 6.6: no fallthrough, and branch bodies must use braces.
- Exceptions use `try`/`catch`/`throw` (3.14); for fatal errors `abort()` (libc) is still available.

### 6.6 switch (implemented in 0.0.45)

```mu
switch code {
    0:         { return 0; }
    1, 2:      { return 1; }
    3..10:     { return 2; }      // half-open range: [3, 10)
    Color.Red: { return 3; }
    else:      { return -1; }
}
```

- Form: `switch expr { cases* }`. Parentheses around `expr` are **optional** (consistent with `if/while/for`; canonical style omits them); no `;` after `}`.
- Case: `pattern list : { statements* }` or `else : { statements* }`. The case body **must** be a brace-delimited block (consistent with the project's "braces are mandatory"), and each case body is its own block scope.
- There is at most one `else`, and it must be the last case. An empty case is written `{}`.
- **No fallthrough**, and no `break` is needed; to match several values in one case just write them separated by `,`.
- Patterns must be **compile-time constants**: integer literals, character literals, literals with a unary `-`/`+`, enum members. Variables (including runtime-initialized `const`) cannot be patterns.
- A range `a..b` is **half-open** (includes `a`, excludes `b`); `a..=b` includes the upper bound; the endpoints must be constants of the same type, and an empty range is a compile error.
  **`step` is not supported** (a case has no notion of iteration, so writing it is an error); the range convention matches for-in (see 6.4).
- **Enums**:
  - A case only accepts members of that enum: the prefix may be omitted (`Red`), or written as `Color.Red` / `unit.Color.Red`; no implicit integer conversion (`0` matching `Color` is an error).
  - Range cases are not supported (enum members have no numeric order).
  - **Exhaustiveness is mandatory**: all members must be covered, otherwise it is a compile error listing the missing members; writing `else` gives up the exhaustiveness guarantee (in that case, if all members are already covered, `else` is unreachable and a warning is emitted).
- **Integers / `char` / `byte`**: `else` is optional; unmatched values are simply skipped, which is not an error.
- The type of the matched value is restricted to integers and enums; `bool`, floating point, pointers, class, interface, `string`, struct, and arrays are all compile errors.
  **This is a decision, not an implementation limitation**: `switch` will not be extended to other matched types (a `string` case would need hashing or a comparison chain, which is a performance trap).
- The matched expression is evaluated only once; cases are matched in source order. Duplicate or overlapping patterns (intersecting ranges, a value intersecting a range) are compile errors, so order does not affect semantics and the backend is free to reorder.
- `break` / `continue` **do not belong** to `switch`: they always apply to the nearest loop; with no enclosing loop they are errors under the existing rules. `switch` cannot be the target of `break`.
- When `as` produces an undeclared value for an enum and there is no `else`, falling through is **UB** (consistent with `as` performing no checking); debug builds may trap.
- Implementation: dense integer/enum values use LLVM `switch` (jump table), range cases use a comparison chain, and `else` becomes the default case.
- **No extensions (settled, 0.0.62)**: `switch` is positioned as C-style **constant dispatch**, kept "small and predictable", and will not be
  dragged by pattern matching into a second control-flow construct. All of the following are **explicitly not done**:
  - `switch` expression form (`let x = switch ...`);
  - guards (an `if` clause on a case);
  - bindings (`x @ pattern`);
  - or patterns (`p1 | p2`; for multiple values use a comma list);
  - type cases (`switch` dispatching on runtime type / sum types);
  - struct / tuple / array destructuring;
  - C-style `case` labels and fallthrough;
  - the `match` keyword (not reserved).
  If sum types/pattern matching are introduced in the future, a separate construct will be established rather than overloading `switch` semantics.

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
- Class methods, constructors, and `this` are covered in 3.11/3.12; methods are virtual by default, and an override is marked `override`.
- Parameters are mutable bindings (phase one); whether they become immutable by default is pending.
- `extern` declaration:

```c
extern int puts(const char* s);
```

  Uses the C ABI; `const` does not affect the ABI.
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
`std.ctypes` provides the C-side names, implemented per platform (`libs/std/platform/<os>/std/ctypes.mu`),
so the same source declares the same C function everywhere:

```mu
use std.ctypes;

extern c_long ftell(void* stream);                  // 32-bit on Windows, 64-bit on POSIX
extern c_size fread(void* buf, c_size size, c_size n, void* stream);
extern int fseek(void* stream, c_long offset, c_int origin);
```

| Names | Width |
|---|---|
| `c_char` `c_schar` `c_uchar` `c_short` `c_ushort` `c_int` `c_uint` `c_float` `c_double` | fixed on every target |
| `c_long` `c_ulong` | **platform-dependent** (32-bit on Windows, 64-bit on POSIX) |
| `c_longlong` `c_ulonglong` | 64-bit |
| `c_size` `c_ssize` `c_intptr` `c_uintptr` `c_voidp` | pointer-sized |
| `c_wchar` | **platform-dependent** (16-bit on Windows, 32-bit elsewhere) |

- Rule: **every `extern` declaration uses these names**, never Mulan's `long`/`usize` directly; the aliases are
  transparent (`c_long` *is* `int` on Windows), so no conversion or cost is introduced.
- `c_voidp` is `void*`; struct by value, unions and function pointers interoperate as in C (3.16).

---

## 8. Memory management

### 8.1 Built-in functions

| Function | Semantics |
|---|---|
| `alloc(bytes: usize) -> void*` | Allocates uninitialized memory; returns `null` on failure |
| `realloc(p: void*, bytes: usize) -> void*` | Resizes; returns `null` on failure, leaving the original block unchanged |
| `free(p: void*)` | Frees; `free(null)` is a no-op |
| `sizeof(T) -> usize` | Size of a type | 
| `sizeof expr -> usize` | Size of the operand's type; the operand is **not evaluated** (`sizeof *p`, `sizeof a[i]`, `sizeof(a)`) |
| `alignof(T) -> usize` / `alignof expr` | Alignment of a type / of the operand's type (not evaluated) |
| `offsetof(T, field) -> usize` | Byte offset of a struct member (0 for every `union` member; compile-time constant) |
| `x.length() -> usize` | Length of a managed sequence (`T[]` / `buffer<T>` / `String` / user containers); `x.length(axis)` for a multidimensional sequence; static arrays use `sizeof(a) / sizeof(a[0])` |

- `alloc` returns memory aligned to at least 16 bytes (consistent with common malloc); this is a **lower-bound guarantee**, not an upper bound.
  For heap restrictions on over-aligned types see 3.4 (class objects are allocated by the compiler with the required alignment; dynamic array elements have `align ≤ 8`;
  a manually `alloc`ed struct instance with `align > 16` is UB). A general `alloc_aligned` is still pending (backlog U27).
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
- Division by zero; **signed division/remainder overflow** (`MIN / -1`, `MIN % -1`); a shift count ≥ the bit width
  (pending: UB or defined).
- Signed integer overflow of `+` `-` `*`: **settled: defined as two's-complement wraparound** (avoids UB and simplifies
  optimization; the backend does not emit `nsw`/`nuw`). **Division is the one exception**: the CPU traps on `MIN / -1`,
  so it is UB exactly like division by zero; write `-x` (which wraps) when that value must be produced.
- Managed-sequence out of bounds: checked by default and throws `BoundsError`; `--bounds=off` removes the check (then it is UB). Static arrays and `T*` are unchecked.
- A pointer obtained by decaying an array **prvalue** (a function result, or the result of `?:`) is only
  valid until the end of the enclosing statement: the temporary it points into is gone afterwards.
- Accessing a class reference that is `null`; continuing to use a handle after it has been set to null (equivalent to a null reference).
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
- An optional `--single-thread` build can use a non-atomic fast path (that build forbids cross-thread use; pending).
- There is no resource release mechanism other than a user destructor; destructors are covered in 3.13.

---

## 9. Modules and C interoperability

- One unit per file:

```c
unit example;
```

  It must be the first declaration in the file.
- `use other;` imports another unit and accesses it as `other.symbol`; cyclic dependencies are forbidden.
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
  - Backend: non-`public` top-level symbols use `internal` linkage (`public` / `extern` / `main` keep external linkage).
  - **Visibility is a compile-time API contract, not a security boundary** (`void*`/`as` can bypass it).
  - `public` does not promise that the symbol appears in the final binary: an **executable** build
    internalizes every definition outside the export surface (plus `--export` names) so that dead code
    can be removed. Library targets keep their exported surface visible (see `agents/toolchain-plan.md` §11).
- No preprocessor; module-level constants use top-level `const`.
- `extern` is used for C functions/global variables; C ABI; struct layout is compatible with C. `extern` functions cannot be overloaded.
- Class objects cannot be passed to C: the C side does not participate in reference counting; when crossing languages use a struct + `alloc`.
- A string literal is a NUL-terminated `const char*` and can be passed to C directly.
- Symbol mangling/naming conflict rules are pending.

### 9.1 Unit-level initialization and cleanup (implemented 0.0.102)

Unit-level variables may have runtime initializers and are destroyed at exit. This section defines
the order and the rules; it lifts the three "no exit-time destruction" restrictions that used to
appear lower in this document (3.3 `String`, 3.4 reference fields, 3.4 user destructors).

- **No new syntax.** `T g = expr;` accepts any runtime expression, and cleanup is the type's `~T()`.
  A unit-level variable whose type needs cleanup (a class/interface handle, `String`, a managed
  sequence, a struct with reference fields or a user destructor) is destroyed at exit.
- **Dependency edges come only from `use`.** Cyclic `use` is already forbidden (9), so the units form
  a DAG. Implicitly loaded standard-library units are treated as implicit `use`s of the entry unit and
  come first.
- **Deterministic order.** Starting at the **entry unit** (the unit that declares `main`), visit `use`
  declarations in source order, depth-first, and append each unit in **post-order** on first visit.
  That sequence initializes the units; cleanup runs the **exact reverse** of the same sequence. The
  order depends only on the source — never on link order, file names or the order units are compiled in.
- **Within a unit**, variables initialize in declaration order and clean up in reverse declaration
  order; together with the previous rule, cleanup is the strict reverse of construction.
- **Who participates.** Compile-time `const`s, `extern` declarations, and variables without an
  initializer whose type has no constructor stay in the static data area and run no code (a struct
  with a parameterless constructor is constructed by the sequence, like a local `S s;`). An
  initializer the constant evaluator can fold (3.7.2) also stays static. A variable whose type needs
  cleanup is registered for cleanup even when it is zero-initialized, because the program may assign
  it later.
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

---

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
- **Near term**: library-level `Variant` / `Any` structs (tag + union/byte buffer, manual memory, C ABI compatible).
- **Later**: language-level sum types (enum with payload) + `match`, for heterogeneous data in closed sets.
- `dynamic` is only reserved as the name for future "runtime dynamic typing" and does not enter phase one; even if needed later, it should be an independent runtime layer rather than a core language feature.
- `any` is reserved as a static top type/library type, distinct from `dynamic`.

---

## 11. Main differences from C

| Topic | C | Mulan |
|---|---|---|
| Mutability | `const` qualifier | Mutable by default; `const` type qualifier (C model) |
| Variadic parameters | `...` + `va_arg` (user-definable) | Only `extern` declarations may have `...`; `va_list` is not provided |
| Declarations | Declarators mixed with types | Type first; multiple variables share the type (including pointers/arrays) |
| No return value | The return type may be omitted (C89) | `void` must be written explicitly |
| Initialization | `T x = 1;` | Declarations use `=` only: `T x = expr`, `T x = {...}`, `T x = {}` (aggregate zero value), scalars `= 0`/`= null`; `T(...)` is expressions only |
| Local type inference | — | `auto x = expr;`; local only, an initializer is required; multiple declarators must have the same type |
| Dynamic typing | None at language level (void*/variant) | None in phase one; `dynamic` is reserved only |
| Unit-level variables | May be declared directly | Declared directly; `private` by default, `public` across units |
| Zero-value initialization | — | Declarations: aggregates `T x = {}`, scalars `= 0`/`= null`; expressions `T()`; no more `T x()` |
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
| switch | Fallthrough, `case` labels | Yes; no fallthrough, no `case`, `pattern list : { }`, enums must be exhaustive (6.6) |
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
| 4 | Layout of `string` and conversion to `byte[]` | Decided (0.0.89): `string` is a **pure library type**; the compiler knows neither its layout nor any of its operations (`src/` no longer has a `TypeKind::String`). A literal is a `const char*` (3.3); the library provides `implicit string(const char*)`, `length()`, `operator[]`, `operator==`, `cstr()`, `strlen()`. Unit-level `string` variables are allowed since 9.1 (a `const` one is still an error: unit-level `const` must be a compile-time constant) |
| 5 | Whether arrays implicitly decay to pointers | **Decided (0.0.106): yes, as in C99.** An array in value position decays to a pointer to its first element (initializer, assignment, argument, struct-field access, row of a static multi-dimensional array); `sizeof`/`alignof` do not decay. The array object itself is **not assignable** (`a = b`, `int[N] b = a`, `s.v = a` are errors) and a `T[N]` parameter is `T*` (6.7.5.3p7). Kept as Mu extensions: an array-typed `return` copies, and a prvalue array (function result) can initialize an array. Arrays of class/interface elements keep value semantics (no pointer-to-reference type exists) |
| 6 | Whether dynamic arrays are included in phase one | Decided: included. `T[]` / `T[,]` / `T[,,]` are **syntax sugar for `Array<T>` / `Array2d<T>` / `Array3d<T>`** (decided 2026-11-14; the containers are still to be written, backlog U38) and the built-in `buffer<T>` is the low-level safe block they build on. Until `Array*` lands both spellings resolve to the same built-in RC sequence (`{rc,len,cap,data}`), so the current semantics are those of `buffer<T>` |
| 7 | Bounds checking strategy | Managed sequences checked by default (`--bounds=on` / `--bounds=off`), a violation throws `BoundsError`; static arrays and `T*` unchecked |
| 8 | Signed overflow | Decided (implemented): `+`/`-`/`*` wrap in two's complement (the backend emits no `nsw`/`nuw`); signed division/remainder overflow (`MIN / -1`, `MIN % -1`) is UB like division by zero, and a constant expression reports it |
| 9 | `switch`/`match` | Decided (0.0.45; extended in 0.0.62, explicitly not done): only `switch` (no fallthrough, `pattern: { }`, multiple values, half-open ranges, exhaustive enums); **the expression form/guards/bindings/or-patterns/type branches/destructuring are all not done**; `match` is not retained, and if pattern matching is needed later a separate construct will be defined |
| 10 | `goto`/`typedef`/function pointers/varargs | `goto` is not done (phase one); function pointers and lambdas are implemented in 3.16 (0.0.84); the `typedef` syntax is not done, type aliases are written `type A = B;` (implemented in 0.0.62, see 4.5); varargs are limited to `extern` |
| 11 | Indexed form of `for-in` / `ref` / default copy | **Decided (the indexed form is implemented)**: value binding + `ref` (built-ins only); `for i, x` provides a `usize` index/ordinal (6.4.4); user types use the iteration forms of 6.4.1/6.4.2; `for ref` is not available for user types; **multi-dimensional arrays forbid `for-in`** |
| 12 | `..=` / `step` / reverse order for ranges | Implemented (0.0.62): `..=` includes the upper bound; `step` only follows a sequence expression (`0..10 step 2`) and must be a non-zero compile-time constant; reverse order relies on an **explicit negative step** (`a..b` does not reverse automatically); switch case ranges support `..=` and **do not support `step`** |
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
| 24 | Whether `const` requires a compile-time constant | Locals do not (they may be initialized at run time); **unit-level `const` initializers must be compile-time constants** (see 4); `constexpr` comes later |
| 25 | How to read `const int* p` | C model: `const int*` = pointer to a read-only int; `const int x` = a read-only binding; `T* const` is not provided |
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
| 55 | Heterogeneous value scheme | A library-level Variant first; sum types + `match` later |
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
| 65 | Upper limit of integer literals | Decided: `2^63-1`, exceeding it is an error |
| 66 | Writability checks for `const`/`const`/`string` | Decided: the compiler must reject writes (see 3.7) |
| 67 | Object model | Decided: struct values / class references; RC + weak; no cycle collector (see 3.8 / 8.3) |
| 68 | Scope of `weak` | Class fields only; locals/parameters/return values are forbidden |
| 69 | Semantics of `del` | **Removed (0.0.47)**: `del x;` is equivalent to `x = null;`, it has no separate semantics and has been deleted from the language (the keyword is kept only for migration diagnostics) |
| 70 | Ownership of class parameters | Changed (0.0.47): the parameter is an **owning local variable** (+1 on entry, -1 on exit); it can be rebound inside the function body; it can be optimized later with borrow/escape analysis |
| 71 | Interop between class and C | Passing to `extern` is forbidden; use a struct + alloc across languages |
| 72 | Arrays of class | Decided: both static `Cat[N]` and dynamic `Cat[]` are supported (RC per element) |
| 73 | `sizeof`/`alignof` on a class | Decided (implemented): a class takes the handle size (8), an interface takes the interface value size (16); the object size is not exposed |
| 74 | Multithreaded reference counting | Decided (see 94): atomic RC (relaxed increments, release decrement + acquire fence), objects may be shared across threads; the `--single-thread` non-atomic fast path is still undecided |
| 75 | User destructor `deinit` | Not provided under that name: user destructors are written `~T()` and are implemented (see 3.13 / #92); fields are released in reverse declaration order |
| 76 | Inheritance model | Decided: single inheritance + multiple interface implementations; no multiple inheritance (see 3.9/3.10) |
| 77 | Virtual methods | Decided: virtual by default; overrides/implementations are written `override` |
| 78 | Abstract classes | Decided: having unimplemented methods makes a class implicitly abstract and it cannot be `new`ed |
| 79 | Interface default methods | Decided: allowed; on conflict the class must explicitly override (a default on a derived interface refines the base default); a `struct` implementing an interface materializes the default as an ordinary method (`this` = the struct), see 3.10 |
| 80 | Constructors | Decided: overloading is allowed; `super(args)` comes first; C++-style dispatch; fields are cleared to zero first |
| 81 | Overload resolution | Decided: exact > implicit widening; base-class overloads are merged; ambiguity is an error (see 3.11) |
| 82 | Operator overloading | Decided: free functions only; arithmetic/unary `-`/comparison/bitwise; a class's `==` is fixed as identity comparison |
| 83 | Object layout | Decided: `[rc][class_desc*]`; the desc contains the vtable and itable directories (see 8.3) |
| 84 | Interface value layout | Decided: `{obj*, class_desc*}`, two words; `null` is supported |
| 85 | Arrays/dynamic arrays of class | Supported; the Widget tree still recommends `first_child/next_sibling` |
| 86 | `sizeof`/`alignof` on a class | Decided (implemented): same as #73 (class=8, interface=16) |
| 87 | Multithreaded RC | Decided (same as 94): atomic RC (relaxed increments, release decrement + acquire fence); the thread-local heap scheme has been rejected |
| 88 | Name mangling | Decided: parameter types are included; `extern` keeps the original name; `main` is special |
| 89 | Unary `~` overloading | Allowed (`operator~`, 1 parameter) |
| 90 | Static arrays of class | Decided: allowed (forbidden for structs); retain/release per element |
| 91 | `[]` overloading | Decided: only `operator[]`; return `ref T` for a writable subscript (`operator[]=` was removed in 0.0.101) |
| 92 | User destructors | Decided: `~T()`, executed when the RC reaches zero; derived runs before base |
| 93 | Exception model | Implemented (0.0.81–0.0.83): unchecked, `Exception` base class, class catch, `finally`, module-local may-throw optimization; see 3.14 |
| 94 | RC thread model | Decided: atomic RC (relaxed increments, release decrement + acquire fence), objects may be shared across threads |
| 95 | Generics roadmap | Decided + implemented (0.0.64–0.0.72): monomorphization, `<>` syntax, structural constraints, `Self`; see 3.15 for details |
| 96 | Reserved words | Decided: see 2.5 |
| 97 | Exception unwinding ABI | Resolved by B2 (0.0.81): no platform unwinding ABI; a thread-local pending slot + call-site checks; no `.eh_frame`/`.pdata`/personality dependency |
| 98 | `const` class views / `const` methods | Decided (0.0.61): **explicitly not done** — no const view types are introduced and no const methods are provided; `const` serves only as a **read-only binding marker** for named types (locals/parameters: not rebindable, fields cannot be written directly through them) and as a qualifier for the pointee of pointers/slices; use a read-only `interface` when a read-only API boundary is needed |
| 99 | `ref` parameters / return values | Implemented (0.0.62 for parameters; extended in 0.0.63): `ref` is a binding mode rather than a storable type — `ref T p` / `ref const T p` / `ref T f()`, and no `T&` is introduced; primitive types/enums/pointers/pure-value structs can be borrowed; a returned borrow must originate from a `ref` parameter, `this`, a global, or pointer/slice storage, and the result is not storable (it can only be written through on the spot / passed as a borrow / used with `for ref`); borrowing a local, a by-value parameter, or a temporary is a compile error; method/constructor parameters and `extern` do not support it (see 3.8, backlog U8) |
| 100 | Downcast from an `interface` to a derived `interface` | Decided: not supported in phase one (only upcasts are allowed; `as` is an error) |
| 101 | Non-ASCII character literals | Decided: not supported in phase one (a literal is a single-byte `byte`; use `'\xNN'` or `string`) |
| 102 | Enum comparison | Decided: only between the same enum; `== != < <= > >=` are supported, the order is the order of the underlying integer values; cross-enum or integer comparisons require `as` (see §3.5) |
| 103 | Qualified references to enum members | Decided: `Color.Red` is equivalent to the unprefixed `Red`; across units write `unit.Color.Red`; members are constants (not assignable, and their address cannot be taken) |
| 104 | Conditional expression `?:` | Decided (0.0.46): `cond ? a : b`, right-associative; the condition must be `bool`; only one branch is evaluated; the two sides must have the same type / implicit widening / `null` adaptation, otherwise `as` is required; the result is not an lvalue; see 5.2 |
| 105 | `const` on field declarations | Decided (0.0.59): **instance fields cannot be `const`** (there are no read-only fields, nor `readonly` assigned in a constructor); `const` inside a type body only denotes a **type-level compile-time constant** (no instance storage, accessed via `Type.NAME`), see 3.7.1; `const int x;` is an error |
| 106 | Floating-point `%` | Decided (implemented): legal, the semantics are LLVM `frem` / C `fmod` (the sign follows the dividend) |
| 107 | Unit-level `const` referencing other `const`s/enum members | Implemented (0.0.59): the compile-time constant evaluator supports referencing unit-level `const`s, enum members, and type-level `const` members, and supports forward references; `const int B = A + 1;` and `const int C = sizeof(int) as int;` are legal; cyclic dependencies/division by zero are errors (see 3.7.2) |
| 108 | Type-level `const` constants (`const` members) | Decided (0.0.59): `const T NAME = <const-expr>;` inside a struct/class body, semantically a compile-time constant with no instance storage; accessed via `Type.NAME` / as a bare name inside a type method body / through the base-class chain; `obj.NAME`, `this.NAME`, assignment and taking the address are all errors; not allowed in unions/interfaces (see 3.7.1) |
| 109 | Bit-fields | Decided (0.0.62): **explicitly not done**. No C-style `int x : 3;` is provided, nor is any Zig/Rust-style explicit bit-width syntax; the allocation order and cross-unit boundaries are implementation-defined, which conflicts with predictable layout. Alternatives: mask+shift / `byte[N]`+offset / `packed`+accessors / a C shim (see 3.4) |
| 110 | A generic interface as a value type / a value type as an interface value | **Split (0.0.108).** (a) *Generic interface as a value type*: **implemented** — `IIterator<int> x`, `class C : IIterator<int>`, `class Src<T> : IEnumerable<T>`, interface-valued returns/fields/locals, `T: IEnumerable<int>` constraints, `as` downcasts; two words `{obj, desc}`, itable slots keyed by the concrete signature, no boxing. (b) *Value type → interface value*: **still explicitly not done** (0.0.75 decision stands) — a struct would have to be boxed, so `Animal a = cat;` is a compile error and `struct S : I` is a compile-time contract only |
| 111 | `const` members in generic types / generic interface inheritance | **Generic interface inheritance implemented (0.0.108)**: `interface Derived<T> : Base<T>` instantiates per level and both levels are interface values of their own type. Still an error: `const` members in generic structs/classes (not wired up) |
| 112 | Type alias `type A = B;` | Decided (0.0.62): `type` is a **contextual keyword** (it is parsed as an alias only at the start of a declaration, `type Ident =`; elsewhere it is still an identifier, which protects the `type` field of C-interop structs); each declaration declares one alias; aliases are transparent (not newtypes) (see 4.5) |
| 113 | Range extensions `..=` / `step` / reverse order | Decided (0.0.62): `..=` includes the upper bound; `step` only follows a sequence expression (`0..10 step 2`); reverse order relies on an **explicit negative step**, `a..b` does not reverse automatically; `step 0` is a compile error; the step size is limited to compile-time constants (see 6.4) |
| 114 | Range expansion in array initialization | Decided (0.0.62): in `int[10] a = {1, 2, 5..9, 10};` the `5..9` expands to 5,6,7,8; the endpoints must be compile-time constants; only `..` is supported inside braces (it does not take `..=`/`step`); after expansion it participates in "zero-pad when short / error when too long" (see 4.1) |
| 115 | `switch` extensions | Decided (0.0.62): **explicitly not done** (the expression form/guards/bindings/or-patterns/type branches/destructuring/`case`/fallthrough); the matched type is limited to integers and enums (see 6.6) |
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
| 126 | Unit-level initialization and cleanup | **Implemented (9.1, 0.0.102).** Runtime initializers are allowed; the order is a DFS post-order over `use` edges from the entry unit (declaration order inside `use` lists), cleanup is its exact reverse, declaration order inside a unit. Only `use` dependencies are tracked; indirect dependencies (function pointer, interface dispatch, `extern` C symbol, a later variable reached through a function body) are undefined. An exception escaping an initializer aborts without cleanup; `atexit` makes `exit()` clean up. Lifted the three "no exit-time destruction" restrictions (3.3 `String`, 3.4 reference fields, 3.4 user destructors). Deferred: lazy first-use initialization, explicit `init(N)` priorities, debug-mode lifecycle assertions, link-time sequencing for split compilation |
| 127 | `export` and the binary export surface | **Decided and implemented (0.0.103).** `export` is a top-level declaration modifier that implies `public` and marks the declaration as part of the binary export surface; it cannot be combined with `private`/`extern`/`use` or used inside a type body (export the type, which exports its public methods and class metadata). `--exports=export` (default) keeps only `export` declarations externally visible, `--exports=public` keeps every `public` declaration; `--export <name>` still adds individual names. In an executable everything outside the surface is internalized and dead-code-eliminated; library targets keep the surface visible (toolchain plan §11) |
| 127 | Member promotion (Go-style embedding) / mixin | **Decided (2026-11-14): explicitly not done.** No `mixin`/`embed` member form: a method call never falls back to a field's type. Rationale: (a) it cannot satisfy an interface without an offset thunk, and after such a thunk `this` is the embedded subobject, so identity comparisons and stored `this` silently change meaning; (b) the set of callable names would depend on another type's evolution across units — adding a method to the embedded type silently turns a remote compile error into legal code, and can silently change overload resolution (violates the "information must be locally expressible" criterion); (c) it saves a few lines of forwarding while adding a resolution hop at every call site, whose verification cost is the expensive one. Reuse instead: `struct S : I` with explicit forwarding methods (3.10, 3.15.7), or field composition. Reopen conditions and rejected variants: `agents/design-backlog.md` U40 |

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
  - **Not supported in phase one**: `const` **methods / deep read-only views (decided not to do)**, `ref` parameters, downcasts from an `interface` to a derived interface (see 12 table 98–100);
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
- Dynamic typing is not in phase one; in the future, implement a library-level Variant first, then evaluate sum types + `match`; `dynamic` acts as a separate runtime layer rather than a core type.
- **Concurrency** (3.17): design settled, not implemented. Order: (0) RC memory ordering (release decrement
  + acquire fence before destruction) and the weak-registry lock ordering; (1) runtime primitives (threads,
  mutex/condvar, monotonic clock); (2) `para` / `para for` with one OS thread per section first, plus the
  cursor form of `for-in`; (3) the M:N task runtime (carriers, park/unpark, handoff);
  (4) pollers (IOCP / epoll / kqueue) and the server library. The `para` lowering targets runtime
  primitives, so switching from OS threads to M:N does not change the source.





