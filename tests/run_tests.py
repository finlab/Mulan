"""mu-lan 回归测试运行器。

对每个用例执行
    <MULANC_CMD> build <case> -o build/tests/<name>.exe --run
把 stdout 与 tests/expected/<name>.txt 比较后给出 PASS/FAIL。

编译器命令通过环境变量 MULANC_CMD 配置（默认 `mulanc`），例如：
    $env:MULANC_CMD = "build/mulanc.exe"; python tests/run_tests.py   (PowerShell)

用法:
    python tests/run_tests.py                 # 全部用例（含根目录 example.mu）
    python tests/run_tests.py --filter arith  # 只跑名字含 "arith" 的用例
    python tests/run_tests.py -v              # 失败时打印完整实际输出
    python tests/run_tests.py --keep-c        # 保留生成的 .c（默认编完即删）

已知限制:
  * `mulanc build --run` 会把编译器状态行（`+ gcc ...`、`ok: <exe>`）
    与程序 stdout 混在一起，运行器会剔除以 `+ `、`ok: ` 开头的行再比较。
    因此用例自身不要输出以这两个前缀开头的行。
  * 只用标准库；在仓库根目录下直接 `python tests/run_tests.py` 运行。
"""

from __future__ import annotations

import argparse
import os
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

# 编译器命令：<MULANC_CMD> build <case.mu> -o <exe> --run
# 默认 `mulanc`（即编译出来的 mulanc.exe）。
DEFAULT_COMPILER = "mulanc"

# 编译器自身打到 stdout 的状态行前缀（与被剔除）
COMPILER_PREFIXES = ("+ ", "ok: ", "error: ")
RUN_TIMEOUT = 120  # 秒


def resolve_compiler() -> list[str]:
    """把 MULANC_CMD 拆成命令 token 列表。

    支持三种写法：
      * 不含空格（如 `mulanc`、`build\\mulanc.exe`）→ 单 token；
      * 是当前机器上存在的单个文件路径（允许含空格）→ 单 token；
      * 含空格且不是文件路径（如带参数的包装脚本）→ shlex 拆分。
    """
    raw = (os.environ.get("MULANC_CMD") or DEFAULT_COMPILER).strip()
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


def collect_cases() -> list[Case]:
    cases: list[Case] = []
    for src in sorted(CASES_DIR.glob("*.mu")):
        cases.append(Case(src.stem, src, EXPECTED_DIR / f"{src.stem}.txt"))
    # 仓库根的 example.mu 也作为一个用例
    cases.append(Case("example", ROOT / "example.mu", EXPECTED_DIR / "example.txt"))
    return cases


class ErrCase:
    """必须被编译器拒绝的用例：.err 每行是一个必须出现在错误输出里的子串。"""

    def __init__(self, name: str, src: Path, spec: Path) -> None:
        self.name = name
        self.src = src
        self.spec = spec


def collect_error_cases() -> list[ErrCase]:
    # 以 lib_ 开头的文件是多 unit 报错用例的依赖库，不作为独立用例
    return [ErrCase(s.stem, s, ERRORS_DIR / f"{s.stem}.err")
            for s in sorted(ERRORS_DIR.glob("*.mu"))
            if not s.stem.startswith("lib_")]


class CheckCase:
    """只需要通过语义检查（`check`）就必须成功的用例。

    用于 codegen 尚未覆盖、但语义层已完整支持的特性（如 class / interface）。
    """

    def __init__(self, name: str, src: Path) -> None:
        self.name = name
        self.src = src


def collect_check_cases() -> list[CheckCase]:
    if not CHECKS_DIR.is_dir():
        return []
    # 以 lib_ 开头的文件是多 unit 测试的依赖库，不作为独立用例
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
        return False, f"超时（>{RUN_TIMEOUT}s）: " + " ".join(cmd)
    except OSError as e:
        return False, f"无法执行编译器 `{cmd[0]}`（{e}）"

    if r.returncode == 0:
        return True, "ok"
    lines = ["语义检查失败（应通过）", "  命令: " + " ".join(cmd)]
    text = ((r.stderr or "") + "\n" + (r.stdout or "")).strip()
    lines += ["    " + ln for ln in text.splitlines()[:10]]
    if verbose:
        lines.append("  --- stderr 全文 ---")
        lines += ["  " + ln for ln in (r.stderr or "").splitlines()]
    return False, "\n".join(lines)


def normalize(text: str) -> list[str]:
    """剔除编译器状态行、行尾空白与末尾空行。"""
    out: list[str] = []
    for line in text.splitlines():
        if line.startswith(COMPILER_PREFIXES):
            continue
        out.append(line.rstrip())
    while out and out[-1] == "":
        out.pop()
    return out


def run_case(case: Case, keep_c: bool, verbose: bool) -> tuple[bool, str]:
    """返回 (是否通过, 说明)。"""
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
        return False, f"超时（>{RUN_TIMEOUT}s）: " + " ".join(cmd)
    except OSError as e:
        return False, f"无法执行编译器 `{cmd[0]}`（{e}）；可用 MULANC_CMD 指定，如 `build/mulanc.exe`"

    if not case.expected.is_file():
        return False, f"缺少期望文件 {case.expected}"

    if r.returncode != 0:
        msg = [
            f"build --run 失败（退出码 {r.returncode}）",
            "  命令: " + " ".join(cmd),
            "  stderr 尾部:",
        ]
        msg += ["    " + ln for ln in (r.stderr.strip().splitlines() or ["<空>"])[-8:]]
        return False, "\n".join(msg)

    actual = normalize(r.stdout)
    with open(case.expected, "r", encoding="utf-8", newline="") as f:
        wanted = normalize(f.read())

    if actual == wanted:
        return True, "ok"

    # 找第一个差异，打印上下文
    i = 0
    while i < len(actual) and i < len(wanted) and actual[i] == wanted[i]:
        i += 1
    lines = [
        f"输出不匹配：第一个差异在第 {i + 1} 行（共实际 {len(actual)} 行 / 期望 {len(wanted)} 行）",
        "  上下文 [expected | actual]:",
    ]
    lo, hi = max(0, i - 2), max(i + 3, min(len(actual), len(wanted)))
    for j in range(lo, hi):
        got = actual[j] if j < len(actual) else "<缺失>"
        exp = wanted[j] if j < len(wanted) else "<缺失>"
        mark = " " if j < len(actual) and j < len(wanted) and actual[j] == wanted[j] else ">"
        lines.append(f"  {mark} {j + 1:>3} | exp: {exp!r} | act: {got!r}")
    if verbose:
        lines.append("  --- 完整实际输出 ---")
        lines += ["  " + ln for ln in r.stdout.splitlines()]
        if not lines[-1]:
            lines.pop()
    return False, "\n".join(lines)


def run_error_case(case: ErrCase, verbose: bool) -> tuple[bool, str]:
    """用例必须被拒绝；.err 里每行子串都必须出现在错误输出中。"""
    if not case.spec.is_file():
        return False, f"缺少期望文件 {case.spec}（每行一个必须出现的错误子串）"
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
        return False, f"超时（>{RUN_TIMEOUT}s）: " + " ".join(cmd)
    except OSError as e:
        return False, f"无法执行编译器 `{cmd[0]}`（{e}）"

    text = (r.stderr or "") + "\n" + (r.stdout or "")
    if r.returncode == 0:
        return False, "编译器接受了这个应当报错的程序（漏检）\n  命令: " + " ".join(cmd)
    missing = [w for w in wanted if w not in text]
    if missing:
        lines = ["错误信息缺少预期关键词: " + ", ".join(repr(m) for m in missing), "  实际输出:"]
        lines += ["    " + ln for ln in text.strip().splitlines()[:10]]
        if verbose:
            lines.append("  --- stderr 全文 ---")
            lines += ["  " + ln for ln in (r.stderr or "").splitlines()]
        return False, "\n".join(lines)
    return True, "ok"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="mu-lan 回归测试运行器")
    ap.add_argument("--filter", help="只跑用例名包含该子串的用例")
    ap.add_argument("-v", "--verbose", action="store_true", help="失败时打印完整实际输出")
    ap.add_argument("--keep-c", action="store_true", help="保留生成的 C 源码与 exe")
    args = ap.parse_args(argv)

    cases = collect_cases()
    err_cases = collect_error_cases()
    check_cases = collect_check_cases()
    if args.filter:
        cases = [c for c in cases if args.filter in c.name]
        err_cases = [c for c in err_cases if args.filter in c.name]
        check_cases = [c for c in check_cases if args.filter in c.name]
    if not cases and not err_cases and not check_cases:
        print(f"没有匹配 `{args.filter}` 的用例")
        return 1
    total = len(cases) + len(err_cases) + len(check_cases)

    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    passed: list[str] = []
    failed: list[str] = []
    c_files: list[Path] = []
    for case in cases:
        c_files.append(ROOT / "build" / (case.src.stem + ".c"))
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
    print(f"通过 {len(passed)} / {total}")
    print(f"  （成功用例 {len(cases)}，必须报错用例 {len(err_cases)}，check 用例 {len(check_cases)}）")
    if failed:
        print("失败用例: " + ", ".join(failed))
        code = 1
    else:
        print("全部通过")
        code = 0

    if not args.keep_c:
        for p in c_files:
            try:
                p.unlink()
            except FileNotFoundError:
                pass
        # LLVM 后端的中间产物（build/<case>.ll / .o）
        for pattern in ("*.ll", "*.o"):
            for p in (ROOT / "build").glob(pattern):
                try:
                    p.unlink()
                except (FileNotFoundError, PermissionError):
                    pass
        for exe in BUILD_DIR.glob("*"):
            try:
                exe.unlink()
            except FileNotFoundError:
                pass

    return code


if __name__ == "__main__":
    raise SystemExit(main())
