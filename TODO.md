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

- [ ] 标准库 — **`std.cstdio` 的常量仍是 Windows CRT 的硬编码**（`BUFSIZ = 512`、`FILENAME_MAX = 260`），
  而 glibc 的真值是 8192 / 4096；`cstd.stdio` 那边已经是平台正确值。本想写
  `public const int BUFSIZ = cstd.stdio.BUFSIZ;`，但 0.0.128 实测报「initializer of `const` unit-level
  variable must be a compile-time constant」（跨 unit 限定名不进常量求值器），所以暂时维持硬编码，
  `cstdio` 用例已改成不依赖该值的写法。要修得先让跨 unit 的 const 引用参与常量求值，
  或把 `std.cstdio` 退化成 `cstd.stdio` 的别名层。

- [ ] 标准库 — **macOS 平台层缺失**：`libs/std/platform/macos` 只有 `std/ctypes.mu`，
  `std/thread/backend` 与 `libs/cstd/platform/macos` 都没有；0.0.128 只落了 Linux。
  Linux 的 backend（pthread + ucontext）大体可复用，差异在 mach 的线程状态接口与 dyld（`_NSGetExecutablePath` 已有）。

- [ ] C ABI — **SysV 下含 float 的聚合仍未实现**（0.0.128 已把整数类聚合按两个 INTEGER eightbyte 打包）：
  需要 per-eightbyte 的 INTEGER/SSE 分类；目前 codegen 对这类签名直接报错拒绝，
  `abi_agg` / `abi_float` 用例已用 `@platform windows` 隔离。

- [ ] 性能 — **issue #18：`weak` 注册表是全局单链表 + 全局 spin lock，注销走 O(W)**
  （https://github.com/finlab/Mulan/issues/18）。改法需要设计决策：按目标对象挂 weak 链表（`mu_weak_node`
  加 `next_in_target` + 目标对象里放头指针），或做成开放寻址表；两者都改 RC/弱引用运行时布局，
  还要先有基准（该 issue 目前**无 benchmark**）。非正确性 bug，无 weak 的程序不受影响（IR 里不发射任何代码）。
  动作：先补一个 weak 创建/销毁的基准，再按基准决定改法。

- [ ] 性能 / 语义 — **issue #19：`--single-thread` 非原子引用计数开关**
  （https://github.com/finlab/Mulan/issues/19）。SPEC §12 #74/#87 记为「未决定」；issue 实测每对象
  33.4 ns vs C malloc/free 19.5 ns。落地需要：`--single-thread` 编译期开关贯穿 RC emit 路径
  （retain/release 之外的 fence 也要处理）、与 `std.thread`/`para` 的互斥诊断（用了线程就必须报错或忽略开关）、
  以及一组基准对比。属于特性 + 决策，不是随手能改的 bug。
