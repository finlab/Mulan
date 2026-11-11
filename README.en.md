# Mulan

[![CI](https://github.com/finlab/Mulan/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/finlab/Mulan/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

[简体中文](README.md) | **English**

Mulan's design goals:

1. Start from C syntax and restrict, change, and extend it, so that it is safer, more explicit, and easier to use.
2. A compiled language with performance close to C/C++, keeping C's low-level expressiveness.
3. Two object mechanisms — value and reference — providing RAII- and RC-based automatic memory management.
4. Exception handling, generics, concurrency, and other modern language features.

Current version **0.0.57**, not yet 1.0. The language specification is [`SPEC.md`](SPEC.md)
(draft v0.2, with each rule marked as settled / suggested / open). It is written in Chinese.

## Design trade-offs

The design is less about what was added than about why each rule exists:

| Decision | What it means | Why |
|---|---|---|
| **Values and references are separate** | `struct` is a value type, `class` is a reference type, with a clear line between them | `=` tells you whether it copies or shares |
| **Sharing is explicit** | Iteration binds by value; write `for ref v in a` to modify elements | No hidden aliases, no accidental in-place mutation |
| **Reclamation is deterministic** | `class` uses atomic reference counting; the destructor runs as soon as the last reference goes away | No GC pauses, no "clean it up later" |
| **Cycles are broken with `weak`** | `weak T` is a non-owning reference; cycles must be broken by hand | No cycle collector, predictable behaviour |
| **No preprocessor** | `unit` / `use` modules plus `const` constants | Errors point at real source lines, not at expanded macros |
| **No implicit conversions** | Only lossless widening; everything else needs `as` | Truncation and reinterpretation are always written by you |
| **Conditions must be `bool`** | `if n` is a compile error; write `n != 0` | Removes the classic `=` / `==` class of bugs |
| **Only two loops** | `while` plus `for-in` (ranges / arrays / slices / `string`) | No trade-off between `for(;;)` and `do-while` edge cases |
| **C ABI is a first-class citizen** | `extern` declares libc directly; `struct` layout is controllable (`packed` / `align(N)`) | Coexist with C rather than replace it |

## A program that runs

Value types, reference types, interface polymorphism, and deterministic destruction in about 30 lines:

```c
unit demo;

extern int puts(const char* s);
extern int printf(const char* fmt, ...);

int live = 0;

interface Shape {
    double area();
}

struct Rect {                 // value type: stack allocated, assignment copies, no refcount
    double w, h;
    double area() { return w * h; }
}

class Circle : Shape {        // reference type: atomic refcount, deterministic destruction
    double r;
    Circle(double rr) { r = rr; live += 1; }
    ~Circle() { live -= 1; }
    override double area() { return 3.14159 * r * r; }
}

int main() {
    Rect box = {2.0, 3.0};

    Shape[2] shapes = { new Circle(1.0), new Circle(2.0) };

    double total = box.area();
    for s in shapes {
        total += s.area();
    }

    shapes[0] = null;         // last reference gone: both Circles are destroyed immediately
    shapes[1] = null;
    if live == 0 and total > 21.70 and total < 21.72 {
        puts("21.71, no survivors");
    }
    return 0;
}
```

```console
$ muc build demo.mu --run
21.71, no survivors
```

What is *not* in this code: a preprocessor, a GC, implicit conversions, the `virtual` keyword,
or the `->` operator (member access is always `.`).

More examples: [`example.mu`](example.mu) (a tour of the syntax) and [`math.mu`](math.mu) (cross-unit calls).

## Language status

**Implemented**

| Area | Details |
|---|---|
| Types | Integers / floating point / `bool` / `byte` / `char`; pointers; fixed-size arrays; dynamic arrays `T[]`; slices; `string`; `struct`; `union`; `enum` |
| Object model | `class` with single inheritance and `interface`; virtual by default; atomic RC; `weak`; constructors and destructors `~T()`; operator overloading |
| Control flow | `if` / `while` / `for-in` / `switch` (exhaustive over enums) / `break` / `continue` / `?:` |
| Functions | Recursion, overloading (bounded rules), variadic `extern` calls (0.0.57) |
| Modules | `unit` / `use` / `public` / `private`; units are loaded from same-named files |
| C interop | `extern` declarations, C ABI, controllable `struct` layout (`packed` / `align(N)`) |
| Toolchain | `check` / `dump` / `build`, `--emit-ir`, DWARF debug info via `-g`, multiple errors reported at once |

**Not implemented yet**

- Exceptions `try` / `catch` / `throw` (designed, not built)
- Generics (monomorphization planned; only the keyword and mangling are reserved)
- Closures, function pointers, `goto`
- A standard library (`libs/std/` currently holds only minimal `cstdio` and `string` bindings)
- Automatic nulling of outstanding `weak` references (you must check liveness yourself)
- Non-Windows targets: the backend is fixed to `x86_64-w64-windows-gnu` and emits COFF

## Quick start

Requirements: **Windows + MSYS2**, CMake ≥ 3.20, Ninja, g++ ≥ 11 (C++20), gcc, and LLVM ≥ 17
development libraries.

```powershell
# 1. LLVM (MSYS2)
C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm mingw-w64-x86_64-llvm"

# 2. Build the compiler (CMake finds LLVM via LLVM_DIR, or probes the MSYS2 path)
python -m cmake -S . -B build -G Ninja
python -m cmake --build build          # output: build/muc.exe

# 3. Try it
build\muc.exe check example.mu         # semantic checks only
build\muc.exe dump  math.mu            # print the AST
build\muc.exe build example.mu --run   # compile, link, run
build\muc.exe build example.mu -g      # with DWARF, so gdb can break and step
```

If a dependency is missing: `python -m pip install cmake ninja`. For LLVM installed elsewhere, pass
`-DLLVM_DIR=<path>/lib/cmake/llvm`.

## Diagnostics

Multiple errors are reported in one pass, with a source snippet and a caret:

```console
multi_errors.mu:4:19: error: undefined identifier `undefined_one`
  |
4 | int f1() { return undefined_one; }
  |                   ^
multi_errors.mu:5:19: error: undefined identifier `undefined_two`
  |
5 | int f2() { return undefined_two; }
  |                   ^
```

## Compiler

A readable C++20 implementation in a handful of files. The only backend uses the LLVM C++ API
(IR constructed directly, then a COFF object file, linked by gcc). It depends on nothing but LLVM.

```
src/
  lexer.cpp         lexing: keywords, literals, nested block comments
  parser.cpp        recursive-descent parsing -> AST
  sema.cpp          name resolution / type checking / annotations
  codegen_llvm.cpp  LLVM backend
  loader.cpp        unit loading (use -> same-named .mu)
docs/
  llvm-backend.md   backend design and limitations
  design-backlog.md open design questions (discussion log)
tests/
  cases/            success cases: compile, run, compare expected output
  errors/           cases that must be rejected; *.err lists required error substrings
  check/            cases that only need to pass semantic checks
  run_tests.py      regression runner (compiler command configurable via MUC_CMD)
```

Quality bar: **289 test cases green** (70 success + 206 must-fail + 13 check), a clean build with
`-Wall -Wextra` and zero warnings, and CI rerunning everything on a fresh MSYS2 environment for
every push.

## Documentation

| File | Contents |
|---|---|
| [`SPEC.md`](SPEC.md) | Language specification, each rule marked settled / suggested / open (Chinese) |
| [`docs/llvm-backend.md`](docs/llvm-backend.md) | LLVM backend design and limitations (Chinese) |
| [`docs/object-model-plan.md`](docs/object-model-plan.md) | Object model implementation plan (Chinese) |
| [`docs/design-review.md`](docs/design-review.md) | Language boundary review (Chinese) |
| [`docs/versions.md`](docs/versions.md) | Per-version change log (Chinese) |

## License

**Apache License 2.0** — see [`LICENSE`](LICENSE) for the full text and [`NOTICE`](NOTICE) for
third-party components.

- Programs written in Mulan, and the object files or executables produced by `muc`, are **not**
  derivative works of the compiler and may be distributed under terms of your choosing.
- Distributing with the LLVM backend also requires compliance with `Apache-2.0 WITH LLVM-exception`.
- Every source file starts with `SPDX-License-Identifier: Apache-2.0`.

<sub>The name "Mulan / 木兰" is unrelated to other open-source projects using the same name.</sub>
