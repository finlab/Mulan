# mu-lan

精巧、现代的类 C 小语言。保留 C 的硬能力（值语义、指针运算、手动内存、struct、C ABI），
去掉历史包袱（预处理器、隐式收窄、整数真值、`do-while`、C 风格 `for`、`++/--`）。

# 版本

当前版本：**0.0.1**（版本规则与每次 build 的记录见 [`docs/versions.md`](docs/versions.md)）

- 语言规范：[`SPEC.md`](SPEC.md)（草案 v0.1，逐条标注「已定 / 建议 / 待定」）
- 语法示例：[`example.mu`](example.mu)、[`math.mu`](math.mu)

## 当前状态

| 组件 | 状态 |
|---|---|
| 编译器主体（C++20，`src/`） | ✅ 移植完成 |
| 词法 `src/lexer.cpp` | ✅ |
| 语法 `src/parser.cpp` | ✅ |
| 语义 `src/sema.cpp` | ✅ |
| C99 后端 `src/codegen_c.cpp` | ✅ |
| LLVM 后端 `src/codegen_llvm.cpp` | ✅ 阶段 B（LLVM C++ API → COFF，gcc 链接） |
| 回归测试 `tests/` | ✅ 58 / 58（20 成功用例 + 38 必须报错用例），**两个后端都通过** |
| 语言边界审查 | [`docs/design-review.md`](docs/design-review.md) |
| 标准库 | 无（只有内建函数 + libc） |

实现路线（见 `SPEC.md` §14）：**阶段 A** 生成 C99 交给 gcc 验证语义；**阶段 B** 用 LLVM C++ API
直接构造 IR、产出 COFF 目标文件，再由 gcc 链接。两个后端共享同一套前端与测试，用
`--backend c|llvm` 切换。

## 构建

依赖：**CMake ≥ 3.20**、**Ninja**、**g++ ≥ 11**（C++20）、**gcc**（编译生成的 C / 链接 LLVM 目标文件）、
**LLVM ≥ 17**（开发库；用于 LLVM 后端）。

```powershell
# 没有 cmake/make 时可用 pip 装用户级版本
python -m pip install cmake ninja

# LLVM（MSYS2；会安装 llvm-libs / llvm-tools / lld）
C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm mingw-w64-x86_64-llvm"
```

构建编译器（CMake 会自动在 `C:/msys64/mingw64/lib/cmake/llvm` 找 LLVM）：

```powershell
python -m cmake -S . -B build -G Ninja
python -m cmake --build build
# 产物：build/mulanc.exe
```

只想要 C 后端、或机器上没有 LLVM：

```powershell
python -m cmake -S . -B build -G Ninja -DMULANC_WITH_LLVM=OFF
```

> 若 `cmake`/`ninja` 已在 PATH，直接 `cmake -S . -B build -G Ninja && cmake --build build` 即可。

## 快速开始

```powershell
# 语义检查
build\mulanc.exe check example.mu

# 查看 AST
build\mulanc.exe dump math.mu

# 编译 + 运行（默认 C 后端，输出到 build/<名字>.exe）
build\mulanc.exe build example.mu --run

# 用 LLVM 后端（生成 .ll + COFF .o，再用 gcc 链接）
build\mulanc.exe build example.mu --backend llvm --run

# 保留中间产物
build\mulanc.exe build example.mu --emit-c build/example.c
build\mulanc.exe build example.mu --backend llvm --emit-ir build/example.ll

# 回归测试（C++ 编译器 + 指定后端）
$env:MULANC_CMD = "build/mulanc.exe"
python tests/run_tests.py                 # C 后端
$env:MULANC_BACKEND = "llvm"
python tests/run_tests.py                 # LLVM 后端
```

## 目录结构

```
src/               编译器（C++20）
  main.cpp         命令行：check / dump / build
  lexer.cpp        词法：关键字、字面量、嵌套块注释
  parser.cpp       递归下降语法分析 -> AST
  ast.hpp          AST 定义 + sema 注解契约
  types.cpp/.hpp   类型模型
  sema.cpp/.hpp    名字解析 / 类型检查 / 注解
  codegen_c.cpp    C99 代码生成（阶段 A 后端）
  codegen_llvm.cpp LLVM 后端（LLVM C++ API → COFF 目标文件，gcc 链接）
  loader.cpp       unit 加载（use -> 同名 .mu）
docs/
  codegen-spec.md  C 后端实现规范
  llvm-backend.md  LLVM 后端设计说明与限制
tests/
  cases/*.mu       成功用例（编译 + 运行 + 比对期望输出）
  expected/*.txt
  errors/*.mu      必须被拒绝的用例，*.err 每行一个必须出现的错误子串
  run_tests.py     测试运行器（MULANC_CMD 可配置）
build/             生成物（.c / .exe / .obj），可随时删除
```

## 语言要点（速查）

```c
unit example;
use math;

extern int puts(readonly char* s);

struct Point { int x, y; }

int main() {
    Point p = {3, 4};              // 声明用 = 初始化
    Point q = Point(x = 1, y = 2); // 表达式用构造调用
    Point z = 0;                   // 零值初始化

    int[5] a = {1, 2, 3};          // 不足补零；数组是值类型
    for v in a { }                 // 值绑定
    for ref v in a { v *= 2; }     // 引用绑定
    for i in 0..len(a) { }         // 半开区间

    if not p.x == 0 and q.y > 0 { puts("ok"); }  // not 优先级低于比较

    int* buf = alloc(len(a) * sizeof(int)) as int*;
    free(buf as void*);
    return 0;
}
```

## 与 C 的关键差异

| 主题 | C | mu-lan |
|---|---|---|
| 逻辑运算 | `&& \|\| !` | `and` / `or` / `not`（`not` 是低优先级） |
| 位取反 | `~` | `!` |
| 幂运算 | `pow()` | 只有 `pow()`，不提供运算符（`^` 是异或） |
| 自增 | `++`/`--` | `x += 1` |
| 条件 | 任意标量真值 | 必须是 `bool` |
| 声明 | 声明符混杂 | 类型在前，`int* p, q;` 中两个都是指针 |
| 初始化 | `T x = ...` | 声明只用 `=`：`T x = expr` / `= {...}` / `= 0` |
| 循环 | `for(;;)` / `do-while` | `while` + `for-in` |
| 转换 | 大量隐式 | 仅无损加宽；其余 `as` |

## 已知限制（阶段 A）

- 数组类型用包装 struct 实现值语义，外部 C 结构体若含数组字段需手工对齐。
- 数组长度只支持整数字面量。
- 无 `switch`/`goto`/函数指针/可变参数/泛型/闭包/异常。
- `alignof` 用 gcc 的 `offsetof` 技巧实现。
- 切片字面量 `T[] s = {...}` 的生命周期是当前作用域（与 SPEC 一致）；返回它属于 UB。
- 全局变量初始化只接受常量表达式（C 静态存储期限制）。

## 质量检查

- `tests/run_tests.py`：58 个用例（20 个成功 + 38 个必须报错）。
  - C 后端：58/58；LLVM 后端：58/58（`MULANC_BACKEND=llvm`）。
- 必须报错用例覆盖：`const`/`readonly`/`string` 可写性、`const` 必须初始化、`auto` 多声明符一致、枚举重复、`void*` 算术、`int`→枚举、非 void 函数掉出末尾、字面量溢出、`string`/切片与 `null` 比较、类型/参数/下标/构造错误等。
- 编译器自身在 `-D_GLIBCXX_ASSERTIONS -fstack-protector-strong` 下重编后跑同一套测试（两个后端）全绿；
  注意 `-D_GLIBCXX_DEBUG` 与 LLVM 共享库 ABI 不兼容，不能用于链接 LLVM 的构建。
- 冷编译 `--clean-first` 零 warning（`-Wall -Wextra`）。
