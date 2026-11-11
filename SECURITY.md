# Security Policy

## Supported versions

This project is pre-1.0 and moves fast. Only the latest commit on `main` is
supported; there are no maintained release branches yet.

## Reporting a vulnerability

**Please do not open a public issue for security problems.**

Use GitHub's private vulnerability reporting instead:

1. Open the [Security tab](https://github.com/finlab/Mulan/security) of this repository.
2. Click **Report a vulnerability**.
3. Describe the issue, ideally with a minimal `.mu` input that triggers it.

The report stays private between you and the maintainer until a fix is ready.

If the Security tab is unavailable to you, contact the maintainer through the
GitHub profile [@finlab](https://github.com/finlab) and ask for a private channel.

## What counts as a vulnerability

The compiler is the attack surface. Reports are in scope when they involve:

- **Miscompilation** — accepted source that produces code whose behaviour
  differs from `SPEC.md` (especially silently wrong values, missing bounds or
  lifetime checks, or `const`/visibility restrictions being bypassed).
- **Memory-safety holes** — accepted programs that read or write out of bounds,
  use freed memory, or double-free without a diagnostic, when `SPEC.md` says
  the construct must be rejected.
- **Generated-code injection** — source constructs that alter the emitted
  object file / linker invocation in ways the language does not allow.
- **Build or toolchain compromise** — issues in `CMakeLists.txt`, CI
  configuration, or the documented build path that let third parties influence
  what gets compiled or executed.

Out of scope:

- Programs that are explicitly undefined behaviour per `SPEC.md`.
- Crashes that require a hand-crafted malformed input to the compiler binary
  itself (robustness reports are welcome, but are treated as ordinary bugs).
- Issues in LLVM, MSYS2, or third-party libraries — please report those upstream.

## What to expect

This is a spare-time project, so response times are best effort. A useful
report usually gets an initial reply within a week. Once a fix lands, you are
welcome to be credited in the changelog entry.
