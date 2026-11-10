# mu-lan

精巧、现代的类 C 小语言。保留 C 的硬能力（值语义、指针运算、手动内存、struct、C ABI），
去掉历史包袱（预处理器、隐式收窄、整数真值、`do-while`、C 风格 `for`）。

# 版本

当前版本：**0.0.41**（版本规则与每次 build 的记录见 [`docs/versions.md`](docs/versions.md)）

- 语言规范：[`SPEC.md`](SPEC.md)（草案 v0.1，逐条标注「已定 / 建议 / 待定」）
- 语法示例：[`example.mu`](example.mu)、[`math.mu`](math.mu)

## 当前状态

| 组件 | 状态 |
|---|---|
| 编译器主体（C++20，`src/`） | ✅ 移植完成 |
| 词法 `src/lexer.cpp` | ✅ |
| 语法 `src/parser.cpp` | ✅ |
| 语义 `src/sema.cpp` | ✅ |
| LLVM 后端 `src/codegen_llvm.cpp` | ✅ 唯一后端（LLVM C++ API → COFF，gcc 链接） |
| 回归测试 `tests/` | ✅ 216 / 216（57 成功用例 + 146 必须报错 + 13 check） |
| 语言边界审查 | [`docs/design-review.md`](docs/design-review.md) |
| 标准库 | 无（只有内建函数 + libc） |

实现路线（见 `SPEC.md` §14）：**阶段 A** 生成 C99 验证语义（0.0.2 起已移除，代码保留在
git 历史 `cf3af48`）；**阶段 B** 用 LLVM C++ API 直接构造 IR、产出 COFF 目标文件，再由 gcc 链接。
当前只有 LLVM 后端。

## 构建

依赖：**CMake ≥ 3.20**、**Ninja**、**g++ ≥ 11**（C++20）、**gcc**（链接 LLVM 目标文件）、
**LLVM ≥ 17**（开发库，必需）。

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

> 若 `cmake`/`ninja` 已在 PATH，直接 `cmake -S . -B build -G Ninja && cmake --build build` 即可。

## 快速开始

```powershell
# 语义检查
build\mulanc.exe check example.mu

# 查看 AST
build\mulanc.exe dump math.mu

# 编译 + 运行（生成 .ll + COFF .o，再用 gcc 链接）
build\mulanc.exe build example.mu --run

# 保留生成的 IR
build\mulanc.exe build example.mu --emit-ir build/example.ll

# 回归测试
$env:MULANC_CMD = "build/mulanc.exe"
python tests/run_tests.py
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
  codegen_llvm.cpp LLVM 后端（LLVM C++ API → COFF 目标文件，gcc 链接）
  loader.cpp       unit 加载（use -> 同名 .mu）
docs/
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
    Point z = {};                  // 聚合零值初始化（数组/结构体用 {}）

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
| 位取反 | `~` | `~`（与 C 相同；逻辑非用 `not`） |
| 幂运算 | `pow()` | 只有 `pow()`，不提供运算符（`^` 是异或） |
| 自增 | `++`/`--` | `x++` / `x--`（仅语句，无返回值） |
| 条件 | 任意标量真值 | 必须是 `bool` |
| 声明 | 声明符混杂 | 类型在前，`int* p, q;` 中两个都是指针 |
| 初始化 | `T x = ...` | 声明只用 `=`：`T x = expr` / `= {...}` / `= {}`（聚合零值）/ 标量 `= 0`、`= null` |
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

- `tests/run_tests.py`：216 个用例（57 个成功 + 146 个必须报错 + 13 个 check），全绿。
- 必须报错用例覆盖：`const`/`string` 可写性、`const` 必须初始化、`auto` 多声明符一致、枚举重复、枚举与整数隐式转换/跨枚举比较/枚举算术/枚举成员误用（不存在、赋值、取地址、非枚举类型限定）、`void*` 算术、`int`→枚举、非 void 函数掉出末尾、字面量溢出、`string`/切片与 `null` 比较、类型/参数/下标/构造错误等。
- 编译器自身在 `-D_GLIBCXX_ASSERTIONS -fstack-protector-strong` 下重编后跑同一套测试（两个后端）全绿；
  注意 `-D_GLIBCXX_DEBUG` 与 LLVM 共享库 ABI 不兼容，不能用于链接 LLVM 的构建。
- 冷编译 `--clean-first` 零 warning（`-Wall -Wextra`）。
