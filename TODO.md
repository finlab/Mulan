# TODO

## 待决策

- **C ABI 边界的投影规则 / 是否引入 `std.ffi` 层**：处理 GitHub issue #10/#11/#15 时，文档 agent
  自行在 `docs/StdLibSPEC.md` 起草了一节「The C boundary」（把 `string`、`T[]`、`class`、sum type、
  异常投影成 C 形状的规则，并新增一个 `std.ffi` 边界单元），因为属于未确认的设计决策，**已撤回**。
  需要决策的两点：
  1. 是否要一个专门的边界单元（`std.ffi`），还是把投影 helper 放进各自的 L2 单元
     （`std.string.cstr()` / `buffer<T>.ptr` 已经存在）；
  2. `export` / `extern` 签名里到底允许哪些类型 —— 目前只由 0.0.121 的实现限制（无 handle /
     `delegate` / `T[]` / sum type / `ref`，见 SPEC 3.6 与 7.2）间接定义，没有一处把它写成完整规则。

  在 `agents/stdlib/` 里落决策记录后再提炼进 `docs/StdLibSPEC.md`。

  **2026-11-14 已定方案（待实施）**：→ 详见 `agents/stdlib/abi-boundary.md`
  - 要专门的边界单元 `std.ffi`（先做一个 unit，条目多了再拆）；
  - 完整规则已写成三张表（直接可用 / 需要包装 / 包装也不行），公开摘要**暂未**进
    `docs/StdLibSPEC.md`（该节曾被撤回，待实施时再提炼）；
  - 导出函数 nothrow 只警告不强制；`--emit-header` 暂缓；句柄用 `Handle<T>` 优先。

## 待办

- [ ] 测试/工具链 — **DLL 变量导入没有回归用例，且依赖 GNU ld 的 auto-import**（2026-11-14 实测）：
  MinGW gcc + GNU ld 下 `extern int g;` / `int[4]` / `struct` / `double` / `const char*` 都能从 DLL 导入，
  读写、取地址、跨 DLL 互相可见都通过；但 Mu 侧不发 `dllimport`，同一个 `.o` 用 `lld-link` 链接会报
  `undefined symbol: g_arr`。要做：① `tests/abi/` 增加 C 变量 shim 用例；② 讨论 external global 是否发
  `dllimport`（需要开关，编译器无法区分"来自 DLL"还是"静态对象"）；③ 导出方向（Mu 生成 DLL、变量
  `dllexport`/`.def`）随 `agents/toolchain-plan.md` P11-4 一起做。
  另：**`errno` / `stdin` / `stdout` / `stderr` 不是数据符号**（CRT 头里是宏 → `_errno()` /
  `__acrt_iob_func()`），`extern` 声明会 `undefined reference`；`dllimport` 解决不了，必须绑访问器函数。
  真正的数据符号（`_iob` / `_environ` / `_fmode`）在 msvcrt 导出表里、今天能链上，但属 Windows 私有，
  归 `std.win32`。详见 `agents/stdlib/c-bindings.md` §3.2。

- [ ] 工具链 — **切到 UCRT（2026-11-15 定）**：安装 `mingw-w64-ucrt-x86_64-gcc` + `mingw-w64-ucrt-x86_64-llvm`，
  用 ucrt64 工具链构建 muc 本身，`--cc` 默认指向 ucrt64 gcc；CI 的 import 白名单从 `msvcrt.dll` 改成
  `ucrtbase.dll` + `api-ms-win-crt-*.dll`；`cstd` 的平台键用 `windows-x86_64-ucrt`（msvcrt 变体保留）。
  依据：`agents/stdlib/c-bindings.md` §2.1、`agents/portability-plan.md` §3。

- [ ] 可选 — **用 MSVC 构建 muc 自己**：源码无 GNU 扩展，前提是 MSVC 版 LLVM（官方二进制/vcpkg）；
  另需 `/utf-8`、`src/loader.cpp` 的 `NOMINMAX`。评估见 `agents/portability-plan.md` §3。
