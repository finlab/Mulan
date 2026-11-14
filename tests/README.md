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
     with `extern int puts(const char* s);` / `extern int putchar(int c);`
     (`print_int` can be copied from an existing case);
   - `main` returns `0` on the normal path;
   - conditions must be bool (`if x` is illegal), there is no `++` expression and no C-style for
     loop, `~` is bitwise complement and `not` is logical negation (`!` is not an operator), and
     there is no `&&`/`||` (use `and`/`or`).
2. Write `tests/expected/<name>.txt`: the genuinely derived expected output (no placeholders).
3. Verify: `build\muc.exe check tests/cases/<name>.mu`, then run the runner.

The file name is the case name; nothing needs to be registered anywhere. Every `examples/*.mu`
that defines `main` is also run as a case by the runner, with expected output
`tests/expected/<name>.txt` (`examples/example.mu` → `example.txt`,
`examples/generics.mu` → `generics.txt`); units without `main` (`examples/math.mu`) are skipped
because they are libraries for the multi-unit example.

## Comparison rules / known limitations

- Trailing whitespace per line, CRLF/LF differences and trailing blank lines are ignored.
- `build --run` mixes compiler status lines (`+ gcc ...`, `ok: <exe>`) into stdout; the runner
  strips lines starting with `+ `, `ok: ` and `error: ` —
  **a case must never print lines with these prefixes itself**.
- Expected outputs are derived under 32-bit `int`, two's complement and the documented shift
  rules (`>>` is arithmetic), and avoid any undefined behaviour (out of bounds, overflow,
  reading uninitialized memory).

## IR assertion cases (`tests/ir/`)

Some policies are only visible in the generated code — the executable target internalizes every
definition except `main`, so unused functions, classes and library units disappear. Cases in
`tests/ir/` compile with `--emit-ir` and assert on the optimized IR text:

- `tests/ir/<name>.mu` — the program (no `main` check: it must build);
- `tests/ir/<name>.txt` — one assertion per line: `+ <regex>` must appear in the IR,
  `- <regex>` must not; `@ <args>` adds compiler options (e.g. `@ --exports=public`); blank lines
  and `#` comments are ignored.

```text
+ ^define .*@main\(
- mu_dead_code_unused_public
```

The regexes are matched against the whole IR with `re.MULTILINE`, so anchor with `^`/`$` when the
position matters. Files starting with `lib_` are dependency units, not cases.

## What the cases cover

| Case | Coverage |
|---|---|
| `arith` | the four arithmetic operators, `%`, shifts, precedence (`+` binds tighter than `<< >>`; `&`/`^`/`\|` are layered; bitwise binds tighter than comparison), while accumulation, compound assignment |
| `bool_ops` | `not a == b` ≡ `not (a==b)`; `not a and b` ≡ `(not a) and b` (De Morgan contrast); `and` binds tighter than `or`; `==` is left-associative; truth tables |
| `bitwise` | `& \| ^ ~` (`~` is bitwise complement), `<< >>` (including negative values), compound assignment `&= \|= ^= <<= >>=`, setting/clearing/flipping bits |
| `control` | `if/elif/else`, `while` + `break/continue` (including `while true`), `for i in 0..n`, range endpoint expressions, nested loops |
| `struct` | value semantics (copies and parameter passing are independent), `{x = 1, y = 2}`, `{3, 4}`, zero initialization, `Point(...)`, `Point()` |
| `arrays` | `int[N]` aggregate initialization pads with zeros, value-semantics copy and parameter passing, **static arrays are C storage** (count with `sizeof(a)/sizeof(a[0])`, iterate with an index loop; no `len`, no `for-in`) |
| `forin_index` | index-style user containers (`length()` + `operator[](C, usize)` for struct/class), `for i, x` (real array index, relative offset for ranges), temporary container evaluated once, empty container |
| `strings` | `s.length()` / `s[i]` (including literal indices and for-in iteration), `char[N] = "..."` copy + NUL, `puts(&buf[0])`, NUL truncation on write |
| `unicode_escape` | `\u{...}`: strings encoded as UTF-8 (1–4 bytes, byte values and `s.length()`), equivalence with raw UTF-8 in source, char literals limited to a single-byte code point (including `U+FF` → `char` −1), upper/lowercase hex and leading zeros |
| `const_member` | type-level `const`: every scalar type, constants referring to each other / unary / comparison, `sizeof`/`alignof`, `Type.Name`, bare names inside methods, base-class constants and bare names in derived classes, private same-unit access, unit-level const referring to type-level constants, `sizeof` unchanged by constants, unsigned `>>` constants consistent with runtime, locals shadowing constants |
| `const_global_init` | constant initialization of non-`const` globals: `int g = 1 + 2;`, `int G = Cfg.K;`, enum/float/`long`, referencing unit-level const, with globals still mutable |
| `ptr` | `&x`/`*p` (including compound assignment), pointer parameters, `p[i]` and pointer arithmetic, `alloc/free`, `null` checks, `const int*`, `(*pp).field` |
| `exception_*` | unchecked `throw`/`try`/`catch`: class matching in written order, base-class catch, nested propagation, rethrow, RC cleanup on unwinding, constructor-throw cleanup, throw from methods/interface calls, loop-local cleanup |
| `finally_*` | `finally` on normal/catch/return/break/continue/exception paths, nested finally order, RC cleanup before/after finally, forbidden control flow in finally |
| `exception_maythrow*` | module-local may-throw analysis: nothrow free functions/recursion/struct methods and nothrow construction still work, may-throw calls still catch |
