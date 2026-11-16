"""CLI checks for the argument order of `muc build`.

The entry file is the first non-option argument, wherever it appears: `muc build --bounds=off a.mu`
and `muc build a.mu --bounds=off` must mean the same thing, matching `muc check`. Before the fix
the first argument was taken as the entry unconditionally, so `--bounds=off` became a source path
and the real file was reported as `unknown option`.

Usage:
    python tests/cli/build_args.py              # uses build/muc.exe
    $env:MUC_CMD = "build/muc.exe"; python tests/cli/build_args.py
"""

from __future__ import annotations

import os
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ENTRY = ROOT / "tests" / "cases" / "arith.mu"
WORK = ROOT / "build" / "cli-build-args"

failures = 0


def muc() -> list[str]:
    return shlex.split(os.environ.get("MUC_CMD", "muc"))


def run(args: list[str]) -> tuple[int, str]:
    proc = subprocess.run(muc() + args, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", cwd=ROOT)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def check(name: str, args: list[str], rc_match, text_need: str = "", text_miss: str = "") -> None:
    global failures
    rc, out = run(args)
    ok = rc_match(rc) and (not text_need or text_need in out) and (not text_miss or text_miss not in out)
    if ok:
        print(f"ok   {name}")
    else:
        failures += 1
        print(f"FAIL {name}")
        print(f"  command: {' '.join(muc() + args)}")
        print(f"  rc={rc}")
        for line in out.splitlines()[:8]:
            print(f"    {line}")


def main() -> int:
    WORK.mkdir(parents=True, exist_ok=True)
    entry = str(ENTRY)
    exe = str(WORK / "before.exe")
    check("option before entry", ["build", "--bounds=off", entry, "-o", exe],
          lambda rc: rc == 0, "ok:")
    check("option after entry", ["build", entry, "--bounds=off", "-o", exe],
          lambda rc: rc == 0, "ok:")
    check("two entry files", ["build", entry, entry],
          lambda rc: rc != 0, "unexpected argument")
    check("missing entry", ["build", "--bounds=off"],
          lambda rc: rc != 0, "missing entry file")
    check("unknown option", ["build", "--not-an-option", entry],
          lambda rc: rc != 0, "unknown option `--not-an-option`")

    if failures:
        print(f"failed {failures} check(s)")
        return 1
    print("all cli checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
