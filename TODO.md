# TODO

## 待决策

- **RC 环 × 依赖析构释放的资源**（2026-11-15 定调「后面阶段解决，现在留给程序员」）：
  闭包/对象成环时析构永不执行，不只是内存泄漏——fd、socket、锁、临时文件都不会归位，
  而且会级联（fd 耗尽 → 后续 open 全失败）。现阶段口径：资源型对象注册回调写 `[weak this]`，
  并在析构里显式注销；`[weak …]` 捕获已于 0.0.130 落地，调试期靠 P3 泄漏报告暴露。
  后续候选方案（三选一，评估后定）：① 环专用 cycle collector（trial deletion，只覆盖 env / 合成类）；
  ② 资源对象不靠析构，改显式 `close()` / `dispose()` + 作用域清理；③ 库层 `Resource` 契约 + debug 断言。
  触发条件：std 库出现第一个资源持有型类型（文件 / socket / 锁句柄的 RAII 包装），或真实程序踩到。
  背景与讨论结论见 `agents/design-backlog.md` U41。

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

- [x] codegen/LLVM — **「`-O2` 把 `if (x != 0)` 折叠成恒真」已定位并修复（0.0.144）**：
  **不是 LLVM 的 bug，是我们自己的 UB**。`Emitter::bindParams` 给 `ref` / `move` 参数按**被引用类型**分配槽
  （`slotFor` 早已正确用 `ptr`），却往里存 8 字节指针；类型小于 8 字节时即越界写。优化器据此删除 move 构造
  调用、折叠分支。修复：间接参数统一 `ptr` + 对齐 8。
  回归用例：`tests/cases/ref_slot_ub.mu`、`tests/cases/value_semantics_matrix.mu`。
  复盘与排查配方（`-opt-bisect-limit`、`llvm-reduce`、如何避免被文件名骗）见 `agents/llvm-assume-notes.md`。
  **教训：`-O0` 正确 + 独立 `opt` 复现 ≠ 优化器 bug；先查越界/对齐/poison。**

- [ ] sema/codegen — **放开「含用户析构器的 struct 作字段 / 数组元素」**（2026-11-15 评估，0.0.131）：
  销毁侧已经通了（`emitReleaseValue` 递归到 `has_dtor` 并调 `dtor_cname`，数组/sum/class 字段全走它），
  真正缺的是 ① 可拷贝性不递归——`noncopyable_struct()` 只看 `t->has_dtor`，放开字段后含 S 的宿主会
  偷偷变成可拷贝 → 双释放；② 字段/元素默认构造从没实现；③ 覆盖赋值无 destroy-then-construct；
  ④ 元素部分构造的异常清理。顺序建议：先递归 copyability（只收紧不改功能）→ 字段+定长数组元素 + 构造 →
  拷贝/移动（rule of 3/5）→ slice/容器。另：`ref S` 参数被「按值会拷贝」规则误伤，可直接放开。
  三条报错文案与 SPEC 3.4/3.13 已过期。这也是 `std.string` 真库化的前置（见 `agents/TODO.md` 第 26 条）。
  **进度**：设计定稿见 `agents/value-semantics.md`（POD/非 POD、谓词族、组合矩阵、分波计划）。
  ✅ W0（0.0.132）构造失败不跑用户析构器；✅ W1（0.0.133）谓词族 + 递归 copyability，功能不变；
  ✅ W2（0.0.134）隐式成员/元素构造（字段、静态数组元素、class 成员、`{}`、部分数组元素），
  POD 值零开销；✅ W3a（0.0.135）放开「struct 字段 / class 字段 / 静态数组元素」——
  `struct S { ~S(){} }` 现在可以作字段和定长数组元素，宿主拷贝仍被递归 copyable 拦住；
  ✅ W3c（0.0.136）`nocopy struct`（显式禁止拷贝，递归传播，也可用 `ref` 借用）；
  ✅ W5a（0.0.137）move 语义：`move x`（仅整个名字）+ `T(move T other)` 移动构造 + `movable` 谓词
  + 传参/返回 + 移动后原对象不可访问（may-move 分析、分支合并、循环内禁止搬外部变量）。
  move-only 类型（`nocopy` + 移动构造）端到端可用；✅ W5b（0.0.138）拷贝构造 `S(ref S other)` +
  合成赋值（copy-and-swap / move-and-swap，自赋值安全、强异常安全）。
  待做：用户显式 `operator=(ref S)` 覆盖、显式构造形式 `S(a)`、前缀清理（W3b）、动态数组元素构造（W4）、
  `ref` 参数进 method。

- [ ] codegen — **构造中途抛异常时，未构造的兄弟元素/成员也会跑用户析构体**（2026-11-15，0.0.135）：
  `S[3] a;` 中第 1 个元素的构造器抛出时，元素 1、2 的用户析构体都会执行（元素 1 半构造、元素 2 从未构造）。
  零值安全约束保证不会双重释放、不会崩，偏差只在于"析构体在从未构造的对象上产生可观察副作用"，
  违反 C++ [except.ctor] 的语义。修法：每个展开步骤建失败块，只销毁 0..k（失败槽用部分析构），
  并保证外层不再重复释放——等价于 C++ 的 landing pad。触发条件：用户报告析构计数不对，或 std 库出现
  依赖析构副作用的对象（文件/socket 句柄）。详见 `agents/value-semantics.md` §5.5。
  **另有一个已实测的 class 侧 bug**（✅ 已修，0.0.132）：构造抛异常时 `emitRelease(p)` 走 `emitDestroy`
  → 无条件跑用户析构器（实测 dtor 计数为 1），而构造中途的对象只有零值字段/半初始化状态；
  C++ 不会跑析构器。修法：`destroy_partial` 按对象 descriptor 派发到"正在执行的 ctor 层"的部分析构
  （自有字段 + 祖先完整析构），不跑本层用户析构体。详见 `agents/value-semantics.md` §5.6。

- [ ] 文档 — **`AGENTS.md` 的测试命令缺 `MUC_CMD`**（2026-11-15 实测）：文档写
  `python tests/run_tests.py`，但 `muc` 不在 PATH 上，直接跑会 576 个用例全部
  `cannot execute compiler \`muc\``。实际用法是 `$env:MUC_CMD="build/muc.exe"; python tests/run_tests.py`
  （`tests/README.md` 有写，`AGENTS.md` 没有）。把 `AGENTS.md` 构建与测试节奏表里的命令补全即可。

- [ ] parser/sema — **`.cpp` 超 1000 行的存量文件**：`sema_expr.cpp`（~1400 行）、`parser_expr.cpp`
  （~1080 行）已越过「每个 `.cpp` ≤ 1000 行」的约定，lambda / 捕获相关代码继续往里加更糟。
  按功能边界拆出 `sema_lambda.cpp`（`check_lambda` 及其捕获分析）与 `parser_lambda.cpp`（`parse_lambda`）；
  纯搬迁、不改行为，拆完跑全量（发现于 2026-11-15，0.0.130 review）

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

- [ ] parser/sema — **成员 `ref T operator[]` 不满足 `for ref` 的结构性检查**（2026-11-15 实测 0.0.130）：
  `struct R { usize length(); ref int operator[](usize i); }` + `for ref x in r` 报「user types do not
  support `for ref` unless `operator[]` returns `ref T`」，而同样签名的**自由函数** `ref int operator[](ref R, usize)`
  可以。根因：`sema_class2.cpp` 成员分支只设 `s->idx_method`，不设 `s->idx_fn`，而 `for ref` 的检查与
  `codegen_stmt.cpp` 都只看 `idx_fn`。两处改为「`idx_fn ? idx_fn : idx_method`」并加 `for ref` + 成员算子的用例。

- [ ] 文档收尾 — **四处已过期、会误导后续 agent 的描述**（2026-11-15 实测 0.0.130）：
  SPEC 3.15.7 表（泛型 `struct S<T> : I`、泛型接口继承、泛型接口作值类型、`Self[]` 均已可用，只剩
  「泛型 struct/class 不支持 `const` 成员」为真）；SPEC 3.16.5 的「Not implemented: capturing closures」
  （0.0.110 起捕获已实现，0.0.130 有弱捕获）；`agents/design-backlog.md` U42 的「S4（待做）」
  （S4 已在 0.0.119/0.0.120 落地）；`agents/TODO.md` 第 26/27 条仍写 `[]=` 可共存（0.0.101 已删除）。

- [ ] 性能 / 语义 — **issue #19：`--single-thread` 非原子引用计数开关**
  （https://github.com/finlab/Mulan/issues/19）。SPEC §12 #74/#87 记为「未决定」；issue 实测每对象
  33.4 ns vs C malloc/free 19.5 ns。落地需要：`--single-thread` 编译期开关贯穿 RC emit 路径
  （retain/release 之外的 fence 也要处理）、与 `std.thread`/`para` 的互斥诊断（用了线程就必须报错或忽略开关）、
  以及一组基准对比。属于特性 + 决策，不是随手能改的 bug。
