"""Mulan regression test runner.

For every case it runs
    <MUC_CMD> build <case> -o build/tests/<name>.exe --run
and reports PASS/FAIL after comparing stdout with tests/expected/<name>.txt.

The compiler command is configured through the MUC_CMD environment variable
(default `muc`), for example:
    $env:MUC_CMD = "build/muc.exe"; python tests/run_tests.py   (PowerShell)

Usage:
    python tests/run_tests.py                 # all cases (including examples/example.mu)
    python tests/run_tests.py --filter arith  # only cases whose name contains "arith"
    python tests/run_tests.py -v              # print the full actual output on failure
    python tests/run_tests.py --keep-c        # keep the generated .ll/.o intermediates

Known limitations:
  * `muc build --run` mixes compiler status lines (`+ gcc ...`, `ok: <exe>`) into the
    program's stdout; the runner strips lines starting with `+ ` and `ok: ` before
    comparing, so cases must not print lines with these prefixes themselves.
  * Standard library only; run it from the repository root as
    `python tests/run_tests.py`.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CASES_DIR = ROOT / "tests" / "cases"
EXPECTED_DIR = ROOT / "tests" / "expected"
ERRORS_DIR = ROOT / "tests" / "errors"
CHECKS_DIR = ROOT / "tests" / "check"
BUILD_DIR = ROOT / "build" / "tests"

# Compiler command: <MUC_CMD> build <case.mu> -o <exe> --run
# Defaults to `muc` (the compiler binary that was built).
DEFAULT_COMPILER = "muc"

# Status-line prefixes the compiler writes to stdout (stripped before comparison)
COMPILER_PREFIXES = ("+ ", "ok: ", "error: ")
RUN_TIMEOUT = 120  # seconds


def resolve_compiler() -> list[str]:
    """Split MUC_CMD into a list of command tokens.

    Three forms are supported:
      * no spaces (such as `muc`, `build\\muc.exe`) → a single token;
      * an existing single file path on this machine (spaces allowed) → a single token;
      * contains spaces but is not a file path (such as a wrapper script with
        arguments) → split with shlex.
    """
    raw = (os.environ.get("MUC_CMD") or DEFAULT_COMPILER).strip()
    if " " not in raw:
        return [raw]
    as_path = Path(raw)
    if as_path.is_file():
        return [str(as_path)]
    return shlex.split(raw)


class Case:
    def __init__(self, name: str, src: Path, expected: Path) -> None:
        self.name = name
        self.src = src
        self.expected = expected


def _strip_comments_and_literals(text: str) -> str:
    """Blank out comments, string literals and character literals.

    Example discovery is a text scan (the runner must not depend on a parser), so the scan runs on
    code only: a comment or a string that mentions `int main` must not make a library unit look
    like an entry point, and comments between `int` and `main` must not hide a real one.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(i + 2, n)
        elif c in "\"'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            out.append(" ")  # keep the tokens on both sides apart
        else:
            out.append(c)
            i += 1
    return "".join(out)


_ENTRY_RE = re.compile(r"\bint\s+main\s*\(")


def defines_main(src: Path) -> bool:
    """Whether the unit defines `main` (formatting- and comment-tolerant text scan)."""
    return _ENTRY_RE.search(_strip_comments_and_literals(src.read_text(encoding="utf-8"))) is not None


def collect_cases() -> list[Case]:
    cases: list[Case] = []
    for src in sorted(CASES_DIR.glob("*.mu")):
        cases.append(Case(src.stem, src, EXPECTED_DIR / f"{src.stem}.txt"))
    # examples/*.mu that define `main` also run as cases; the rest are library units
    # for the multi-unit example (such as examples/math.mu).
    for src in sorted(ROOT.glob("examples/*.mu")):
        if not defines_main(src):
            continue
        cases.append(Case(src.stem, src, EXPECTED_DIR / f"{src.stem}.txt"))
    return cases


class ErrCase:
    """A case the compiler must reject: each .err line is a substring that must appear in the diagnostics."""

    def __init__(self, name: str, src: Path, spec: Path) -> None:
        self.name = name
        self.src = src
        self.spec = spec


def collect_error_cases() -> list[ErrCase]:
    # Files starting with lib_ are dependency libraries for multi-unit error cases, not cases themselves
    return [ErrCase(s.stem, s, ERRORS_DIR / f"{s.stem}.err")
            for s in sorted(ERRORS_DIR.glob("*.mu"))
            if not s.stem.startswith("lib_")]


class CheckCase:
    """A case that only has to pass the semantic check (`check`).

    Used for features the semantic layer fully supports but codegen does not cover
    yet (such as class / interface).
    """

    def __init__(self, name: str, src: Path) -> None:
        self.name = name
        self.src = src


def collect_check_cases() -> list[CheckCase]:
    if not CHECKS_DIR.is_dir():
        return []
    # Files starting with lib_ are dependency libraries for multi-unit tests, not cases themselves
    return [CheckCase(s.stem, s) for s in sorted(CHECKS_DIR.glob("*.mu"))
            if not s.stem.startswith("lib_")]


def run_check_case(case: CheckCase, verbose: bool) -> tuple[bool, str]:
    cmd = resolve_compiler() + ["check", str(case.src.relative_to(ROOT))]
    try:
        r = subprocess.run(
            cmd, cwd=ROOT, capture_output=True, text=True,
            errors="replace", timeout=RUN_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return False, f"timeout (>{RUN_TIMEOUT}s): " + " ".join(cmd)
    except OSError as e:
        return False, f"cannot execute compiler `{cmd[0]}` ({e})"

    if r.returncode == 0:
        return True, "ok"
    lines = ["semantic check failed (it should pass)", "  command: " + " ".join(cmd)]
    text = ((r.stderr or "") + "\n" + (r.stdout or "")).strip()
    lines += ["    " + ln for ln in text.splitlines()[:10]]
    if verbose:
        lines.append("  --- full stderr ---")
        lines += ["  " + ln for ln in (r.stderr or "").splitlines()]
    return False, "\n".join(lines)


def normalize(text: str) -> list[str]:
    """Strip compiler status lines, trailing whitespace and trailing blank lines."""
    out: list[str] = []
    for line in text.splitlines():
        if line.startswith(COMPILER_PREFIXES):
            continue
        out.append(line.rstrip())
    while out and out[-1] == "":
        out.pop()
    return out


def run_case(case: Case, keep_c: bool, verbose: bool) -> tuple[bool, str]:
    """Return (passed, explanation)."""
    exe = BUILD_DIR / (case.name + (".exe" if sys.platform == "win32" else ""))
    cmd = resolve_compiler() + [
        "build",
        str(case.src.relative_to(ROOT)),
        "-o", str(exe.relative_to(ROOT)),
        "--run",
    ]
    try:
        r = subprocess.run(
            cmd, cwd=ROOT, capture_output=True, text=True,
            errors="replace", timeout=RUN_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return False, f"timeout (>{RUN_TIMEOUT}s): " + " ".join(cmd)
    except OSError as e:
        return False, f"cannot execute compiler `{cmd[0]}` ({e}); set MUC_CMD, e.g. `build/muc.exe`"

    if not case.expected.is_file():
        return False, f"missing expected file {case.expected}"

    if r.returncode != 0:
        msg = [
            f"build --run failed (exit code {r.returncode})",
            "  command: " + " ".join(cmd),
            "  stderr tail:",
        ]
        msg += ["    " + ln for ln in (r.stderr.strip().splitlines() or ["<empty>"])[-8:]]
        return False, "\n".join(msg)

    actual = normalize(r.stdout)
    with open(case.expected, "r", encoding="utf-8", newline="") as f:
        wanted = normalize(f.read())

    if actual == wanted:
        return True, "ok"

    # Locate the first difference and print some context
    i = 0
    while i < len(actual) and i < len(wanted) and actual[i] == wanted[i]:
        i += 1
    lines = [
        f"output mismatch: first difference at line {i + 1} (actual {len(actual)} lines / expected {len(wanted)} lines)",
        "  context [expected | actual]:",
    ]
    lo, hi = max(0, i - 2), max(i + 3, min(len(actual), len(wanted)))
    for j in range(lo, hi):
        got = actual[j] if j < len(actual) else "<missing>"
        exp = wanted[j] if j < len(wanted) else "<missing>"
        mark = " " if j < len(actual) and j < len(wanted) and actual[j] == wanted[j] else ">"
        lines.append(f"  {mark} {j + 1:>3} | exp: {exp!r} | act: {got!r}")
    if verbose:
        lines.append("  --- full actual output ---")
        lines += ["  " + ln for ln in r.stdout.splitlines()]
        if not lines[-1]:
            lines.pop()
    return False, "\n".join(lines)


def run_error_case(case: ErrCase, verbose: bool) -> tuple[bool, str]:
    """The case must be rejected; every .err line substring must appear in the diagnostics."""
    if not case.spec.is_file():
        return False, f"missing expectation file {case.spec} (one required error substring per line)"
    wanted = [
        ln.strip()
        for ln in case.spec.read_text(encoding="utf-8", errors="replace").splitlines()
        if ln.strip() and not ln.strip().startswith("#")
    ]
    cmd = resolve_compiler() + ["check", str(case.src.relative_to(ROOT))]
    try:
        r = subprocess.run(
            cmd, cwd=ROOT, capture_output=True, text=True,
            errors="replace", timeout=RUN_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return False, f"timeout (>{RUN_TIMEOUT}s): " + " ".join(cmd)
    except OSError as e:
        return False, f"cannot execute compiler `{cmd[0]}` ({e})"

    text = (r.stderr or "") + "\n" + (r.stdout or "")
    if r.returncode == 0:
        return False, "the compiler accepted this program, which must be rejected (missed diagnostic)\n  command: " + " ".join(cmd)
    missing = [w for w in wanted if w not in text]
    if missing:
        lines = ["diagnostics are missing expected keywords: " + ", ".join(repr(m) for m in missing), "  actual output:"]
        lines += ["    " + ln for ln in text.strip().splitlines()[:10]]
        if verbose:
            lines.append("  --- full stderr ---")
            lines += ["  " + ln for ln in (r.stderr or "").splitlines()]
        return False, "\n".join(lines)
    return True, "ok"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Mulan regression test runner")
    ap.add_argument("--filter", help="only run cases whose name contains this substring")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the full actual output on failure")
    ap.add_argument("--keep-c", action="store_true", help="keep the generated .ll/.o intermediates and the exe")
    args = ap.parse_args(argv)

    cases = collect_cases()
    err_cases = collect_error_cases()
    check_cases = collect_check_cases()
    if args.filter:
        cases = [c for c in cases if args.filter in c.name]
        err_cases = [c for c in err_cases if args.filter in c.name]
        check_cases = [c for c in check_cases if args.filter in c.name]
    if not cases and not err_cases and not check_cases:
        print(f"no cases match `{args.filter}`")
        return 1
    total = len(cases) + len(err_cases) + len(check_cases)

    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    passed: list[str] = []
    failed: list[str] = []
    # Only the files this invocation creates may be deleted afterwards (never a glob over `build/`):
    # the compiler writes `build/<entry>.o` and the runner links `build/tests/<name>.exe`.
    artifacts: list[Path] = []
    for case in cases:
        artifacts.append(ROOT / "build" / (case.src.stem + ".o"))
        artifacts.append(BUILD_DIR / (case.name + (".exe" if sys.platform == "win32" else "")))
        ok, msg = run_case(case, args.keep_c, args.verbose)
        if ok:
            passed.append(case.name)
            print(f"PASS {case.name}")
        else:
            failed.append(case.name)
            print(f"FAIL {case.name}")
            print(msg)

    for case in err_cases:
        ok, msg = run_error_case(case, args.verbose)
        if ok:
            passed.append(case.name)
            print(f"PASS {case.name} (must-fail)")
        else:
            failed.append(case.name)
            print(f"FAIL {case.name} (must-fail)")
            print(msg)

    for case in check_cases:
        ok, msg = run_check_case(case, args.verbose)
        if ok:
            passed.append(case.name)
            print(f"PASS {case.name} (check)")
        else:
            failed.append(case.name)
            print(f"FAIL {case.name} (check)")
            print(msg)

    print()
    print(f"passed {len(passed)} / {total}")
    print(f"  ({len(cases)} run cases, {len(err_cases)} must-fail cases, {len(check_cases)} check cases)")
    if failed:
        print("failed cases: " + ", ".join(failed))
        code = 1
    else:
        print("all passed")
        code = 0

    if not args.keep_c:
        for p in artifacts:
            try:
                p.unlink()
            except (FileNotFoundError, PermissionError):
                pass

    return code


if __name__ == "__main__":
    raise SystemExit(main())
