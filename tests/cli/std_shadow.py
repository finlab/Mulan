"""CLI check: a `-I` directory must not silently replace a `std.*` unit.

`std.*` is the compiler's trusted implicit surface; a user directory that happens to contain `std/`
would replace the unit (layout and ABI included) without a word. The precedence stays as it is, but
the loader warns.

Usage:
    python tests/cli/std_shadow.py
    $env:MUC_CMD = "build/muc.exe"; python tests/cli/std_shadow.py
"""

from __future__ import annotations

import os
import shlex
import subprocess
import sys
from pathlib import Path

from muc_cmd import muc_tokens

ROOT = Path(__file__).resolve().parent.parent.parent
WORK = ROOT / "build" / "cli-std-shadow"
FAKE = WORK / "fakestd" / "std"


def muc() -> list[str]:
    return muc_tokens()


def main() -> int:
    FAKE.mkdir(parents=True, exist_ok=True)
    (FAKE / "string.mu").write_text(
        "public unit std.string;\n"
        "public struct String {\n"
        "    public isize length() { return 999; }\n"
        "}\n",
        encoding="utf-8",
    )
    entry = WORK / "useshadow.mu"
    entry.write_text("unit useshadow;\nint main() { return 0; }\n", encoding="utf-8")

    cmd = muc() + ["check", "-I", str(FAKE.parent), str(entry)]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", cwd=ROOT)
    out = (proc.stdout or "") + (proc.stderr or "")
    if "shadowed by" not in out:
        print("FAIL std shadow warning")
        print(f"  command: {' '.join(cmd)}")
        for line in out.splitlines()[:8]:
            print(f"    {line}")
        return 1
    print("ok   std shadow warning")

    # Without the -I directory there is no warning: the portable unit resolves normally.
    proc = subprocess.run(muc() + ["check", str(entry)], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", cwd=ROOT)
    out = (proc.stdout or "") + (proc.stderr or "")
    if "shadowed by" in out:
        print("FAIL spurious shadow warning without -I")
        return 1
    print("ok   no warning without -I")
    return 0


if __name__ == "__main__":
    sys.exit(main())
