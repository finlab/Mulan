# Standard library style

> Status: decided 2026-11-14. Applies to `libs/` and every public API; compiler-internal code
> follows it where practical. Identifier **syntax** is specified in `SPEC.md` §2.2 — this file fixes
> **naming**: which case each kind of name uses, and which name each concept gets.

## 1. Identifier styles

| Kind | Style | Examples |
|---|---|---|
| Built-in types (including `buffer<T>`) | lowercase | `int`, `byte`, `bool`, `usize`, `buffer<T>` |
| Library types (struct / class / enum / type alias), containers included | PascalCase | `Array<T>`, `Vec<T>`, `Thread`, `Mutex`, `Exception`, `Color` |
| Convenience alias (declared with `type`) | lowercase allowed | `string` = `String` |
| Interfaces | `I` + PascalCase | `IIterator<T>`, `IIndexable<T>` |
| Exception classes | `*Error` suffix; the base is `Exception` | `TaskLimitError` |
| Enum members | PascalCase | `Color.Red` |
| Methods and free functions | snake_case, verb-first | `length()`, `is_empty()`, `starts_with()` |
| Fields | snake_case nouns; public fields use full words | `data`, `capacity` |
| Parameters | snake_case, full words | `index`, `count`, `value`, `other` |
| Local variables | snake_case; loop indices `i` / `j` / `k` | `total`, `found` |
| Constants | UPPER_SNAKE | `MAX_CARRIERS`, `EOF` |
| Type parameters | single uppercase letter | `T`, `K`, `V` |
| Units | lowercase, dot-separated | `std.string`, `std.interfaces` |
| Files | lowercase snake_case, the path mirrors the unit | `libs/std/string.mu` |

The lowercase rule has a testable boundary: **is it built-in?**

- built-in → lowercase (`buffer<T>`, `int`) — the compiler provides it;
- library → PascalCase (`Array<T>`, `Vec<T>`, `Thread`) — even when it sits in the prelude and
  needs no `use`.

The canonical type is `String`; `string` is a declared alias (`type string = String;`) kept because it
is the type name every program types. Aliases may be lowercase; a real type name may not.

The two array-ish names do not overlap (decided 2026-11-14): **`buffer<T>` is the built-in
low-level safe block** (compiler-provided, lowercase), and **`T[]` / `T[,]` / `T[,,]` are syntax
sugar for `Array<T>` / `Array2d<T>` / `Array3d<T>`**, i.e. library containers, PascalCase, written
out in APIs where the sugar would be ambiguous. The sugar never denotes `buffer<T>`.

## 2. Methods

One concept, one name. Synonyms (`length` / `size` / `count`) are the main source of both review
churn and hallucinated APIs.

| Concept | Rule |
|---|---|
| Element count | `length()` only — the `for-in` index form requires `length() -> usize` (SPEC 6.4.1). `size()` / `count()` never mean the element count. The built-in `len` is removed; every **managed** sequence answers `length()`. Static arrays are C storage: no `length()`, no `for-in` — derive the count with `sizeof(a) / sizeof(a[0])`. |
| Emptiness | `is_empty()`; a bare `empty()` is not part of the API. |
| Boolean query | `is_*` / `has_*` / `can_*`. |
| Mutation | verb-first: `push`, `pop`, `insert`, `remove`, `clear`, `reserve`, `resize`. |
| Deep copy | `clone()` returns a new container. |
| In-place fill | `copy_from(src)`; must not allocate when the capacity suffices. |
| Conversion | `to_*` / `from_*` (`to_string()`, `from_utf8()`). |
| Search | `find(x) -> isize`, `-1` when absent; `contains(x)` is the boolean convenience. |
| Subscript | `operator[]` is the only element access. **No `at()`**: out-of-range follows the language bounds policy (debug check), so there is exactly one out-of-range behaviour. |
| Forbidden | `get_*` / `set_*` prefixes — `length()`, not `get_length()`. |

## 3. Parameters

- Full words: `index` not `idx`, `count` not `cnt`, `value` not `val`. Locals may be short; public
  parameter names may not.
- `other` is the second operand of a binary operation: `concat(string other)`.
- `dst` / `src` are the **only** permitted abbreviations, for copy-like operations, destination
  first: `copy_into(dst, src)` (matches `memcpy`).
- Boolean parameters carry the same prefix as boolean queries: `sort(bool is_stable)`.
- Keep three words apart: `count` = number of items, `length` = sequence length, `size` = bytes
  (only in C-facing APIs).
- `ref` parameters need no `out_` prefix — the signature already says it; a written target goes
  first.
- A parameter must not shadow a field or a method.

## 4. Fields

- snake_case nouns. Public fields use full words (`data`, `capacity`); private fields may be short
  (`p`, `n`) — `private:` already marks them, so **no `_` / `m_` prefixes**.
- Boolean fields use `is_*` / `has_*`.
- A field must not share a name with a method (`length` field plus `length()` method is an error,
  SPEC 3.11); containers therefore keep `n` / `cap` as private fields.

## 5. Constants and enums

- Constants are UPPER_SNAKE.
- A related group of constants is an `enum`, not a set of `const int`s — an enum gets the exhaustive
  `switch` check (SPEC 6.6).
- Type-level constants are PascalCase because they live in the type's namespace: `Color.Red`.

## 6. Units and files

- Unit names are lowercase and dot-separated; the file path mirrors the unit
  (`std.string` → `libs/std/string.mu`).
- One public unit per file. Platform-specific implementations live under
  `libs/std/platform/<os>/...` (SPEC 2.1).
- A free function does **not** repeat its module name: inside `std.thread`, write `start` / `join`,
  not `thread_start` / `thread_join`.

## 7. C interop

- `extern` declarations keep the C name verbatim (`fopen`, `fread`, `memcpy`); do not "mulanise"
  them.
- C ABI types are lowercase `c_*` (`c_int`, `c_long`, `c_size`) from `std.ctypes` (SPEC 7.2).
- Mulan wrappers around C functions follow the Mulan rules above.

## 8. Abbreviations

- In type names an acronym is a word: `HttpClient`, `JsonParser` — not `HTTPClient`,
  `JSONParser`.
- Constants may be fully uppercase: `MAX_HTTP_CONNECTIONS`.
- Public API names are not abbreviated. `dst` / `src` (parameters) and `cstr` (C interop) are the
  only exceptions.

## 9. Review checklist

1. One concept, one name — no `length` / `size` / `count` synonyms.
2. Public names are not abbreviated.
3. Booleans carry `is_` / `has_` / `can_`.
4. The unit does not repeat its own name in its functions.
5. A type is lowercase only if it is built-in.

Existing deviations from this document are tracked in `agents/design-backlog.md` (U37).
