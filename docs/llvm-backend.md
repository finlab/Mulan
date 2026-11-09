# LLVM 后端（阶段 B）

用 **LLVM C++ API** 直接构造 IR，产出 **COFF 目标文件**，再由 gcc 链接成 exe。
这是编译器当前的唯一后端（0.0.2 起移除了 C99 后端，代码保留在 git 历史 `cf3af48`）。

```powershell
build\mulanc.exe build example.mu --run
build\mulanc.exe build example.mu --emit-ir build/example.ll
```

## 环境

- LLVM ≥ 17 开发库（本机 MSYS2 `mingw-w64-x86_64-llvm` 23.1.5）
- CMake 通过 `LLVMConfig.cmake` 找到 LLVM（必需；找不到时配置失败并给出提示）
- 目标三元组：`x86_64-w64-windows-gnu`，`Reloc::PIC_`（否则全局常量会得到 ADDR32 绝对重定位，
  gcc 链接时报 `relocation truncated to fit`）

## 类型映射

| mu-lan | LLVM |
|---|---|
| `bool` | `i1` |
| `byte` / `char` | `i8` |
| `short` / `ushort` | `i16` |
| `int` / `uint` / `enum` | `i32` |
| `long` / `ulong` / `usize` / `isize` | `i64` |
| `float` / `double` | `float` / `double` |
| `string` | `%mu_string = type { ptr, i64 }` |
| `T[]` | `%mu_slice = type { ptr, i64 }` |
| `T*` | `ptr`（opaque pointer） |
| `T[N]` | `%mu_arr_<elem>_<N> = type { [N x T] }` |
| `struct S` | `%mu_<unit>_<S>` |

与 mu-lan 的类型布局约定一致（字符串/切片 16 字节、数组包装 struct、结构体自然对齐）。

## 降级策略

- 所有局部变量用 `alloca` 承载，读写走 `load`/`store`；不手工做 SSA/phi（除 `and`/`or` 短路需要 phi）。
- 控制流：`if` / `while` / `for-in` 直接映射到基本块；`break`/`continue` 用标签栈。
- 字符串字面量：私有常量 `[N x i8]` + 常量 `%mu_string`；重复字面量缓存复用。
- `char[N] = "..."`：`memset` 清零后 `memcpy`。
- 数组 → 切片：取数组地址 + 长度，构造 `%mu_slice`（指向原存储，不做拷贝）。
- 指针算术：`getelementptr`；指针相减：`ptrtoint`/`sub`/`sdiv sizeof(T)`。
- `sizeof` / `alignof`：编译期常量（后端自己实现布局计算）。
- `length`：数组为常量；切片/字符串读 `len` 字段。
- 字符串 `==` / `!=`：比较长度 + `memcmp`。
- `alloc` / `free` / `realloc` → libc `malloc` / `free` / `realloc`。
- 结构体构造：`insertvalue` 链；零构造：`zeroinitializer`。
- 聚合初始化：先整体清零，再逐槽位写入（与 C 语义一致）。

## 验证

`tests/run_tests.py` 的 58 个用例全绿，`example.mu` 输出与 `tests/expected/example.txt` 逐行一致：

```powershell
$env:MULANC_CMD = "build/mulanc.exe"
python tests/run_tests.py
```

## 已知限制

- 尚未接入 LLVM 优化管线（当前 `CodeGenOptLevel::None`，只有 `-O0`）；`alloca` 未被 `mem2reg` 提升，
  性能不是现阶段目标。
- 全局变量初始化支持常量表达式子集（字面量、零值、常量聚合）；不支持全局算术常量折叠。
- 未实现调试信息（DWARF/CodeView）。
- 目标仅 x86_64 Windows（COFF + gcc 链接）；其它平台需调整 triple / 链接器。
- `bool` 在内存中是 1 字节 `i1`，与 C `bool` 兼容；但跨语言传递结构体时的 ABI 细节未做专门测试。
- **不要用 `-D_GLIBCXX_DEBUG` 构建链接 LLVM 的编译器**：debug 容器会改变 `std::string` 布局，
  与 LLVM 共享库 ABI 不兼容（访问冲突）。需要加强检查时用 `-D_GLIBCXX_ASSERTIONS`（不改布局）。
