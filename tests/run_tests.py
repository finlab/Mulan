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

Expected files (`tests/expected/<name>.txt`) hold the program's stdout, plus up to two kinds of
marker lines that are stripped before comparison:
    @exit <code>   the run must end with this exit code (default 0; abnormal termination is 3)
    @ <args>       extra options appended to the `muc build` command (for example `--bounds=off`)
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
WARN_DIR = ROOT / "tests" / "warnings"
IR_DIR = ROOT / "tests" / "ir"
ABI_DIR = ROOT / "tests" / "abi"
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


def parse_expected(path: Path) -> tuple[list[str], int, list[str]]:
    """Split an expected file into (output lines, expected exit code, extra compiler args).

    Three marker lines are recognised and removed from the output comparison:
      * `@exit <code>` — the run must end with that process exit code (default 0; a case that
        documents abnormal termination expects 3);
      * `@platform <os>[,<os>]` — the case only runs on those hosts (see `platform_skips`);
      * `@stderr-contains <text>` — the run's stderr must contain this substring (see
        `stderr_needles`; stdout is what the body of the file compares);
      * `@ <args>` — extra options appended to the `muc build` command (for example `@ --bounds=off`).
    """
    exit_code = 0
    extra: list[str] = []
    out: list[str] = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        for raw in f.read().splitlines():
            line = raw.rstrip()
            if line.startswith("@exit"):
                exit_code = int(line[len("@exit"):].strip())
            elif line.startswith("@platform"):
                continue
            elif line.startswith("@stderr-contains"):
                continue  # asserted separately, by stderr_needles
            elif line.startswith("@"):
                extra += shlex.split(line[1:].strip())
            else:
                out.append(line)
    while out and out[-1] == "":
        out.pop()
    return out, exit_code, extra


def host_os() -> str:
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def platform_skips(spec: Path | None) -> bool:
    """Whether a case is fenced off from this host by an `@platform <os>[,<os>]` marker line.

    The marker sits in the case's expectations file (or IR spec). It exists for cases whose
    subject is a platform fact — MSVC-only symbols, the MS x64 aggregate ABI — so the case is not
    silently weakened to the lowest common denominator.
    """
    if spec is None or not spec.is_file():
        return False
    wanted: list[str] | None = None
    for raw in spec.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("@platform"):
            wanted = [p.strip() for p in line[len("@platform"):].split(",") if p.strip()]
            break
    return bool(wanted) and host_os() not in wanted


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


def expected_path(stem: str) -> Path:
    """The expectations file for a case: `<name>.<os>.txt` when it exists, else `<name>.txt`.

    Per-OS files exist only where the observable result really differs (a `BUFSIZ`-like constant,
    the aggregate ABI); everything else stays in one platform-neutral file.
    """
    per_os = EXPECTED_DIR / f"{stem}.{host_os()}.txt"
    return per_os if per_os.is_file() else EXPECTED_DIR / f"{stem}.txt"


def collect_cases() -> list[Case]:
    cases: list[Case] = []
    for src in sorted(CASES_DIR.glob("*.mu")):
        cases.append(Case(src.stem, src, expected_path(src.stem)))
    # examples/*.mu that define `main` also run as cases; the rest are library units
    # for the multi-unit example (such as examples/math.mu).
    for src in sorted(ROOT.glob("examples/*.mu")):
        if not defines_main(src):
            continue
        cases.append(Case(src.stem, src, expected_path(src.stem)))
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

    These are semantic-level exercises — visibility (`private` / `public` across units and
    libraries), import forms, declaration shapes — whose `main` is written for the checker, not
    for execution (many print nothing or return an arbitrary value). Features that deserve a
    runtime assertion belong in `tests/cases/` (run cases) instead.
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


class WarnCase:
    """A case that must compile (exit 0) and print the expected warning lines.

    The spec is one expected substring per line plus `@ <args>` lines (for example `@ --strict`);
    `#` is a comment. Warnings are stderr-only, so they never fail a normal build.
    """

    def __init__(self, name: str, src: Path, spec: Path) -> None:
        self.name = name
        self.src = src
        self.spec = spec


def collect_warn_cases() -> list[WarnCase]:
    if not WARN_DIR.is_dir():
        return []
    return [WarnCase(s.stem, s, WARN_DIR / f"{s.stem}.warn")
            for s in sorted(WARN_DIR.glob("*.mu"))]


class IrCase:
    """A case that asserts on the *optimized* LLVM IR (`--emit-ir`).

    Used for target/linkage policy: which symbols survive whole-program dead-code elimination.
    Each .txt line is `+ <regex>` (must match) or `- <regex>` (must not match); `#` is a comment.
    """

    def __init__(self, name: str, src: Path, spec: Path) -> None:
        self.name = name
        self.src = src
        self.spec = spec


def collect_ir_cases() -> list[IrCase]:
    if not IR_DIR.is_dir():
        return []
    def spec_for(stem: str) -> Path:
        per_os = IR_DIR / f"{stem}.{host_os()}.txt"
        return per_os if per_os.is_file() else IR_DIR / f"{stem}.txt"

    return [IrCase(s.stem, s, spec_for(s.stem)) for s in sorted(IR_DIR.glob("*.mu"))
            if not s.stem.startswith("lib_")]


class AbiCase:
    """A cross-language case: `tests/abi/<name>.mu` plus a C shim `tests/abi/<name>.c`.

    The shim is compiled by the C compiler and handed to `muc build --link`, so the two sides are
    really different compilers' objects: this is what checks the C ABI of `extern` / `export`
    aggregates (byval / sret), which no single-language test can observe.
    """

    def __init__(self, name: str, src: Path, shim: Path, expected: Path) -> None:
        self.name = name
        self.src = src
        self.shim = shim
        self.expected = expected


def collect_abi_cases() -> list[AbiCase]:
    if not ABI_DIR.is_dir():
        return []
    return [AbiCase(s.stem, s, ABI_DIR / (s.stem + ".c"), EXPECTED_DIR / f"{s.stem}.txt")
            for s in sorted(ABI_DIR.glob("*.mu"))
            if not s.stem.startswith("lib_")]


def c_compiler_command() -> list[str]:
    """The C compiler: `$CC` when set (the same variable `muc build` uses for linking), else gcc."""
    raw = (os.environ.get("CC") or "gcc").strip()
    return shlex.split(raw) if " " in raw else [raw]


def run_abi_case(case: AbiCase, verbose: bool) -> tuple[bool, str]:
    if not case.shim.is_file():
        return False, f"missing C shim {case.shim}"
    if not case.expected.is_file():
        return False, f"missing expected file {case.expected}"
    shim_obj = BUILD_DIR / (case.name + "_shim.o")
    cc = c_compiler_command()
    ccmd = cc + ["-c", str(case.shim.relative_to(ROOT)), "-o", str(shim_obj.relative_to(ROOT))]
    try:
        rc = subprocess.run(ccmd, cwd=ROOT, capture_output=True, text=True,
                            errors="replace", timeout=RUN_TIMEOUT)
    except (subprocess.TimeoutExpired, OSError) as e:
        return False, f"cannot compile the C shim ({e}): " + " ".join(ccmd)
    if rc.returncode != 0:
        msg = ["C shim failed to compile", "  command: " + " ".join(ccmd), "  stderr tail:"]
        msg += ["    " + ln for ln in (rc.stderr.strip().splitlines() or ["<empty>"])[-8:]]
        return False, "\n".join(msg)

    exe = BUILD_DIR / (case.name + (".exe" if sys.platform == "win32" else ""))
    cmd = resolve_compiler() + [
        "build",
        str(case.src.relative_to(ROOT)),
        "-o", str(exe.relative_to(ROOT)),
        "--link", str(shim_obj.relative_to(ROOT)),
        "--run",
    ]
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return False, f"timeout (>{RUN_TIMEOUT}s): " + " ".join(cmd)
    except OSError as e:
        return False, f"cannot execute compiler `{cmd[0]}` ({e})"
    return compare_run_output(case.expected, 0, r, cmd, verbose)


def abnormal_exit(rc: int) -> bool:
    """The process was killed by the OS instead of returning (a stack overflow, an access violation).

    POSIX reports -signal and a Windows SEH crash reports a negative NTSTATUS; `muc --run` re-exits with
    the child's code, which arrives as the unsigned 32-bit NTSTATUS (3221225725 = 0xC00000FD,
    STATUS_STACK_OVERFLOW). A case that expects 3 ("abnormal termination") accepts any of these.
    """
    return rc < 0 or rc >= 0xC0000000


def stderr_needles(path: Path) -> list[str]:
    """`@stderr-contains <text>` marker lines: substrings the run's stderr must contain.

    stdout is what the body of the file compares; stderr is only searched, because it carries the
    compiler's own lines as well. A case that pins a runtime message — the uncaught-exception report,
    a bounds message — lists just the part it needs.
    """
    needles: list[str] = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        for raw in f.read().splitlines():
            line = raw.strip()
            if line.startswith("@stderr-contains"):
                text = line[len("@stderr-contains"):].strip()
                if text:  # an empty needle would match anything
                    needles.append(text)
    return needles


def compare_run_output(expected: Path, expected_code: int, r, cmd: list[str],
                       verbose: bool) -> tuple[bool, str]:
    """Compare the run's exit code and stdout with the expectation (compiler status lines stripped)."""
    if r.returncode != expected_code and not (expected_code == 3 and abnormal_exit(r.returncode)):
        msg = [
            f"build --run failed (exit code {r.returncode}, expected {expected_code})",
            "  command: " + " ".join(cmd),
            "  stderr tail:",
        ]
        msg += ["    " + ln for ln in (r.stderr.strip().splitlines() or ["<empty>"])[-8:]]
        return False, "\n".join(msg)

    for needle in stderr_needles(expected):
        if needle not in (r.stderr or ""):
            return False, ("stderr does not contain " + repr(needle) + "\n  --- stderr ---\n" +
                           "\n".join("  " + ln for ln in (r.stderr or "<empty>").splitlines()))

    actual = normalize(r.stdout)
    with open(expected, "r", encoding="utf-8", newline="") as f:
        wanted = normalize(f.read())
    # Marker lines (`@exit`, `@ <args>`) are not part of the output
    wanted = [ln for ln in wanted if not ln.startswith("@")]
    while wanted and wanted[-1] == "":
        wanted.pop()

    if actual == wanted:
        return True, "ok"

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


def run_ir_case(case: IrCase, verbose: bool) -> tuple[bool, str]:
    if not case.spec.is_file():
        return False, f"missing expectation file {case.spec} (+ / - regex per line)"
    # `@ <args>` lines add compiler options (for example `@ --exports=public`).
    checks: list[tuple[str, str]] = []
    extra: list[str] = []
    for raw in case.spec.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("@platform"):
            continue
        if line.startswith("@"):
            extra += shlex.split(line[1:].strip())
            continue
        if line[0] not in "+-":
            checks.append(("?", line))
            continue
        checks.append((line[0], line[1:].strip()))

    exe = BUILD_DIR / (case.name + (".exe" if sys.platform == "win32" else ""))
    ir = BUILD_DIR / (case.name + ".ll")
    cmd = resolve_compiler() + [
        "build", str(case.src.relative_to(ROOT)),
        "--emit-ir", str(ir.relative_to(ROOT)),
        "-o", str(exe.relative_to(ROOT)),
    ] + extra
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return False, f"timeout (>{RUN_TIMEOUT}s): " + " ".join(cmd)
    except OSError as e:
        return False, f"cannot execute compiler `{cmd[0]}` ({e})"

    if r.returncode != 0:
        msg = ["build --emit-ir failed", "  command: " + " ".join(cmd), "  stderr tail:"]
        msg += ["    " + ln for ln in (r.stderr.strip().splitlines() or ["<empty>"])[-8:]]
        return False, "\n".join(msg)

    text = ir.read_text(encoding="utf-8", errors="replace")
    problems: list[str] = []
    for kind, pattern in checks:
        if kind == "?":
            problems.append(f"bad expectation line (must start with +, - or @): {pattern!r}")
        elif not re.search(pattern, text, re.MULTILINE):
            if kind == "+":
                problems.append(f"missing from IR: {pattern}")
        elif kind == "-":
            problems.append(f"still present in IR: {pattern}")
    if not problems:
        return True, "ok"
    lines = ["IR assertion failed"] + ["  " + p for p in problems]
    if verbose:
        lines.append("  --- full IR ---")
        lines += ["  " + ln for ln in text.splitlines()]
    return False, "\n".join(lines)


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
    if not case.expected.is_file():
        return False, f"missing expected file {case.expected}"
    _, expected_code, extra = parse_expected(case.expected)
    exe = BUILD_DIR / (case.name + (".exe" if sys.platform == "win32" else ""))
    cmd = resolve_compiler() + [
        "build",
        str(case.src.relative_to(ROOT)),
        "-o", str(exe.relative_to(ROOT)),
        "--run",
    ] + extra
    try:
        r = subprocess.run(
            cmd, cwd=ROOT, capture_output=True, text=True,
            errors="replace", timeout=RUN_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return False, f"timeout (>{RUN_TIMEOUT}s): " + " ".join(cmd)
    except OSError as e:
        return False, f"cannot execute compiler `{cmd[0]}` ({e}); set MUC_CMD, e.g. `build/muc.exe`"

    return compare_run_output(case.expected, expected_code, r, cmd, verbose)


def run_error_case(case: ErrCase, verbose: bool) -> tuple[bool, str]:
    """The case must be rejected; every .err line substring must appear in the diagnostics."""
    if not case.spec.is_file():
        return False, f"missing expectation file {case.spec} (one required error substring per line)"
    # `@ <args>` lines add compiler options (same convention as the run and IR cases), so a
    # diagnostic that only appears under a flag (for example `--single-thread`) can be pinned.
    wanted: list[str] = []
    unwanted: list[str] = []
    extra: list[str] = []
    for raw in case.spec.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("@"):
            extra += shlex.split(line[1:].strip())
            continue
        if line.startswith("!"):
            # `!<text>`: must NOT appear. A diagnostic that explains the wrong thing is a defect of
            # its own (#134), so a case can pin the absence of a misleading explanation.
            unwanted.append(line[1:].strip())
            continue
        wanted.append(line)
    cmd = resolve_compiler() + ["check", str(case.src.relative_to(ROOT))] + extra
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
    present = [u for u in unwanted if u in text]
    if present:
        lines = ["diagnostics contain text that must not appear: " + ", ".join(repr(p) for p in present),
                 "  actual output:"]
        lines += ["    " + ln for ln in text.strip().splitlines()[:10]]
        return False, "\n".join(lines)
    missing = [w for w in wanted if w not in text]
    if missing:
        lines = ["diagnostics are missing expected keywords: " + ", ".join(repr(m) for m in missing), "  actual output:"]
        lines += ["    " + ln for ln in text.strip().splitlines()[:10]]
        if verbose:
            lines.append("  --- full stderr ---")
            lines += ["  " + ln for ln in (r.stderr or "").splitlines()]
        return False, "\n".join(lines)
    return True, "ok"


def run_warn_case(case: WarnCase, verbose: bool) -> tuple[bool, str]:
    """The case must compile successfully; every .warn line is a substring that must be printed."""
    if not case.spec.is_file():
        return False, f"missing expectation file {case.spec} (one required warning substring per line)"
    wanted: list[str] = []
    extra: list[str] = []
    for raw in case.spec.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("@"):
            extra += shlex.split(line[1:].strip())
            continue
        wanted.append(line)
    cmd = resolve_compiler() + ["check", str(case.src.relative_to(ROOT))] + extra
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
    if r.returncode != 0:
        lines = ["the compiler rejected this program (a warning case must compile and exit 0)",
                 "  command: " + " ".join(cmd), "  output:"]
        lines += ["    " + ln for ln in text.strip().splitlines()[:10]]
        if verbose:
            lines.append("  --- full stderr ---")
            lines += ["  " + ln for ln in (r.stderr or "").splitlines()]
        return False, "\n".join(lines)
    missing = [w for w in wanted if w not in text]
    if missing:
        lines = ["warnings are missing expected keywords: " + ", ".join(repr(m) for m in missing),
                 "  actual output:"]
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
    warn_cases = collect_warn_cases()
    ir_cases = collect_ir_cases()
    abi_cases = collect_abi_cases()
    if args.filter:
        cases = [c for c in cases if args.filter in c.name]
        err_cases = [c for c in err_cases if args.filter in c.name]
        check_cases = [c for c in check_cases if args.filter in c.name]
        warn_cases = [c for c in warn_cases if args.filter in c.name]
        ir_cases = [c for c in ir_cases if args.filter in c.name]
        abi_cases = [c for c in abi_cases if args.filter in c.name]

    skipped: list[str] = []
    def fence(case_list, spec_of):
        kept = []
        for c in case_list:
            if platform_skips(spec_of(c)):
                skipped.append(c.name)
            else:
                kept.append(c)
        return kept

    cases = fence(cases, lambda c: c.expected)
    err_cases = fence(err_cases, lambda c: c.spec)
    warn_cases = fence(warn_cases, lambda c: c.spec)
    ir_cases = fence(ir_cases, lambda c: c.spec)
    abi_cases = fence(abi_cases, lambda c: c.expected)
    if (not cases and not err_cases and not check_cases and not warn_cases
            and not ir_cases and not abi_cases):
        print(f"no cases match `{args.filter}`")
        return 1
    total = (len(cases) + len(err_cases) + len(check_cases) + len(warn_cases)
             + len(ir_cases) + len(abi_cases))

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

    for case in warn_cases:
        ok, msg = run_warn_case(case, args.verbose)
        if ok:
            passed.append(case.name)
            print(f"PASS {case.name} (warning)")
        else:
            failed.append(case.name)
            print(f"FAIL {case.name} (warning)")
            print(msg)

    for case in ir_cases:
        artifacts.append(ROOT / "build" / (case.src.stem + ".o"))
        artifacts.append(BUILD_DIR / (case.name + (".exe" if sys.platform == "win32" else "")))
        artifacts.append(BUILD_DIR / (case.name + ".ll"))
        ok, msg = run_ir_case(case, args.verbose)
        if ok:
            passed.append(case.name)
            print(f"PASS {case.name} (ir)")
        else:
            failed.append(case.name)
            print(f"FAIL {case.name} (ir)")
            print(msg)

    for case in abi_cases:
        artifacts.append(ROOT / "build" / (case.src.stem + ".o"))
        artifacts.append(BUILD_DIR / (case.name + (".exe" if sys.platform == "win32" else "")))
        artifacts.append(BUILD_DIR / (case.name + "_shim.o"))
        ok, msg = run_abi_case(case, args.verbose)
        if ok:
            passed.append(case.name)
            print(f"PASS {case.name} (c abi)")
        else:
            failed.append(case.name)
            print(f"FAIL {case.name} (c abi)")
            print(msg)

    print()
    print(f"passed {len(passed)} / {total}")
    print(f"  ({len(cases)} run cases, {len(err_cases)} must-fail cases, "
          f"{len(check_cases)} check cases, {len(warn_cases)} warning cases, "
          f"{len(ir_cases)} ir cases, {len(abi_cases)} c abi cases)")
    if skipped:
        print(f"  skipped on {host_os()}: " + ", ".join(skipped))
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
