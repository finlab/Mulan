# Mulan regression tests

For each **run case** `run_tests.py` runs:

```
<MUC_CMD> build <case.mu> -o build/tests/<name>.exe --run
```

then compares the program's stdout and exit code with `tests/expected/<name>.txt` and reports
PASS/FAIL. The other case kinds (must-fail, check, ir, C ABI) have their own acceptance rules —
see the *Directory layout* section below.
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

Assert on what the *optimizer guarantees*, not on the shape of the lowering: symbol names, linkage,
parameter attributes (`byval` / `sret`), vtable and descriptor contents, and the absence of dead code.
Basic-block **labels** are not a stable target — `SimplifyCFG` merges an empty block away, the inliner
appends suffixes, and a branch the optimizer can fold disappears with its label, so a label assertion
passes or fails depending on the LLVM version (the `assign.same` / `assign.diff` self-assignment case was
such a case and is now covered by the run case `assign_same_rc`, which asserts the counting behaviour
itself). Matching a label name also tends to match the unit name inside mangled symbols by accident —
escape the dot (`assign\.diff`) if a symbol name must be matched.

## Directory layout

`run_tests.py` collects five kinds of cases from five directories; nothing is registered by hand, a
new file in the right directory (with its companion) is picked up automatically. The counts below are
the files currently in the tree (as of 0.0.122); they grow as cases are added.

| Directory | Kind | Cases (as of 0.0.122) | How a case passes |
|---|---|---|---|
| `tests/cases/` | **run** | 186, plus every `examples/*.mu` that defines `main` | built and run; stdout and exit code compared against `tests/expected/<name>.txt` |
| `tests/errors/` | **must-fail** | 356 | `muc check` must reject it, and every line of `tests/errors/<name>.err` must appear in the diagnostics |
| `tests/check/` | **check** | 20 | `muc check` must accept it (sema only; the program never runs) |
| `tests/ir/` | **ir** | 4 | built with `--emit-ir`; the `+ <regex>` / `- <regex>` lines of `tests/ir/<name>.txt` assert on the optimized IR (see the section above) |
| `tests/abi/` | **c abi** | 3 | built and run like a run case, but the C shim `tests/abi/<name>.c` is first compiled by the C compiler and `--link`ed into the executable (expected file: `tests/expected/<name>.txt`) |

Notes per kind:

- **Must-fail (`tests/errors/`)**: `tests/errors/<name>.mu` plus `tests/errors/<name>.err`; each
  non-empty, non-`#` line of the `.err` file is a substring that must appear in the diagnostics, so
  one case can pin several messages; an `@ <args>` line adds compiler options (for example
  `@ --single-thread`, for diagnostics that only appear under a flag). Names starting with `lib_` are dependency **libraries** of
  multi-unit cases, not cases; helper units with dotted unit names live in subdirectories
  (`tests/errors/vislib/secret.mu`), which keeps them out of the `*.mu` glob.
- **Check (`tests/check/`)**: only the semantic checker runs — visibility across units and
  libraries, `use` import forms, declaration shapes. `main` is written for the checker, not for
  execution; features that deserve a runtime assertion belong in `tests/cases/`.
- **Run (`tests/cases/`)**: besides plain output lines, an expected file may contain marker lines that
  are stripped before comparison:
  - `@exit <code>` — the run must end with this exit code (default `0`; a case whose documented
    behaviour is abnormal termination writes `@exit 3`, the status `muc`-generated code exits with);
  - `@stderr-contains <text>` — the run's stderr must contain this substring; stderr is not compared
    line by line, so a case that pins a runtime message (the uncaught-exception report, a bounds
    message) names only what it needs;
  - `@ <args>` — extra options appended to the `muc build` command (for example `@ --bounds=off`).
- **C ABI (`tests/abi/`)**: both sides are really different compilers' object files, which is what
  checks the C ABI of `extern` / `export` aggregates (byval / sret) — something no single-language
  test can observe.

## What the cases cover

The table below lists the **run cases** (`tests/cases/` plus the runnable examples); must-fail,
check, ir and C ABI cases are described in the *Directory layout* section.

`tests/bench/` holds small, hand-run benchmarks (not collected by the runner). Build and run them
directly, e.g. `build\muc.exe build tests/bench/rc_new.mu --run` versus the same with
`--single-thread`; each file's header explains what it measures.

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
| `exception_*` | unchecked `throw`/`try`/`catch`: class matching in written order, base-class catch, nested propagation, rethrow, RC cleanup on unwinding, constructor-throw cleanup (a nested struct field, a fixed-array prefix and a class's value members release only what finished, in reverse order), throw from methods/interface calls, loop-local cleanup |
| `finally_*` | `finally` on normal/catch/return/break/continue/exception paths, nested finally order, RC cleanup before/after finally, forbidden control flow in finally |
| `exception_maythrow*` | module-local may-throw analysis: nothrow free functions/recursion/struct methods and nothrow construction still work, may-throw calls still catch |
