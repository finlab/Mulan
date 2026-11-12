# Mulan Language Specification (Draft v0.2)

> Date: drafted on 2026-11-07; the object model (class / single inheritance / interface / overload / constructor) was added on 2026-11-09;
> 2026-11-10: added `switch` and the conditional expression `?:`, and aligned the specification with the implementation for behaviors that were implemented but undocumented (version 0.0.46)
> Status: draft, current through version 0.0.83. The text distinguishes "decided", "recommended", and "undecided"; items that are not frozen can be discussed one by one.
> Positioning: a compact, modern, C-like small language; it retains the core capabilities of C; manual memory management + reference counting for heap objects.
> Non-goals for phase one: multiple inheritance, closures, GC, a preprocessor, reflection. (Generics have landed, see 3.15; exceptions have landed, see 3.14; destructors are specified in 3.13.)

---

## 0. Summary of Discussion Conclusions

| Topic | Conclusion |
|---|---|
| Loops | Only `while` and `for-in` are kept; C-style `for` and `do-while` are not kept |
| Iteration | `for x in seq { }` (parentheses optional); ranges `a..b` / `a..=b` (optionally `step n`), fixed-size arrays, and dynamic arrays are supported; `continue` proceeds to the next iteration |
| Mutability | C#-style: the type is declared before use; mutable by default; `const` is a C-model prefix qualifier: on named types it is a read-only binding marker, on pointers/slices it makes the pointee read-only |
| Declaration initialization | Declarations use only `=`: `Point p = {1, 2}`, `Point p = {}`, `Point p = Point(1, 2)`; in non-declaration positions use `Point(...)` |
| Type inference | Local variables may use `auto x = expr;`; local only, and an initializer is required; `dynamic`/`any` are reserved names only |
| Dynamic types | Not done in phase one; heterogeneous values prefer library-level Variant/sum types; `dynamic`/`any` are reserved only |
| Scope | Declarations do not write `T x(...)`; `Point p` is uninitialized, `= {}` zero-initializes; unit-level variables are declared directly and are `private` by default |
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

---

## 2. Lexical

### 2.1 Source Files

- UTF-8 encoding, extension `.mu`.
- Comments: `//` line comments; `/* ... */` block comments, **nesting is supported** (implemented).
- Statements end with `;`; semicolons are not inserted automatically.

### 2.2 Identifiers

- `[A-Za-z_][A-Za-z0-9_]*`, case-sensitive; non-ASCII identifiers are not allowed in phase one.

### 2.3 Keywords

```
unit use public private const auto struct enum extern class interface new weak packed union
if else while for in switch ref break continue return
true false null as sizeof alignof len alloc realloc free
void bool byte char short ushort int uint long ulong
float double usize isize
and or not override operator super this
```

> `del` was removed in 0.0.47 (it is kept as a keyword only to give a migration diagnostic): to release a reference, write `x = null;`.

> `goto/typedef/fn` are not included for now (function types are written `(T1, T2) => R` with no keyword, and no
> `typedef`-style declarator is introduced; see 3.16). `len/sizeof/alloc/realloc/free/alignof` and the like are built-in identifiers,
> **and must not be shadowed by user declarations** (implemented: using them as a variable/parameter/field name is a hard error).
> `dynamic` / `any` are reserved for future dynamic types / top types and are not keywords in phase one.
> `packed` is a **hard keyword** (a layout modifier, see 3.4, implemented); `align` is a **contextual keyword** — it is parsed as a modifier only when it appears at the start of a type declaration
> and is immediately followed by `(` (`align(8) struct S {...}`); in other positions it may still be used as an identifier (`int align = 4;` is legal).
> If the implementation would rather not maintain contextual keywords, `align` can simply be added to the hard keyword table (there is currently no identifier with the same name anywhere in the repository).
> **`type` is a contextual keyword** (0.0.62): it is parsed as a **type alias** only when `type Ident =` appears at the start of a declaration;
> in other positions it is still an ordinary identifier (so `type` fields in C interop structs are unaffected).
> **`step` is a contextual keyword**: it is parsed as a step size only after the range sequence of a `for` (`for i in 0..10 step 2`); in other positions it is an ordinary identifier.
> **`Self` is a contextual keyword** (0.0.67): it is parsed as the "implementor type" only when it appears in type position inside an `interface` body (see 3.15.6); in other positions it is still an ordinary identifier.

### 2.4 Literals

- **Integer**: decimal, `0x`, `0b`; `_` may be used as a separator; octal is not supported.
- **Float**: decimal point / exponent; `double` by default, the `f` suffix means `float`.
- **Character**: `'a'`; escapes `'\n' '\t' '\\' '\'' '\0' '\xNN' '\u{...}'`; the default type of a literal is `byte` (unsigned 8-bit), adapted to `char`/`byte` according to context.
  - `char` is **signed** 8-bit (C style, −128–127); `byte` is **unsigned** 8-bit (0–255). There is no `ubyte`/`uchar` alias.
  - Literals are interpreted by **bit pattern**: `'\xE4'` is by default the `byte` value 228; assigned to a `char` its value is −28. Use `byte` (or `x as byte`) when an unsigned byte is needed.
  - A character literal denotes a **single byte**, not a Unicode code point: `'a'` is `0x61`; multi-byte characters such as `'中'` are a **compile error** (non-ASCII raw characters are not supported for now).
    `'\u{...}'` only accepts `U+00`–`U+FF` (a single byte); larger code points use the string literal `"\u{4E2D}"` (see below).
  - Unicode text uses `string` (a UTF-8 byte sequence, see 3.3); both `len(s)` and `s[i]` operate on bytes; the element type of `s[i]` and of string for-in is `byte`.
- **`\u{...}` escape** (shared by character and string literals):
  - The form is `\u{` + 1–6 hexadecimal digits + `}` (either case is allowed, leading zeros are allowed, e.g. `\u{4E2D}`, `\u{1f600}`); the four-digit `\uXXXX` form is not supported.
  - The value range is `U+0000`–`U+10FFFF`; **the surrogate range `U+D800`–`U+DFFF` and values outside the range are compile errors**.
  - Written in a string literal it is encoded as **UTF-8** into 1–4 bytes (`"\u{4E2D}"` has the same bytes as `"中"` in source code); the NUL produced by `\u{0}` is an **error** just like `\0` (a string literal cannot contain NUL).
  - Written in a character literal it requires a code point ≤ `U+FF` (a single-byte bit pattern); otherwise it is an error and the compiler suggests using a string instead.
- **Unicode code point type = `int`** (i32): the range is 0–`0x10FFFF`, arithmetic and comparison work naturally, and `-1` can serve as an invalid sentinel; **no** types or aliases such as `rune` / `unichar` / `char32` are **introduced**.
  APIs that iterate a string by code point (`s.chars()` and the like) belong to the string library and are designed together with the string type (see backlog U3); they will return `int`.
- **String**: `"..."`, UTF-8, statically stored at compile time, NUL-terminated; the type is the std library's `string` value type (see 3.3, for the implementation see the library documentation), implicitly convertible to `const char*`; supports `\u{...}` (see above).
- **Boolean**: `true` / `false`.
- **Null**: `null` (not 0); it can be assigned to pointers and class references, but cannot be used for structs/scalars.

### 2.5 Reserved Words (future extensions, not usable as identifiers for now)

The following identifiers are reserved for future features; the compiler should reject their use as variable/function/type names:

```
where  try  catch  throw  finally
async  await  spawn  shared  atomic
move  unsafe  typeof
```

- Generics: `where`;
- Exceptions: `try/catch/throw/finally`;
- Concurrency: `async/await/spawn/shared/atomic`;
- Others: `move/unsafe/typeof`.
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
- Operations: `&expr`, `*p`, `p[i]`, `p + n`, `p - n`, pointer comparison, pointer subtraction.
- `null` can only be assigned to a pointer or a class reference (see 3.8); `p == null` is legal.
- `T*` implicitly converts to `void*`; `void*` to `T*` requires an explicit `as`.
- `T*` implicitly converts to `const T*`; the reverse requires an explicit `as` (unsafe).
- Pointer arithmetic is in units of `sizeof(T)`; arithmetic on `void*` is not allowed.
- Function pointers are written `(T1, T2) => R`: a one-word value, binary compatible with a C function pointer,
  with no declarator nesting and no `*`; see 3.16 (implemented in 0.0.84).

### 3.3 Arrays and dynamic arrays

- Fixed-size arrays: `T[N]`, where `N` is a compile-time integer constant; a **value type**, copied on assignment/argument passing.
- Dynamic arrays: `T[]`, a **reference-counted heap array**; the handle is a pointer to an internal header
  `{ i64 rc; i64 len; i64 cap; data... }` (`cap` is reserved for future growth).
  - `new T[n]` creates one (`n` is a runtime integer, elements zeroed); `T[] s = {}` means empty (null handle).
  - Assignment/argument passing copies the **handle** (O(1)), sharing the same array; destroyed when the reference count reaches zero;
    if the element type holds class/interface references, release element by element.
  - Element types: scalars, enums, pointers, structs (including reference fields), class/interface references;
    structs with user destructors are not yet allowed as elements (element construction/destruction rules are undecided).
  - `len(s)`: 0 for a null handle; `s[i]` indexing; `for x in s` iterates over the elements.
  - `for ref x in s` can write to a shared array; **not allowed when the element is a class/interface** (the element is a handle, iterate with value binding);
    `string` also cannot use `for ref` (immutable).
  - Null value: `T[] s = null;` and `T[] s = {};` are equivalent (both null handles); but **a slice cannot be compared with `null`**
    (use `len(s) == 0`).
  - Releasing a dynamic array: setting the handle to empty (`s = {}` / `s = null`) releases it once; `del` was removed (0.0.47).
  - `sizeof(T[])` / `sizeof(T[,])` = 8 (handle size); the size of library types is defined by the library (for `string` see the library docs).
  - Aggregate initialization `T[] s = {1, 2, 3};` allocates a new array and initializes the elements.
  - `T[N]` → `T[]` implicit conversion: **allocates and copies** (dynamic arrays no longer provide a borrowed view).
  - Sub-slicing/borrowed-view syntax (`s[1..]` etc.) is not provided yet.
- **Static rectangular multi-dimensional arrays**: `T[m,n,...]` (`m,n` compile-time constants), contiguous memory, value type.
  - `int[2,3] a = {{1, 2, 3}, {4, 5, 6}};` (nested braces, missing elements filled with zero).
  - Indexing `a[i,j]`, equivalent to the chained form `a[i][j]`; `a[i]` yields a row value of type `T[n]`.
  - `len(a)` = dimension 0, `len(a,k)` = dimension k (k must be a literal).
  - May contain class/interface/struct reference elements (RC element by element); `sizeof` is the total byte count.
- **Dynamic rectangular multi-dimensional arrays**: `T[,]` (only the rank is written; the size is determined by `new`).
  - `int[,] a = new int[m,n];`; any `new T[d0,d1,...]`.
  - Indexing `a[i,j]` (all dimensions must be given; taking a "row view" is not supported).
  - `len(a)` = dimension 0, `len(a,k)` = dimension k (k may be a runtime expression; dynamic arrays are bounds-checked).
  - **`for-in` is not supported** (iterate multi-dimensional arrays with index loops; use `len(a, k)` for each dimension's length, see 6.4).
  - Conversion: `T[m,n,...] → T[,]` allocates and copies.
- **Jagged arrays**: `T[][]` (the element is itself a dynamic array); rows may have different lengths.
  - `new T[m][]` creates `m` empty row slots (elements are null `T[]`), then loop
    `a[i] = new T[n_i];`.
  - `new T[m][n]` is **not** legal syntax: a jagged array creates only one dimension at a time; for rectangular arrays use
    `new T[m,n]`. Static jagged arrays can use fixed-size row slots `T[][N]` or aggregate initialization.
- All-zero: `int[5] c = {};`; empty dynamic/rectangular arrays: `int[] d = {};`, `int[,] e = {};`.
- Arrays do **not implicitly decay to pointers**; take the first address with `&arr[0]`.
- Indexing `x[i]` / `x[i,j]`; the index type is an integer, `usize` recommended.
- `len(x)`: fixed-size arrays return a compile-time constant, dynamic arrays return the runtime length, both `usize`.
- Bounds checking: TBD (recommended in debug builds, configurable off in release; dynamic arrays checked by default).
- All-zero/null values use `{}`: `int[5] a = {};`, `int[] s = {};`, `string s = {};` (aggregate types no longer support `= 0`).
- **`string` is a value type provided by the std library** (not a language built-in type; for layout, buffer management, and the full API see
  `lib-std-string.md`):
  - **Language-visible contract** (the part SPEC guarantees):
    - The type of the literal `"abc"` is `string`; `string s = {}` is the empty string (zero value).
    - **Immutable**: cannot be traversed with `for ref`, `s[i]` is read-only.
    - The element type of `s[i]` is `byte`; `len(s)` is the **byte length**; `for c in s` iterates by byte.
    - `==` / `!=` compare by **byte value** (not identity); the zero value equals `""`.
    - Cannot be compared with `null` (use `len(s) == 0` or `s == ""`).
    - `string → const char*` / `const byte*` implicit conversion (requires NUL termination; guaranteed for whole strings/literals).
    - Usable as struct/class fields, array elements, parameters, return values; **copyable and destructible** (value semantics, copies are independent).
  - Size: `sizeof(string)` is defined by the library and **no fixed value is promised**; to pass strings across the ABI use `const char*`.
  - Construction and operations (`string(const char*)`, `length()`, `at`, `sub`, `concat`, `find`, `trim`, etc.)
    are **library API**, see the library docs; they are not part of the language specification.
  - Implementation status: the compiler still has built-in special cases for `string` (zero-allocation literals, direct wiring of `len`/`for-in`/`[]`/`==`,
    implicit conversion to `const char*`), which is an **implementation optimization**, not a language mechanism; the prerequisite for "true library-ization" is user destructors on structs and
    copy/assignment semantics (leftover from backlog U24).
- Initializing a fixed-size `char[N]` from a `string` literal performs a byte copy: copies the content and appends NUL and zeros.

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
- Can declare **methods** (`ret name(params) { ... }`): static dispatch, no vtable, no inheritance/virtual functions;
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
  - Structs with reference fields cannot yet be global variables (no exit-time destruction);
  - `extern C` signature recursive check: structs containing class/interface cannot be C parameters/return values;
  - Structs with **user destructors** still cannot be copied/assigned/passed/returned by value, nor be global variables, array elements,
    or fields of other structs/classes (the interaction between user destructors and automatic field RC is to be implemented later); pass them through pointers.
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

### 3.5 Enums (proposed)

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
  - `T[N]` → `T[]`, `T[m,n,...]` → `T[,]` (array to dynamic array: allocates and copies).
- All other numeric/pointer conversions must be written with `as`.
- Enums and integers (excluding floating point) can only be converted explicitly with `as` in both directions; enums do not participate in implicit widening, nor implicitly convert to `int`.
- Integer literals adapt to the context type (if representable); the literal upper bound is `2^63-1`, and exceeding it is a compile error.
- Floating-point literals likewise adapt to the context: `float f = 1.5;` is legal; but `double` → `float` between **variables/expressions** still requires an explicit `as`.
- Integer arithmetic: types smaller than `int` are promoted to `int` first; mixed sign/width follows the rules above, and it is an error when no implicit conversion is possible.
- `bool` does not participate in numeric conversions; conditions must be `bool`.
- `string` → `const char*` / `const byte*` is implicit (returns `p`, requires NUL termination).
- `string` is not a numeric type and cannot be converted implicitly to/from pointers/integers.
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
- `string → const char*` / `const byte*` is implicit (returns `p`; requires NUL termination).
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
- Unary `-` `+` `not` `!`; binary `+ - * / %`, `& | ^ << >>`, `< <= > >= == !=`, `and` `or`;
- `as` conversions (the same truncation / sign extension / integer↔float conversion semantics as at runtime);
- References to other constants: unit-level `const` of this unit, enum members (`Color.Red` or a bare `Red`),
  type-level `const` members (`Type.NAME`); **forward references and arbitrary declaration order are supported** (`const int A = B; const int B = 1;`);
- Built-in `sizeof(T)` / `alignof(T)`: the result is `usize`; assigning it to `int` and similar types requires an explicit `as`
  (`const int W = sizeof(int) as int;`; `const int W = sizeof(int);` still fails under the strict typing rules);
- Errors: division by zero / modulo by zero, circular dependency (`circular constant definition involving ...`), nesting depth exceeding 512 levels;
- In constant expressions `and` / `or` **evaluate both sides** (at runtime they short-circuit); therefore `false and (1 / 0 == 0)` reports division by zero in a constant context;
- Despite forward references and arbitrary declaration order, constants of the same name coming from `use` still **report ambiguity** under the normal name-resolution rules (they do not silently take the first);
- Not supported: function calls (there are no `constexpr` functions), `?:` conditional expressions, pointers/strings/aggregate values participating in evaluation.
  String and aggregate constants follow the old rules (`const string S = "hi";`, `const int[] G = {10, 20, 30};` continue to work).
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
- Memory: atomic reference counting (relaxed); fields are released in reverse declaration order; weak references are not counted.
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
- A default method may only call interface methods and cannot access fields; when two interfaces provide default implementations with the same signature, the class must explicitly override them, otherwise it is a compile error.
- An interface value is laid out as two machine words `{object pointer, class descriptor pointer}`; it may be `null`.
- Conversions: `class → interface` is implicit; `interface A → interface B` (A inherits B) is a static conversion;
  `class → derived class` and `interface → class` use `as` for a runtime check (along the desc base class chain), yielding `null` on failure;
  the downcast `interface → derived interface` is not supported in phase one (error).
- Interface references are strong references: copying is +1, overwriting is -1; `weak` is only supported for class fields, and **interfaces do not support `weak` (not done in phase one)**.
- An interface cannot be used as a parameter of an `extern` C function.
- Generic interfaces (`interface ISeq<T>`) are supported (see 3.15), but are **currently only usable as constraints** and cannot be value types.
- A `struct` may also implement interfaces (`struct S : I1, I2`): after `:` you may **only list interfaces**, and all methods must be implemented explicitly with
  `override`; this is **static satisfaction**, which produces no value type and generates no vtable (see 3.15.7 and backlog U33).
- `for-in` does not depend on interfaces: it follows the **iteration form** of 6.4 (structural recognition, static, zero allocation).
  Interface-driven traversal requires "interfaces as value types" (M5b), see backlog U30.

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

**Operator overloading** (only the free function form is allowed):

```c
Point operator+(Point a, Point b) { ... }
bool  operator==(Point a, Point b) { ... }
Vec   operator<<(Vec v, int n) { ... }
```

| Operator | Allowed | Description |
|---|---|---|
| `+ - * / %` | Yes | Binary; may return any type |
| Unary `-` | Yes | |
| `< <= > >=` | Yes | Must return `bool` |
| `& \| ^ << >>` | Yes | Bitwise operations |
| `[]` / `[]=` | Yes | **Explicit read/write**: `T operator[](C c, I i)` reads; `void operator[]=(C c, I i, T v)` writes. `c[i]` calls the former, `c[i] = v` calls the latter, `c[i] += v` reads first and then writes. **Recommended**: `ref T operator[](ref C c, I i)` returns a borrow, so `c[i]` is directly a writable lvalue (in which case `[]=` is no longer needed, and `+=`/passing a `ref` parameter/`for ref` all work as well) |
| `== !=` | struct only | A class's `==` is fixed as identity comparison and cannot be overloaded; a struct that does not define `==` is not comparable |
| `()` `=` `&& \|\|` `as/new` | No | Conflicts with the built-in semantics |

- An operator must have at least one struct/class operand; built-in types cannot be redefined.
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

`ISeq<int> x;` and `Animal a = cat;` are both compile errors. If borrow checking is introduced in the future, a zero-cost borrowed interface of the `ref I` form can be reconsidered
(see backlog U33 "Final Decision" and U34 M5b for details).

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

  Rationale: "declaration vs expression statement" stays a single-token decision (a declaration starts with a keyword
  or an identifier), and `(int) => int f` would read like a cast.
- `const` before a function type marks the **binding** read-only (like `const int x`). A function has no pointee, so
  there is no "read-only function" qualifier.

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

#### 3.16.5 Implementation status

Implemented in 0.0.84 for the whole path — lexer (`=>`), parser (`(`-started parameter-type list, lambda detection
by the matching `)` followed by `=>`), the `Func` type kind (canonical spelling `(int, int) => int`, mangling
`fn_int_int__int`, 8-byte layout), sema (function names as values, overload selection by target type, calls through a
value, lambda lowering and single-pass result-type inference), and codegen (`ptr` + an indirect call; each lambda is a
hidden `FuncDecl` emitted with the unit).

Not implemented: capturing closures (a future `delegate`), method values, generic function values
(`max2<int>` as a value), and anonymous function types in declaration positions (a unit-level variable, a local
declaration and a return type must name the type with `type` first).

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
  literals, unary `-`/`+`/`not`/`!`, `as`, arithmetic/bitwise/comparison/`and`/`or`, and **references to other constants**
  (unit-level `const`, enum members, type-level `const` members) as well as `sizeof(T)` / `alignof(T)`;
  forward references are supported; `const int B = A + 1;`, `int g = 1 + 2;`, `int G = Cat.MaxAge;` are all legal. Cyclic dependencies and division by zero are errors.
  String/aggregate constants are still written as literals or aggregate initialization (`const string S = "hi";`, `const int[] G = {10, 20, 30};`).
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
| 1 | `()` `[]` `.` `as` | left |
| 2 | `-` `+` `&` `*` `~` `sizeof` `alignof` `len` | unary (prefix) |
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
- `len(seq)` operates on arrays/slices.
- Built-in functions: `alloc(bytes: usize) -> void*`, `realloc(p: void*, bytes: usize) -> void*`, `free(p: void*)`.
- Struct construction expression: `Point(1, 2)`, `Point(x = 1, y = 2)`, `Point()` (zero value); brace initialization is not an expression.
- `new T(...)` is an expression (`T` must be a class) and returns a class reference; there is no corresponding release statement (`del` has been removed; just set it to null).
- `0` in an expression is an `int`; zero-value initialization (aggregate `{}` / scalar constants) is only used in the declaration-initializer position. For a struct's zero value in an expression/assignment use `Point()`; you cannot write `p = {}`, `p = 0`, or `f(0)`.
- A string literal has type `string`; it converts implicitly to `const char*`; `"abc"[0]` is legal.

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
- Built-in sequences: half-open range `a..b`, fixed-size arrays, slices, `string` (iterated byte by byte).
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
  - a free-function operator `operator[](C, usize) -> T`.
- Signatures are matched **exactly** on `usize`: `operator[](C, int)` does not qualify (no narrowing conversion); on error the actual signature is reported.
- Lowering: `usize n = c.length();` (evaluated only once) → `for (usize i = 0; i < n; ++i) { T x = c[i]; ... }`.
- The element type is the return type of `operator[]` (if it returns `ref T`, the element type is `T`); **proxy/reference wrappers are not recognized**
  (a `vector<bool>`-style proxy design is not supported).
- `length()` must equal the number of valid indices of `[]`; a mismatch is a user error (debug bounds checks catch it).
- In `for i, x`, `i` is the real `usize` index.
- `for ref` requires `operator[]` to return `ref T` (the element is then addressable); when `[]` returns a value, use `for i, x` + `c[i] = ...`.
- **Copy cost**: `operator[](C, usize)` is an ordinary call; when `C` is a struct, the whole struct is copied on every iteration
  (reference fields are retained/released field by field). Iterable containers should preferably be `class`, or the receiver should be written as `ref C`.
- A temporary container (`for x in makeVec()`) is constructed only once and cleaned up at loop exit according to U24.

#### 6.4.2 Cursor form (sequential access; not implemented yet)

```c
struct Iter {
    bool next() { ... }      // advance; false = end
    int  current() { ... }   // current element
}
class List { Iter iterator() { ... } }
```

- Conditions: a method `iterator() -> It`, and `It` has the methods `next() -> bool` and `current() -> T`.
- Lowering: `auto it = c.iterator(); while it.next() { T x = it.current(); ... }`; `iterator()` is evaluated only once.
- The element type is the return type of `current()`.
- The cursor is a struct returned by value: zero allocation, static dispatch, inlinable; with class/interface fields it does per-field RC according to U24,
  cleaning up at loop exit / `break` / `return`.
- The cursor form does not support `for ref` (`current()` returns a value); mutable iteration goes through the index form (`[]` returning `ref T`) or `for i, x`.

#### 6.4.3 Resolution order and diagnostics

1. Built-in sequence → built-in lowering (see the start of this section); it does not participate in form matching;
2. Satisfies the index form → index lowering (**implemented**);
3. Otherwise satisfies the cursor form → cursor lowering (not implemented yet);
4. Both are satisfied → the **index** form is used and a warning is emitted (the semantics are determined by `[]`; this lands together with the cursor form);
5. Neither is satisfied → a compile error listing the missing members of both sets (currently only the missing items of the index form are reported).

- `len(e)` serves **only** built-in types (fixed-size arrays/slices/multidimensional arrays/`string`) and does **not** dispatch to user types;
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
  | `string` | the underlying `const char*` (for `printf("%s", s)`) |
  | pointer / POD struct / union | unchanged |

- Arguments to `...` do not participate in reference counting: types that contain RC (class/interface references, dynamic arrays, structs with reference fields or with a user destructor)
  are a **compile error** as variadic arguments.
- No runtime type checking: `printf("%d", 1.5)` is UB as in C (the caller guarantees that the format string matches the argument types).
- When a "variadic API internal to Mulan" is needed, use `Value[]` (a tagged union, see 10.1); do **not** introduce a `va_arg`-style mechanism.
- `va_list` / `va_start` / `va_arg` / `va_copy` are not provided: forwarding C's `vprintf` family or exposing variadic functions to C
  is a different class of feature; see backlog U31.

---

## 8. Memory management

### 8.1 Built-in functions

| Function | Semantics |
|---|---|
| `alloc(bytes: usize) -> void*` | Allocates uninitialized memory; returns `null` on failure |
| `realloc(p: void*, bytes: usize) -> void*` | Resizes; returns `null` on failure, leaving the original block unchanged |
| `free(p: void*)` | Frees; `free(null)` is a no-op |
| `sizeof(T) -> usize` | Type size |
| `alignof(T) -> usize` | Type alignment |
| `len(seq) -> usize` | Length of an array/slice/string (**built-in types only**; it does not dispatch to user types; user containers use `length()`, see 6.4.3) |

- `alloc` returns memory aligned to at least 16 bytes (consistent with common malloc); this is a **lower-bound guarantee**, not an upper bound.
  For heap restrictions on over-aligned types see 3.4 (class objects are allocated by the compiler with the required alignment; dynamic array elements have `align ≤ 8`;
  a manually `alloc`ed struct instance with `align > 16` is UB). A general `alloc_aligned` is still pending (backlog U27).
- No GC, no ownership, no borrow checking; leaking is the programmer's responsibility.

### 8.2 Undefined behavior (UB)

- Using freed memory, or double-freeing (`free(null)` excepted).
- Dereferencing `null`, out-of-bounds pointer arithmetic/dereference, dangling pointers.
- Reading uninitialized memory.
- Misaligned access: a pointer/field whose dereference location does not satisfy the type's natural alignment (fields of a `packed` struct are such a case; see 3.4).
- Data races (phase one has no language-level concurrency).
- Division by zero; a shift count ≥ the bit width (pending: UB or defined).
- Signed integer overflow: **settled: defined as two's-complement wraparound** (avoids UB and simplifies optimization; the backend does not emit `nsw`/`nuw`).
- Array/slice out of bounds: pending (suggested: checked in debug builds, may be disabled in release; or always checked).
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
- Every class object carries an **atomic** strong reference count (relaxed); after `new` creates it the count is 1.
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
  - Backend: non-`public` top-level symbols use `internal` linkage (`public` / `extern` / `main` keep external linkage).
  - **Visibility is a compile-time API contract, not a security boundary** (`void*`/`as` can bypass it).
- No preprocessor; module-level constants use top-level `const`.
- `extern` is used for C functions/global variables; C ABI; struct layout is compatible with C. `extern` functions cannot be overloaded.
- Class objects cannot be passed to C: the C side does not participate in reference counting; when crossing languages use a struct + `alloc`.
- A string literal is a NUL-terminated `const char*` and can be passed to C directly.
- Symbol mangling/naming conflict rules are pending.

---

## 10. Phase-one non-goals

- Multiple inheritance (class allows single inheritance only)
- Dynamic arrays / container classes (to be provided later as reference types)
- Platform unwinding tables / personality routines (exceptions use the B2 TLS-flag lowering, 3.14)
- Capturing closures (non-capturing lambdas and function values are in scope, see 3.16)
- GC/ownership/borrow checking
- Unrestricted operator overloading (only the limited set in 3.11 is allowed)
- Exponentiation operator (`^` is reserved for bitwise xor; use the `pow` function for exponentiation)
- Preprocessor/macros
- RTTI/reflection
- Language-level concurrency
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
| Arrays | Degenerate to pointers, mixed value semantics | Fixed-size arrays have value semantics; slices are explicit |
| Strings | `char*` | std.string `string` (UTF-8; NUL guaranteed only for complete strings/literals) |
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
| 4 | Layout of `string` and conversion to `byte[]` | Decided: `string` is a **value type provided by the std library**, and the layout/buffer/RC/API are library implementation details (`lib-std-string.md`); the language only guarantees the visible contract of §3.3 |
| 5 | Whether arrays implicitly decay to pointers | No decay |
| 6 | Whether dynamic arrays are included in phase one | Decided: included (`T[]` / `T[,]`, RC heap arrays) |
| 7 | Bounds checking strategy | Checked in debug; configurable in release |
| 8 | Signed overflow | Decided (implemented): two's-complement wraparound (the backend emits no `nsw`/`nuw`) |
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
| 19 | Details of the conversions between `string` and `byte[]`/`char[N]` | `string` is an immutable view; `char[N]` initialization copies byte by byte; the remaining conversions are library APIs (see the library documentation) |
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
| 62 | `string` comparison semantics | Decided: `==`/`!=` compare byte values; no comparison with `null`; the zero value equals `""` (a library type contract, not a language special case) |
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
| 74 | Multithreaded reference counting | Decided (see 94): atomic RC (relaxed), objects may be shared across threads; the `--single-thread` non-atomic fast path is still undecided |
| 75 | User destructor `deinit` | Not done in phase one; fields are released in reverse declaration order |
| 76 | Inheritance model | Decided: single inheritance + multiple interface implementations; no multiple inheritance (see 3.9/3.10) |
| 77 | Virtual methods | Decided: virtual by default; overrides/implementations are written `override` |
| 78 | Abstract classes | Decided: having unimplemented methods makes a class implicitly abstract and it cannot be `new`ed |
| 79 | Interface default methods | Decided: allowed; on conflict the class must explicitly override |
| 80 | Constructors | Decided: overloading is allowed; `super(args)` comes first; C++-style dispatch; fields are cleared to zero first |
| 81 | Overload resolution | Decided: exact > implicit widening; base-class overloads are merged; ambiguity is an error (see 3.11) |
| 82 | Operator overloading | Decided: free functions only; arithmetic/unary `-`/comparison/bitwise; a class's `==` is fixed as identity comparison |
| 83 | Object layout | Decided: `[rc][class_desc*]`; the desc contains the vtable and itable directories (see 8.3) |
| 84 | Interface value layout | Decided: `{obj*, class_desc*}`, two words; `null` is supported |
| 85 | Arrays/dynamic arrays of class | Supported; the Widget tree still recommends `first_child/next_sibling` |
| 86 | `sizeof`/`alignof` on a class | Decided (implemented): same as #73 (class=8, interface=16) |
| 87 | Multithreaded RC | Decided (same as 94): atomic RC (relaxed); the thread-local heap scheme has been rejected |
| 88 | Name mangling | Decided: parameter types are included; `extern` keeps the original name; `main` is special |
| 89 | Unary `~` overloading | Allowed (`operator~`, 1 parameter) |
| 90 | Static arrays of class | Decided: allowed (forbidden for structs); retain/release per element |
| 91 | `[]` overloading | Decided: `operator[]` reads, `operator[]=` writes; explicitly distinguished, no `&`/`const` |
| 92 | User destructors | Decided: `~T()`, executed when the RC reaches zero; derived runs before base |
| 93 | Exception model | Implemented (0.0.81–0.0.83): unchecked, `Exception` base class, class catch, `finally`, module-local may-throw optimization; see 3.14 |
| 94 | RC thread model | Decided: atomic RC (relaxed), objects may be shared across threads |
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
| 110 | A generic interface as a value type / a value type as an interface value | **Decided (0.0.75): explicitly not done**. Without borrow checking, a value type → interface value necessarily boxes (heap allocation + RC), at a cost comparable to `new`ing a class object, and it loses explicit lifetimes and downcasting; runtime polymorphism is fully covered by `class`, and heterogeneous value collections use sum types (10.1). Generic interfaces serve only as constraints (3.15.7); `struct S : I` provides only static satisfaction (U33). If borrow checking is introduced in the future, zero-cost borrows of the `ref I` form can be re-evaluated |
| 111 | `const` members in generic types / generic interface inheritance | Undecided: `const` members of generic structs/classes and `interface IList<T> : ISeq<T>` are errors for now |
| 110 | Type alias `type A = B;` | Decided (0.0.62): `type` is a **contextual keyword** (it is parsed as an alias only at the start of a declaration, `type Ident =`; elsewhere it is still an identifier, which protects the `type` field of C-interop structs); each declaration declares one alias; aliases are transparent (not newtypes) (see 4.5) |
| 111 | Range extensions `..=` / `step` / reverse order | Decided (0.0.62): `..=` includes the upper bound; `step` only follows a sequence expression (`0..10 step 2`); reverse order relies on an **explicit negative step**, `a..b` does not reverse automatically; `step 0` is a compile error; the step size is limited to compile-time constants (see 6.4) |
| 112 | Range expansion in array initialization | Decided (0.0.62): in `int[10] a = {1, 2, 5..9, 10};` the `5..9` expands to 5,6,7,8; the endpoints must be compile-time constants; only `..` is supported inside braces (it does not take `..=`/`step`); after expansion it participates in "zero-pad when short / error when too long" (see 4.1) |
| 113 | `switch` extensions | Decided (0.0.62): **explicitly not done** (the expression form/guards/bindings/or-patterns/type branches/destructuring/`case`/fallthrough); the matched type is limited to integers and enums (see 6.6) |
| 114 | Function type syntax | Decided and implemented (3.16, 0.0.84): `(T1, T2) => R`; a one-word pointer value with no `*` and no declarator nesting; `sizeof` = pointer size; `=>` has lowest precedence and is right-associative; `*`/`[]` bind to the return type; block-/unit-level declarations never begin with an anonymous function type and a return type is never anonymous (use `auto` or a `type` alias), while fields and parameters may |
| 115 | Lambdas | Decided and implemented (3.16, 0.0.84): `(int a) => expr` or `(int a) => { ... }`; parameter types are always written; the return type is inferred from the body (all `return` types must be identical; no `return expr` means `void`); **no captures**; each lambda is a hidden `private` function (C ABI, no allocation); stateful callbacks use `class` + single-method `interface` |
| 116 | Free function / method values | Decided and implemented (3.16, 0.0.84): a free function name is already a function value (no `&`, and `&f` is an error); overloaded names need a target type; generic function names cannot be used as values; `u.f` and `extern` functions are values; **methods are not values** |
| 117 | Capturing closures | Decided: phase-one non-goal; if added later, a separate two-word `delegate` type (`{code, env}`), never a fatter `fn` |

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
- Unit-level variables are zero-initialized by the loader/startup code.
- `auto` only takes the expression type directly in the front end; multiple declarators require identical inference results; no runtime overhead.
- Dynamic typing is not in phase one; in the future, implement a library-level Variant first, then evaluate sum types + `match`; `dynamic` acts as a separate runtime layer rather than a core type.





