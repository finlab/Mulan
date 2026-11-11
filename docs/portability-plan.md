# 跨平台方案（Linux / macOS / 交叉编译）

> **状态：方案已定，暂不实施**（2026-11-11 决定）。
> 等语言机制（异常、泛型、字符串类、`ref` 参数等见 `docs/design-backlog.md`）完善后再统一动手，
> 避免在语言语义还在变的时候把后端、ABI、标准库绑定各改一遍。
>
> 本文件记录：现状盘点、必须改动的点（带 `文件:行` 定位）、目标平台矩阵、分阶段计划与验收标准。
> 相关：`docs/llvm-backend.md`（后端现状与限制）、`SPEC.md` §14（实现路线）。

---

## 0. 「跨平台」其实是两件独立的事

| | 含义 | 主要难点 |
|---|---|---|
| **A. 编译器自身可移植** | muc 能在 Linux / macOS 上编译并运行 | CMake 找 LLVM、平台 API（`windows.h`、路径、控制台编码）、链接器默认值 |
| **B. 面向其它平台生成** | 在任一宿主上产出 Linux / macOS 的目标文件或可执行文件 | 目标 triple、目标后端初始化、**链接**、**C ABI 绑定** |

用 LLVM 生成别平台的目标文件**几乎免费**（选对 triple 即可）；
真正花钱的是 **链接** 和 **C 互操作的类型映射**。计划按这个边界拆分。

---

## 1. 现状盘点（已具备的能力）

| 项 | 现状 | 位置 |
|---|---|---|
| 控制台 UTF-8 代码页 | 已 `#ifdef _WIN32`，POSIX 分支空实现 | `src/main.cpp:64-80` |
| 路径反斜杠修正 | 已 `#ifdef _WIN32` | `src/main.cpp:86-92` |
| 可执行文件目录 | 有 `#else` 分支（探测 `/proc/self/exe`） | `src/loader.cpp:68-79` |
| 测试输出后缀 | 已按 `sys.platform` 区分 | `tests/run_tests.py:154` |
| 运行时依赖 | 只需 libc（`malloc/free/realloc/abort` + `std.cstdio` 的 libc 绑定），无自建 runtime | `src/codegen_llvm.cpp`、`libs/std/cstdio.mu` |
| 类型宽度 | 全部固定（`long` 恒 64 位），不受 LLP64/LP64 影响 | `src/types.cpp`、`src/codegen_llvm.cpp:187-193` |
| 布局计算 | 已委托语义层 `sizeOf/alignOf`，并显式插填充保证与 LLVM 一致 | `src/types.cpp:270-330`、`codegen_llvm.cpp:197-360` |

结论：**语言语义层面基本与平台无关**，改动集中在后端目标选择、链接、平台 API 与标准库绑定。

---

## 2. 必须改动的点

### 🔴 阻塞级（不改则完全跑不起来）

| # | 问题 | 位置 | 修法 |
|---|---|---|---|
| 1 | 目标 triple 写死 `x86_64-w64-windows-gnu` | `src/codegen_llvm.cpp:3740` | 改 `llvm::sys::getDefaultTargetTriple()`；新增 `--target` |
| 2 | 只初始化 Native 后端（`InitializeNativeTarget`），交叉编译认不出非宿主目标 | `codegen_llvm.cpp:3874/3881/3886/3889` | 改 `InitializeAllTargets()` / `AllTargetMCs` / `AllAsmPrinters` |
| 3 | `_aligned_malloc` / `_aligned_free` 是 MSVC CRT 符号，POSIX 不存在 | `codegen_llvm.cpp:1962`、`:3482` | 抽一层 `mu_alloc_aligned` / `mu_free_aligned`：Windows 用 `_aligned_*`；POSIX 用 `aligned_alloc`/`posix_memalign` + `free`（注意 size 必须是对齐倍数、`free` 配对差异） |
| 4 | 链接命令拼字符串交 shell（`std::system`），未转义内嵌引号/`$`/`&` | `src/main.cpp:357-359`、`:367` | 改 argv 数组 + `CreateProcess` / `posix_spawn`，不经过 shell |
| 5 | 默认链接器写死 `gcc` | `src/main.cpp:281` | 按平台取默认：Windows `gcc`、Linux `cc`、macOS `clang`；`CC` 环境变量优先级不变 |
| 6 | 输出名写死 `.exe` | `src/main.cpp:342` | 按平台加后缀（Windows `.exe`，其余空） |
| 7 | CMake 交叉链接组件只有 x86 | `CMakeLists.txt:63` | 补 `aarch64codegen/aarch64desc/aarch64info`，或直接链 `LLVM` 全量目标 |
| 8 | CMake LLVM 探测只认 MSYS2 路径 | `CMakeLists.txt:24-35` | 补 `/usr/lib/llvm-XX/lib/cmake/llvm`、`$(brew --prefix llvm)/lib/cmake/llvm`，以及 `llvm-config --cmakedir` 探测 |

### 🟡 正确性级（能编译但会算错）

| # | 问题 | 位置 | 说明 |
|---|---|---|---|
| 9 | macOS 无 `/proc/self/exe` | `src/loader.cpp:75` | 用 `_NSGetExecutablePath`；否则 `libs/` 自动探测失效，`std.string` 找不到 |
| 10 | `MUC_LIBS` 同时用 `;` 和 `:` 分隔 | `src/loader.cpp:87` | **现存 bug**：Windows 上 `C:\a;D:\b` 会被盘符冒号切断。改为 `#ifdef _WIN32` 用 `;` |
| 11 | C ABI 类型映射不准 | `libs/std/cstdio.mu:51-52` | **现存 bug**：`fseek(void*, long, int)` / `ftell` 里的 `long`，Mu 侧恒 64 位，Windows C ABI 的 `long` 是 32 位。跨平台必须引入 C ABI 类型层（`c_int`/`c_long`/`size_t`）或 per-target `libs` 覆盖层 |
| 12 | PIC / PIE 一致性 | `codegen_llvm.cpp:3746`（`Reloc::PIC_`） | Windows-gnu 是为避免 `relocation truncated to fit` 才要 PIC；Linux 需与默认 PIE 一致，macOS 默认要 PIC。保持 `PIC_` + 统一用 clang/cc 链接即可，但要在文档里写明理由 |
| 13 | 文件系统大小写敏感性 | 加载器（unit → 文件映射） | Windows 上 `unit Foo;` 能命中 `foo.mu`，Linux 不能。CI 在 Linux 跑一遍即可暴露全部不一致 |
| 14 | 仓库换行不统一 | 约 108 个文件 CRLF、95 个 mixed | 目前没坏，因为 `run_tests.py` 的 `normalize()` 会 `rstrip` 每行；建议补 `.gitattributes`：`* text=auto`，`*.mu/*.err/*.txt/*.md eol=lf` |

### 🟢 产品级（真要「多平台发行」才需要）

| # | 问题 | 说明 |
|---|---|---|
| 15 | **32 位目标无法支持** | 指针/slice 硬编码 8 字节、interface 16 字节（`types.cpp:281-320`），`usize/isize` 恒 64 位（`codegen_llvm.cpp:191`），而 `SPEC.md:129` 写的是「指针宽」。**建议：明确只支持 64 位目标，修正 SPEC 措辞**（改文档比参数化布局便宜得多） |
| 16 | CI 只有 Windows | `.github/workflows/ci.yml` 单 job `windows-latest` + MSYS2，需加 `ubuntu-latest` / `macos-latest` 矩阵 |
| 17 | 调试信息格式固定 DWARF | `initDebug()`（`DW_LANG_C99`）。COFF + DWARF 在 gdb 可用；接 MSVC 系工具需 CodeView（需单独决策） |
| 18 | 异常 ABI（尚未实现） | `SPEC.md` 计划先做 Windows EH。跨平台意味着 Itanium EH + SEH 两套，还要引 personality/unwind 运行时，会打破「只依赖 libc」的优势。**建议异常设计不依赖平台 unwind 表**（自研栈式 / setjmp），否则平台数翻倍成本翻倍 |
| 19 | 摆脱 C 工具链（阶段 B 遗留目标） | 想在一个宿主直接产出别平台可执行文件，最终要走 in-process `lld`（`ld.lld` / `ld64.lld` / `lld-link`）。macOS 额外需要 SDK 的 `libSystem.tbd`，是唯一绕不开的外部依赖 |

---

## 3. 目标平台矩阵（先定这个再动手）

| 目标 triple | 对象格式 | 链接方式 | 难度 |
|---|---|---|---|
| `x86_64-w64-windows-gnu` | COFF | `gcc`（现状） | 已完成 |
| `x86_64-pc-linux-gnu` | ELF | `cc`（默认 PIE） | 低 |
| `aarch64-unknown-linux-gnu` | ELF | `aarch64-linux-gnu-gcc` | 低（纯交叉生成） |
| `x86_64-apple-darwin` / `arm64-apple-darwin` | Mach-O | `clang` + SDK | 中（SDK 依赖） |
| `x86_64-pc-windows-msvc` | COFF | `lld-link` + MSVC CRT | 高（ABI + CRT），非必需 |

分界原则：**`--target` 只管「生成」；链接交给 `--cc` 指定的（交叉）工具链；C ABI 类型单独成层。**

---

## 4. 分阶段计划

### 阶段 1：muc 原生跑在 Linux（性价比最高，约 1–2 天）

- [ ] `CMakeLists.txt`：LLVM 探测补 POSIX 路径 / `llvm-config --cmakedir`；目标组件补全
- [ ] `getDefaultTargetTriple()` 取代硬编码 triple
- [ ] 全局 target 初始化（`InitializeAllTargets` 等）
- [ ] `--cc` 默认值按平台；输出后缀按平台
- [ ] `_aligned_malloc/_aligned_free` → 抽象层
- [ ] `MUC_LIBS` 分隔符按平台（#10）
- [ ] `.gitattributes`（#14）
- [ ] CI 加 `ubuntu-latest`

**验收**：Linux 上 `cmake --build` 零 warning；`python tests/run_tests.py` 全绿；`examples/example.mu` 输出与 `tests/expected/example.txt` 一致。

### 阶段 2：macOS（约 1–2 天）

- [ ] `_NSGetExecutablePath` 取代 `/proc/self/exe`（#9）
- [ ] 默认链接器 `clang`；SDK 路径探测（`xcrun --show-sdk-path`）
- [ ] arm64 后端组件
- [ ] CI 加 `macos-latest`（注意 runner 是 arm64：triple 必须按宿主取，不能假设 x86_64）

**验收**：macOS 原生全量测试通过；`--run`、`-g`（gdb/lldb）可用。

### 阶段 3：交叉编译 `--target`（约 2–3 天）

- [ ] `--target <triple>` / `--cpu` / `--features` 参数；`--emit-obj`（只出目标文件）
- [ ] per-target 的 C ABI 类型库（`libs/std/` 按平台拆 overlay，解决 #11）
- [ ] 链接：外接交叉工具链（`--cc aarch64-linux-gnu-gcc` 等）

**验收**：Windows 宿主上 `muc build --target x86_64-pc-linux-gnu --emit-obj` 产出合法 ELF；
在有交叉工具链的环境下产出可运行的 Linux 可执行文件。

### 阶段 4：摆脱宿主 C 工具链 + 调试/异常对齐（按需，数天）

- [ ] in-process `lld`（`ld.lld` / `ld64.lld` / `lld-link`）
- [ ] CodeView 调试信息（若需要 Windows 原生调试器）
- [ ] 异常 ABI 决策与实现（#18）

---

## 5. 与语言机制耦合、必须等它们的部分

延后不是因为工作量，而是因为这些东西**还在变**，现在做会白做：

| 语言机制 | 会牵动 |
|---|---|
| 异常（`try/catch/throw`） | 平台 EH ABI、personality 函数、是否需要 unwind 运行时（直接决定「只依赖 libc」能否保住） |
| 泛型（单态化） | mangling 与符号名 → 跨平台符号/AEH 兼容性 |
| 字符串类 / `StringBuilder` | 标准库绑定层结构（C ABI 类型层要不要、怎么拆） |
| `ref` 参数、class 值/引用模型（U7/U8/U23） | 调用约定与参数传递，跨平台 ABI 一致性 |
| 32 位目标是否支持 | 决定 `sizeOf/alignOf` 是否要参数化（#15） |

**先做语言机制，再一次性做平台化的判断成立的前提是**：期间不要引入新的平台相关假设
（例如新增 libc 绑定一律走 `c_long`/`size_t` 风格的类型名，不要直接用 Mu 的 `long`）。

---

## 6. 与跨平台无关、现在就该修的两个 bug

这两个是**现存缺陷**，不需要等平台化：

1. `libs/std/cstdio.mu:51-52`：`fseek` / `ftell` 用 Mu 的 `long`（恒 64 位）声明，
   Windows C ABI 的 `long` 是 32 位 → **当前在 Windows 上就是错的**。
2. `src/loader.cpp:87`：`MUC_LIBS` 把 `:` 当分隔符，Windows 盘符会被切断（`C:\a` 被拆成 `C` 和 `\a`）。
