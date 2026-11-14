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
  - 完整规则已写成三张表（直接可用 / 需要包装 / 包装也不行），公开摘要进 `docs/StdLibSPEC.md` §8；
  - 导出函数 nothrow 只警告不强制；`--emit-header` 暂缓；句柄用 `Handle<T>` 优先。

## 待办

- [ ] 测试/工具链 — **DLL 变量导入没有回归用例，且依赖 GNU ld 的 auto-import**（2026-11-14 实测）：
  MinGW gcc + GNU ld 下 `extern int g;` / `int[4]` / `struct` / `double` / `const char*` 都能从 DLL 导入，
  读写、取地址、跨 DLL 互相可见都通过；但 Mu 侧不发 `dllimport`，同一个 `.o` 用 `lld-link` 链接会报
  `undefined symbol: g_arr`。要做：① `tests/abi/` 增加 C 变量 shim 用例；② 讨论 external global 是否发
  `dllimport`（需要开关，编译器无法区分"来自 DLL"还是"静态对象"）；③ 导出方向（Mu 生成 DLL、变量
  `dllexport`/`.def`）随 `agents/toolchain-plan.md` P11-4 一起做。
