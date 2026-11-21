"""Shared MUC_CMD splitter for the CLI checks.

A backslash Windows path (or any path containing spaces) must stay one token; shlex would mangle it.
"""
import os
import shlex
from pathlib import Path


def muc_tokens(default: str = "muc") -> list[str]:
    raw = (os.environ.get("MUC_CMD") or default).strip()
    if " " not in raw:
        return [raw]
    as_path = Path(raw)
    if as_path.is_file():
        return [str(as_path)]
    return shlex.split(raw)
