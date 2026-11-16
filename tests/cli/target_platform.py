"""CLI checks for the platform / target validation of `muc build`.

The expectations are derived from the host the script runs on, so the same table covers a
Windows, a Linux and a macOS host:

  * a platform that names another architecture or another C runtime must be rejected
    (issue #22: the unit set would not match the object file);
  * a triple that names another OS, another architecture or another C runtime (musl vs glibc)
    is a cross target and needs `--cc` (issue #25: `x86_64-unknown-linux-musl` on a glibc host
    used to be linked with the host driver and "succeeded" as a glibc binary);
  * the host's own platform spec and triple must keep working.

Usage:
    python tests/cli/target_platform.py           # uses build/muc.exe
    $env:MUC_CMD = "build/muc.exe"; python tests/cli/target_platform.py
"""

from __future__ import annotations

import os
import platform
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ENTRY = ROOT / "tests" / "cases" / "arith.mu"
WORK = ROOT / "build" / "cli-target-platform"


def host_os() -> str:
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def host_arch() -> str:
    machine = platform.machine().lower()
    if machine in ("arm64", "aarch64"):
        return "aarch64"
    return "x86_64"


def host_abi() -> str:
    """The host C runtime family; empty when it cannot be told apart reliably."""
    if host_os() != "linux":
        return ""
    libc, _ = platform.libc_ver()
    return "gnu" if libc == "glibc" else ""


def other_arch() -> str:
    return "aarch64" if host_arch() == "x86_64" else "x86_64"


def other_os() -> str:
    return "linux" if host_os() != "linux" else "windows"


def host_triple() -> str:
    if host_os() == "windows":
        return f"{host_arch()}-pc-windows-msvc"
    if host_os() == "macos":
        return f"{host_arch()}-apple-darwin"
    return f"{host_arch()}-unknown-linux-" + (host_abi() or "gnu")


def compiler() -> list[str]:
    raw = (os.environ.get("MUC_CMD") or str(ROOT / "build" / "muc.exe")).strip()
    if " " not in raw:
        return [raw]
    as_path = Path(raw)
    if as_path.is_file():
        return [str(as_path)]
    return shlex.split(raw)


def run(extra: list[str]) -> tuple[int, str]:
    cmd = compiler() + ["build", str(ENTRY), "-o", str(WORK / "out.bin")] + extra
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=120)
    return r.returncode, (r.stderr or "") + "\n" + (r.stdout or "")


def cases() -> list[tuple[str, list[str], bool, list[str]]]:
    """(name, extra args, must be accepted, expected message fragments)."""
    out: list[tuple[str, list[str], bool, list[str]]] = []
    # The host's own platform spec: accepted, the unit set matches the object file.
    out.append(("host platform", [f"--platform", host_os()], True, []))
    out.append(("host platform with arch", [f"--platform", f"{host_os()}-{host_arch()}"], True, []))
    # Another architecture: the unit set would not match the object file (issue #22).
    out.append((
        "foreign arch platform",
        [f"--platform", f"{host_os()}-{other_arch()}"],
        False,
        ["selects the " + other_arch() + " units", "muc check --platform"],
    ))
    # Another OS: `build` cannot link another platform's units (unchanged behaviour).
    out.append((
        "foreign os platform",
        [f"--platform", other_os()],
        False,
        ["is not supported by `build` yet", "muc check --platform"],
    ))
    # The host's own triple: the host driver links it (unchanged behaviour).
    out.append(("host triple", ["--target", host_triple()], True, []))
    # Another architecture on the host OS: a cross target, it needs a cross driver.
    out.append((
        "foreign arch triple",
        ["--target", f"{other_arch()}-unknown-linux-gnu" if host_os() == "linux"
         else f"{other_arch()}-pc-windows-msvc"],
        False,
        ["is not the host", "--cc"],
    ))
    # musl against a glibc host: the C runtime differs, so the host driver must not link it
    # (issue #25). On a musl host the triple is the host's own target.
    musl = f"{host_arch()}-unknown-linux-musl"
    if host_os() == "linux" and host_abi() == "gnu":
        out.append(("musl triple on glibc host", ["--target", musl], False, ["is not the host", "--cc"]))
    elif host_os() == "linux":
        out.append(("musl triple on musl host", ["--target", musl], True, []))
    else:
        out.append(("musl triple", ["--target", musl], False, ["is not the host", "--cc"]))
    # A runtime that does not exist on the target OS: no driver can produce it, so the build
    # fails early instead of linking something that does not match the target.
    out.append((
        "musl runtime on a windows target",
        ["--target", "x86_64-pc-windows-musl"],
        False,
        ["musl runtime, which does not exist on Windows"],
    ))
    out.append((
        "msvc runtime on a linux target",
        ["--target", "x86_64-unknown-linux-msvc"],
        False,
        ["msvc runtime, which is a Windows runtime"],
    ))
    return out


def main() -> int:
    if not ENTRY.is_file():
        print(f"missing entry file {ENTRY}")
        return 1
    WORK.mkdir(parents=True, exist_ok=True)
    failures = 0
    for name, extra, accepted, wanted in cases():
        rc, text = run(extra)
        ok = (rc == 0) == accepted
        missing = [w for w in wanted if w not in text]
        if missing:
            ok = False
        status = "PASS" if ok else "FAIL"
        print(f"{status} {name}: muc build {' '.join(extra)} -> exit {rc}")
        if not ok:
            failures += 1
            if missing:
                print("  missing from the output: " + ", ".join(repr(m) for m in missing))
            print("  output:")
            for line in text.strip().splitlines()[:6]:
                print("    " + line)
    total = len(cases())
    print(f"{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
