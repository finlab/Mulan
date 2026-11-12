# Mulan regression tests

For each case `run_tests.py` runs:

```
<MUC_CMD> build <case.mu> -o build/tests/<name>.exe --run
```

then compares the program's stdout with `tests/expected/<name>.txt` and reports PASS/FAIL.
Only the Python standard library is used; run it from the **repository root**:

```powershell
# The compiler command comes from the MUC_CMD environment variable (default: muc)
$env:MUC_CMD = "build/muc.exe"
python tests/run_tests.py                 # all cases
python tests/run_tests.py --filter arith  # only cases whose name contains "arith"
python tests/run_tests.py -v              # print the full actual output on failure
python tests/run_tests.py --keep-c        # keep generated intermediates (deleted after the build by default)
```

Exit code is 0 when everything passes and 1 when any case fails (a missing compiler or a
failed build is reported as FAIL rather than crashing). Run it with UTF-8 I/O: the cases
themselves may print non-ASCII text (see `unicode_escape`), and CI sets `PYTHONUTF8=1` and
`PYTHONIOENCODING=utf-8` for that reason.

## How to add a case

1. Write `tests/cases/<name>.mu`:
   - start with `unit <name>;` (the name must be a legal identifier; keywords such as
     `struct` cannot be a unit name, which is why the `struct` case uses the unit name
     `struct_case`);
   - keep it self-contained: no `use` of other units; declare any output functions yourself
     with `extern int puts(readonly char* s);` / `extern int putchar(int c);`
     (`print_int` can be copied from an existing case);
   - `main` returns `0` on the normal path;
   - conditions must be bool (`if x` is illegal), there is no `++` and no C-style for loop,
     `!` is bitwise complement rather than logical not (logical not is `not`), and there is no
     `&&`/`||` (use `and`/`or`).
2. Write `tests/expected/<name>.txt`: the genuinely derived expected output (no placeholders).
3. Verify: `build\muc.exe check tests/cases/<name>.mu`, then run the runner.

The file name is the case name; nothing needs to be registered anywhere. `examples/example.mu`
is automatically run as a case by the runner (expected output `tests/expected/example.txt`).

## Comparison rules / known limitations

- Trailing whitespace per line, CRLF/LF differences and trailing blank lines are ignored.
- `build --run` mixes compiler status lines (`+ gcc ...`, `ok: <exe>`) into stdout; the runner
  strips lines starting with `+ `, `ok: ` and `error: ` —
  **a case must never print lines with these prefixes itself**.
- Expected outputs are derived under 32-bit `int`, two's complement and the documented shift
  rules (`>>` is arithmetic), and avoid any undefined behaviour (out of bounds, overflow,
  reading uninitialized memory).

## What the cases cover

| Case | Coverage |
|---|---|
| `arith` | the four arithmetic operators, `%`, shifts, precedence (`+` binds tighter than `<< >>`; `&`/`^`/`\|` are layered; bitwise binds tighter than comparison), while accumulation, compound assignment |
| `bool_ops` | `not a == b` ≡ `not (a==b)`; `not a and b` ≡ `(not a) and b` (De Morgan contrast); `and` binds tighter than `or`; `==` is left-associative; truth tables |
| `bitwise` | `& \| ^ !` (`!` is bitwise complement), `<< >>` (including negative values), compound assignment `&= \|= ^= <<= >>=`, setting/clearing/flipping bits |
| `control` | `if/elif/else`, `while` + `break/continue` (including `while true`), `for i in 0..n`, range endpoint expressions, nested loops |
| `struct` | value semantics (copies and parameter passing are independent), `{x = 1, y = 2}`, `{3, 4}`, zero initialization, `Point(...)`, `Point()` |
| `arrays` | `int[N]` aggregate initialization pads with zeros, value-semantics copy and parameter passing, `len`, `for v in arr`, `for ref v in arr` mutating elements |
| `forin_index` | index-style user containers (`length()` + `operator[](C, usize)` for struct/class), `for i, x` (real array index, relative offset for ranges), temporary container evaluated once, empty container |
| `strings` | `len`/`s[i]` (including literal indices and for-in iteration), `char[N] = "..."` copy + NUL, `puts(&buf[0])`, NUL truncation on write |
| `unicode_escape` | `\u{...}`: strings encoded as UTF-8 (1–4 bytes, byte values and `len`), equivalence with raw UTF-8 in source, char literals limited to a single-byte code point (including `U+FF` → `char` −1), upper/lowercase hex and leading zeros |
| `const_member` | type-level `const`: every scalar type, constants referring to each other / unary / comparison, `sizeof`/`alignof`, `Type.Name`, bare names inside methods, base-class constants and bare names in derived classes, private same-unit access, unit-level const referring to type-level constants, `sizeof` unchanged by constants, unsigned `>>` constants consistent with runtime, locals shadowing constants |
| `const_global_init` | constant initialization of non-`const` globals: `int g = 1 + 2;`, `int G = Cfg.K;`, enum/float/`long`, referencing unit-level const, with globals still mutable |
| `ptr` | `&x`/`*p` (including compound assignment), pointer parameters, `p[i]` and pointer arithmetic, `alloc/free`, `null` checks, `readonly int*`, `(*pp).field` |
