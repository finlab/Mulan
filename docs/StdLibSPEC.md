# Mulan Standard Library Specification (Draft v0.1)

> **Related files**
> - This file: [docs/StdLibSPEC.md](file:///D:/dev/mu_lan/docs/StdLibSPEC.md)
> - Language specification: [docs/SPEC.md](file:///D:/dev/mu_lan/docs/SPEC.md)
> - Version history: [docs/versions.md](file:///D:/dev/mu_lan/docs/versions.md)
>
> Internal design records (not published with the repository): `agents/stdlib/policy.md`,
> `agents/stdlib/c-bindings.md`, `agents/stdlib/interfaces.md`, `agents/stdlib/style.md`,
> `agents/stdlib/task.md`, `agents/stdlib/backlog.md`.

> Status: draft, current through build 270 (2026-11-20). Companion to the language specification:
> `docs/SPEC.md` says what a program *means*; this file says what the library *provides*.
> **Boundary test**: delete the library, and a program that uses none of it must still be fully
> specified by `docs/SPEC.md` alone.

---

## 1. Layers

A unit belongs to exactly one layer. The unit-name prefix makes the layer visible at the `use` site.

| Layer | Units | Contract | Status |
|---|---|---|---|
| **L0 ABI types** | `std.ctypes` | C-side widths, per platform | implemented |
| **L1 raw C bindings** | `cstd.stdio`, `cstd.stdlib`, `cstd.string`, `cstd.ctype`, `cstd.math`, `cstd.time`, `cstd.locale`, `cstd.errno`, `cstd.limits`, `cstd.floats`, `cstd.stdint`, `cstd.inttypes`, `cstd.fenv` | a **separate library** (`libs/cstd/`): C semantics — sentinel returns, `errno`, caller-owned buffers, no checks, no exceptions, no ownership | implemented for Windows/UCRT and Linux/glibc |
| **L1 platform C** | `#if` arms inside the same `cstd.*` unit files | the same C library where the targets disagree; the shared declarations are written once and only the differing ones sit in an arm | Windows and Linux implemented; other targets fail with `#error` |
| **L2 native API** | `std.string`, `std.option`, `std.result`, `std.mem`, `std.fmt`, `std.io`, `std.collections`, `std.fs`, `std.parse`, `std.random`, `std.unicode`, `std.time` | Mulan semantics: exceptions, bounds checks, explicit state, value types where possible | `string`, `option`, `result`, `mem` implemented, the rest planned |
| **L3 runtime contracts** | `std.exception`, `std.interfaces`, `std.task`, `std.thread` | The minimum the compiler and the runtime need; implicitly loaded | implemented |

Rules:

1. **L1 never helps.** A raw binding neither checks a length, nor converts an error into an
   exception, nor takes ownership of a pointer. It is C with a calibrated signature. The moment a
   raw binding starts "fixing things up", both layers become untrustworthy.
2. **L2 is the default.** New code uses L2. L1 exists for interoperability and for code that is
   deliberately C-shaped.
3. **L3 is loaded implicitly** (`std.string`, `std.exception`, `std.interfaces`, `std.task`,
   `std.thread`); every other unit needs an explicit `use`. Implicit loading is a language decision
   (SPEC 9), so its list is also recorded there.
4. **Extensions are not ISO C.** POSIX and Win32 surface goes into `std.posix` / `std.win32`, never
   into `std.c*`: the `c` layer promises the ISO C surface, no more.

## 2. Naming

The full naming rules for library authors are an internal guide (`agents/stdlib/style.md`). The parts
that matter for the layer model:

- Unit names are lowercase, dot-separated; the path mirrors the name (`std.string` →
  `libs/std/string.mu`).
- **Platform differences are written in the unit, not in the file tree.** A unit that differs between
  targets keeps one file: the shared declarations are written once and only the differing ones sit in an
  `#if` arm (`cstd.stdio`, `cstd.time`). Every unit with such an arm opens with a guard
  (`#if os != OS.Windows and os != OS.Linux { #error "…" }`) and a `use std.comptime;` *before* its first
  `#if` — a declaration-list condition only sees the constants visible at that source position.
  `libs/std/platform/<os>/` (the loader-level shadow directory) is the older form, still used by
  `std.ctypes` and `std.thread.backend`.
- The `cstd` library holds the raw C bindings (`cstd.stdio`, `cstd.string`, …); `std` itself never
  contains a C-header binding.
- A **library** type is PascalCase (`String`, `Thread`), a **built-in** type is lowercase
  (`int`, `T[]`); `string` is a declared lowercase alias of `String`.
- A free function does not repeat its module name: `std.thread.start`, not `std.thread.thread_start`
  (the current `thread_start` / `thread_join` / `thread_sleep` names are a pending cleanup, U37).
- `extern` declarations keep the C name verbatim; wrappers follow the Mulan rules.

## 3. Unit inventory

| Unit | Layer | Implicit | Public surface | Details |
|---|---|---|---|---|
| `std.ctypes` | L0 | no | `c_char` … `c_voidp` | §5, §7 |
| `cstd.stdio`, `cstd.stdlib`, `cstd.string`, `cstd.ctype`, `cstd.math`, `cstd.time`, `cstd.locale`, `cstd.errno`, `cstd.limits`, `cstd.floats`, `cstd.stdint`, `cstd.inttypes`, `cstd.fenv`, `cstd.streams` | L1 | no | the ISO headers, one unit per header, in the separate `cstd` library; platform differences live in `#if` arms inside the unit (`windows` = MSVC CRT, `linux` = glibc) | §5, §7 |
| `std.string` | L2 | **yes** | `String`: `String()` (zero), `String(const char*)` (implicit, copies), `String(byte[])`, `String(int[])` (code points), `length`, `is_empty`, `operator[]`, `==`, `sub`, `char_slice`, `concat`, `repeat`, Python-style byte methods (`find`, `rfind`, `count`, `startswith`, `endswith`, `replace`, `split`, `join`, `strip_ascii`, `lower_ascii`, `upper_ascii`, `is_*_ascii`), UTF-8 structure (`is_valid_utf8`, `char_count`, `char_at`, `chars`, `bytes`, `codepoints`), `cstr`, `strlen` (plus the lowercase `string` alias) | SPEC §3.3, §6.3 |
| `std.unicode` | L2 | no | `IUnicode` extension on `String`, active only where this unit is used: `lower`, `upper`, `strip`, `lstrip`, `rstrip`, `isalpha`, `isdigit`, `isalnum`, `isspace`, `isupper`, `islower` | §6.3 |
| `std.exception` | L3 | **yes** | `Exception`, `BoundsError`, `bounds_fail`, `NullReferenceError`, `null_fail`, `ArithmeticError`, `div_fail` | §6.1 |
| `std.interfaces` | L3 | **yes** | `IIterator<T>`, `IIterable<T>`, `IIndex<T>`, `IEqual`, `IOrder`, `IAdd` … | §6.2 |
| `std.task` | L3 | **yes** | `muc_para_*` (compiler entry points), `set_max_live_tasks`, `set_task_stack`, `set_carrier_limit`, `live_task_count`, `carrier_count`, `TaskLimitError` | §6.4 |
| `std.thread` | L3 | **yes** | `Thread`, `thread_start/join/detach`, `thread_sleep`, `monotonic_ms`, `Mutex`, `CondVar`, `WAIT_INFINITE` | §6.5 |

Planned (no code yet, listed so that naming and layering are decided before implementation):

| Unit | Layer | Purpose |
|---|---|---|
| `std.fmt` | L2 | type-safe formatting (`format("{}", x)`), replaces `printf` for new code |
| `std.io` | L2 | `Reader` / `Writer` interfaces, buffered streams, binary mode explicit |
| `std.collections` | L2 | `Array<T>` / `Array2d` / `Array3d` (wrappers over the built-in `T[]`), `Map<K,V>`, `StringBuilder` |
| `std.fs` | L2 | files, directories, paths (UTF-8 paths) |
| `std.parse` | L2 | `parse_int` / `parse_float` with explicit failure |
| `std.random` | L2 | explicit PRNG state, seedable (replaces `rand`) |
| `std.unicode` | L2 | UTF-8 decode/encode, codepoint iteration |
| `std.time` | L2 | `Instant` / `Duration` / wall clock |

## 4. Error model

| Layer | Failure is |
|---|---|
| L2 | an exception: a subclass of `std.exception.Exception`, named `*Error` (`BoundsError`, `TaskLimitError`, …) |
| L1 | C's own signal: `-1` / `null` / a partial count, plus `errno` |
| L3 | contract-specific: `TaskLimitError` from a refused task start; `BoundsError` from a managed-sequence subscript; `NullReferenceError` from a dereferenced null class/interface handle; `ArithmeticError` from integer division/remainder by zero |

- **Bounds checks belong to the language** (SPEC 3.3): a managed-sequence subscript that is out of
  range throws `BoundsError`; `--bounds=off` removes the check. The library adds no second policy
  and exposes no `at()`.
- **The null-reference check belongs to the language** (SPEC 3.8, 8.2): dereferencing a null class
  handle or interface value throws `NullReferenceError`; `--null-check=off` removes the check.
- **L2 never exposes `errno`.** A native wrapper calls the C function, inspects the result, and
  throws. `errno` stays an L1 detail.
- **`errno` does not survive a task switch.** `errno` is OS-thread-local and a `para` task may resume
  on another carrier; `std.task` does not save it (documented limitation). Consequence: L1 calls
  whose result depends on `errno` must be read on the calling thread without an intervening park, and
  L2 is the only safe place to turn them into exceptions.
- **Allocation failure aborts**, it is not an exception (SPEC 3.8): `new` and `alloc` mirror C's
  contract. The library does not invent a second allocator failure mode.

## 5. C interoperability policy

The ISO C surface is imported **relatively completely** into the separate `cstd` library: one unit per
header, one file per unit. A unit keeps the same name and the same file on every platform: the shared
declarations are written once, and a symbol whose name, value or layout differs is declared truthfully in
that platform's `#if` arm instead of being unified. Every `extern` declaration uses `std.ctypes` names,
and the raw layer keeps C semantics.

Constructs that are not importable at all stay out: macros (`assert`, `errno`, `limits`, ...) are
re-expressed as constants or functions; `stdarg` (no `va_list`), `setjmp` (conflicts with exceptions and
RC), and the wide-character API (UTF-16 vs UTF-32) are excluded by construction.

**Standard baseline: C99, plus a whitelist of C11/POSIX symbols that exist on every target.** C17 adds
no library functions; C23 is not a baseline. Consequences: `timespec_get` / `quick_exit` are not
portable (UCRT only on Windows), `aligned_alloc` and `threads.h` are absent on Windows, and `strdup` is
C23/POSIX (`_strdup` on Windows) — the platform files declare both spellings, so they are usable on the
platform that has them.

The per-header inventory, the tier labels, the CRT linking model (system CRT, dynamically linked), the
per-symbol minimum standard and the platform variance list (where `printf` / `fopen` / `rand` / `clock`
differ between Windows and POSIX) are implementation policy and live in the internal records
`agents/stdlib/clib-inventory.md` and `agents/stdlib/c-bindings.md`.

## 6. Contracts of the implicit units

### 6.1 `std.exception`

```mu
public class Exception {
public:
    string message;                  // Exception(), Exception(string msg)
}

public class BoundsError : Exception {
public:
    isize index;
    isize length;
    isize axis;                      // 0-based; multi-dimensional subscript
}

public class NullReferenceError : Exception {
public:
    // message names the failed access, e.g. "method call on a null `Dog` reference"
}

public class ArithmeticError : Exception {
public:
    long value;                      // the dividend
    int kind;                        // 0 = division/remainder by zero, 1 = MIN / -1
}
```

- `Exception` is the root of every throwable/catchable class (SPEC 3.14). It carries only `message`;
  user error classes add fields and constructors.
- `BoundsError` is thrown by the compiler-emitted bounds check; `bounds_fail(index, length, axis)` is
  the cold path the checker calls.
- `NullReferenceError` is thrown by the compiler-emitted null-reference check (SPEC 3.8, 8.2);
  `null_fail(what)` is the cold path. `--null-check=off` removes the check.
- `ArithmeticError` is thrown by the compiler-emitted division checks (SPEC 8.2): division/remainder by
  zero and `MIN / -1`; `div_fail(value, kind)` is the cold path. `--bounds=off` removes the checks.
- `TaskLimitError` is declared in `std.task`, not here: it belongs to the scheduler contract.

### 6.2 `std.interfaces`

The predeclared contracts are ordinary `interface` declarations in `std.interfaces`, satisfied
**structurally** by providing the members (the language rule — structural satisfaction, explicit
declaration checking the same shape, `class` equality staying identity — is SPEC 3.15.9).

| Interface | Members | Satisfied by |
|---|---|---|
| `IIterator<T>` | `bool next()`, `T current()` | a cursor for `for-in` (SPEC 6.4.2) |
| `IIterable<T>` | `IIterator<T> iterator()` | a container |
| `IIndex<T>` | `isize length()`, `T operator[](Self c, isize i)` | a readable container; satisfied by a member `T operator[](isize)` (subscripts are member-only since build 161), a `ref T` implementation (a borrow is usable as a value), or the built-in block subscript |
| `IIndexMut<T> : IIndex<T>` | also `ref T operator[](Self c, isize i)` | a writable container; satisfied only by a `ref` implementation (or the built-in block subscript). Required by a generic body that writes through an element, including `for ref` |
| `IEqual` | `==` (`!=` is derived, SPEC 3.11) | numbers, pointers, a value type with those operators |
| `IOrder : IEqual` | `<` (`>`, `<=`, `>=` are derived from it) | numbers, pointers, enums, a value type with those operators; promises a **total order** |
| `IAdd` / `ISub` / `IMul` / `IDiv` / `IMod` | the binary `+ - * / %` returning `Self` | numbers (`%` is `frem` for floats) |
| `INeg` | unary `-` | numbers |
| `INumber` | the arithmetic bundle (`IAdd` … `IOrder`, C# generic-math shape) | numbers; not enums |

- Each contract asks only for the operator the compiler can derive the rest from: `IEqual` needs `==`,
  `IOrder` needs `<`. A type that also declares `!=` / `>` / `<=` / `>=` still satisfies the contract,
  and its own operator wins over the derived one (that is how a type keeps the native semantics or
  avoids the extra call).
- There is deliberately **no bitwise contract**: the constraint checker recognises only
  `+ - * / %` and the comparison operators as built-in operators.
- User value types satisfy these contracts through `operator` members or free `operator` functions in
  a visible unit; `class` equality stays identity, so a `class` does not satisfy `IEqual`.

### 6.3 `std.string`

- **Language-visible contract** is SPEC 3.3: an immutable UTF-8 byte sequence, a value type,
  `s[i]` reads a `byte`, `==` compares bytes, a literal is a `const char*`, implicit construction
  from `const char*`, no unit-level `const` variable.
- **The model is bytes**: `length`, `s[i]`, `sub`, `find` and the search/split/replace family are
  byte-based. Code point operations are explicit and O(n) where indexing is involved.
- **Naming**: byte-oriented methods use Python names. Where the ASCII and Unicode meanings differ, the
  core method carries the `_ascii` suffix (`strip_ascii`, `lower_ascii`, `upper_ascii`,
  `is_alpha_ascii`, …). The unsuffixed Unicode names live in `std.unicode` and appear only where that
  unit is used.
- **Core surface**: `startswith`, `endswith`, `find`, `rfind`, `count`, `contains`, `replace`,
  `split(sep)`, `split_ascii_whitespace`, `splitlines_ascii`, `join`, `strip_ascii`/`lstrip_ascii`/
  `rstrip_ascii`, `lower_ascii`/`upper_ascii`, `is_alpha_ascii`/`is_digit_ascii`/`is_alnum_ascii`/
  `is_space_ascii`/`is_upper_ascii`/`is_lower_ascii`, `repeat`, `removeprefix`, `removesuffix`.
- **UTF-8 structure** (no Unicode tables): `is_valid_utf8`, `char_count`, `char_at`, `char_slice`,
  `is_char_boundary`, `byte_offset`, `char_index`, `codepoints`, `chars()`, `bytes()`. Invalid input
  is decoded lossily: each invalid byte becomes U+FFFD. `String(byte[])` copies with a NUL;
  `String(int[])` encodes UTF-8 and replaces invalid code points with U+FFFD.
- **`std.unicode`** adds `lower`, `upper`, `strip`, `lstrip`, `rstrip`, `isalpha`, `isdigit`,
  `isalnum`, `isspace`, `isupper`, `islower` through an interface extension on the `IString` hook.
  Properties and simple one-to-one case mappings come from Unicode 15.0.0 tables generated by
  `tools/gen-unicode-tables.py`. Normalization, grapheme/word/line segmentation and collation are
  **not** provided yet; multi-character case mappings (for example sharp s to SS) are not applied.
- **`cstr()` contract**: every representation of a value returns a NUL-terminated C string for that
  value — the zero value (`{}`) is the empty string, and `sub` copies into its own terminated buffer,
  so a substring never exposes the source bytes after its end.
- **`sub(start, count)`** clamps both bounds to the source length. It is a byte slice: the offsets are
  not required to be code point boundaries, so slicing inside a code point produces an invalid-UTF-8
  value (`char_slice` is the code point operation).
- **`find(needle)`** returns a **byte** offset (`isize`): `-1` means not found, and every representable
  offset stays positive on the supported 64-bit targets. `char_index` converts a boundary offset to a
  code point index.
- The compiler knows neither the layout nor the operations (SPEC 12 #4): `String` is an ordinary
  struct in `libs/std/string.mu`.

### 6.4 `std.task`

- Compiler entry points: `muc_para_start`, `muc_para_join`, `muc_para_group_new/add/join_free`.
- Settings and introspection: `set_max_live_tasks` (default 10000), `set_task_stack`
  (default 64 KiB commit / 256 KiB reserve), `set_carrier_limit`, `live_task_count`,
  `carrier_count`.
- `TaskLimitError` (a subclass of `Exception`) is thrown when a start is refused; the region joins
  what already started, then propagates.
- The language surface (`para`, `para for`, `thread`) and its rules are SPEC 3.17. The runtime model,
  guarantees and limitations are an internal record (`agents/stdlib/task.md`).
- **Planned, not implemented**: `Channel<T>` and `TaskGroup` (the deferred-join facility SPEC 3.17.1
  refers to as a future library addition) do not exist yet. Until they do, a deferred join has no
  library spelling.

### 6.5 `std.thread`

Low-level OS threads, mutexes and condition variables; the C-shaped layer the task runtime is built
on. It does not know about `para`.

**Exception boundary.** `thread_start`'s body is a thread boundary: an exception that escapes it
**aborts the process** (the same rule a `para` task body follows, SPEC 3.17.7). There is no way to hand
an exception to the joiner — the pending slot is thread-local — so catching it inside the body (or
reporting it some other way) is the caller's protocol. Signatures (bodies omitted):

```mu
public const uint WAIT_INFINITE = 0xFFFFFFFF;

public struct Thread { void* h; }          // opaque OS handle
public struct Mutex   { usize[8] s; }      // opaque storage: 64 bytes, sized for the largest backend
public struct CondVar { usize[8] s; }      // (SRWLOCK is 8, pthread_mutex_t 40, pthread_cond_t 48 on 64-bit Linux)

public Thread start((void*) => void body, void* arg);
public void   join(ref Thread t);
public void   detach(ref Thread t);
public void   sleep(uint ms);
public ulong  monotonic_ms();

public void mutex_lock(ref Mutex m);
public void mutex_unlock(ref Mutex m);

public bool cond_wait(ref CondVar c, ref Mutex m, uint ms);   // mutex held on entry, re-acquired on return
public void cond_signal(ref CondVar c);
public void cond_broadcast(ref CondVar c);
```

The declarations above show the intended names; the current source still uses `thread_start` /
`thread_join` / `thread_detach` / `thread_sleep` (U37, pending rename). `Mutex` / `CondVar` are opaque
byte blobs (0.0.128): each backend's lock object lives inside the 64-byte storage, and both the Win32
and the pthread backend accept zeroed memory as a valid unlocked lock, so a lock field still needs no
init/destroy call. The OS layer is `std.thread.backend`, resolved through the platform search path.

## 7. Platform units

- **Inside a unit**, a platform difference is an `#if` arm (SPEC 9.2): the unit keeps one file, the
  shared declarations stay outside the directive, and the arm holds only what differs. Every such unit
  opens with a guard that rejects an unsupported target with `#error` rather than silently compiling
  the wrong arm. Only the selected arm is name-resolved, type-checked and emitted; the other arms are
  parsed, so a dead arm cannot rot into a syntax error. The `cstd` library is written this way.
- **Across units**, the older form is the loader's shadow directory: a normal unit under
  `libs/std/platform/<os>/...` (`std.thread.backend`, `std.ctypes`), resolved from `--os` / the host.
  The public unit never mentions the platform (SPEC 2.1). Both mechanisms select per target; a unit
  tree should use one of them consistently.
- Platform **extensions** are separate units, not conditional declarations: `std.posix` (read,
  write, open, dirent, getline, `*_r` variants) and `std.win32` (handles, `CreateFile`,
  `WaitForSingleObject`). Both are invisible unless the target matches.
- `std.c*` is the ISO C surface only. A function that exists on one platform and not another goes to
  the platform unit, even when it looks like libc.

## 8. Roadmap

Order is driven by "what blocks ordinary programs", not by header completeness:

1. **`std.fmt` + `std.io`** — the only output mechanism today is `printf`; type-safe formatting and a
   `Reader`/`Writer` pair are the foundation for everything that follows (`fs`, `net`, tests).
2. **L1 completion** — `cstdlib` / `cstring` / `cmath` / `ctime` at tier A/B, with the platform
   variance table maintained in the internal C-binding record.
3. **`std.collections`** — `Array<T>` and friends (U38), `Map`, `StringBuilder`; these make example
   code natural and let `std.string` stop carrying container duties.
4. **`std.fs` / `std.time` / `std.process`** — after `std.io`, with UTF-8 paths as the contract.
5. **`std.posix` / `std.win32`** — only when the non-Windows backends land; they are the place where
   platform differences are allowed to be visible.

Each item follows the same acceptance shape as `std.task`: a contract section in this file, a decision
record under `agents/stdlib/`, and tests that exercise the contract rather than the implementation.
