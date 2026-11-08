# codegen_c 实现规范（阶段 A：C99 后端）

本文件是 `src/codegen_c.cpp` 的实现依据。语义分析 `src/sema.cpp` 已经在 AST 节点上写好注解
（见 `src/ast.hpp` 顶部契约），后端**只读注解、不再做类型推导**。

> LLVM 后端（`src/codegen_llvm.cpp`）遵循同一套类型映射与 lowering 规则，差异见
> [`llvm-backend.md`](llvm-backend.md)。

## 0. 入口

```cpp
// 把整个程序生成一份自包含的 C99 源码（所有 unit 合并到一个 .c）
std::string generate(Program& prog);
```

`prog.units` 已按依赖在前排序；`prog.entry` 是入口 unit 名。

## 1. 输出结构

```
/* 预置头 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct { const char* ptr; size_t len; } mu_string;

static void mu_fill_cbuf(void* dst, size_t n, mu_string s);   // 定义紧跟声明

/* 1. struct 前置声明（解决指针循环） */
typedef struct mu_<unit>_<Name> mu_<unit>_<Name>;

/* 2. 按依赖顺序的类型定义（struct / 数组包装 / 切片） */
/* 3. 所有全局变量 */
/* 4. 所有函数原型（含 extern） */
/* 5. 函数定义 */
```

## 2. 名字修饰

| 实体 | C 名 |
|---|---|
| 本 unit 定义的函数 | `mu_<unit>_<name>`（`FuncDecl.cname` 已算好，直接用） |
| `extern` 函数 | 原名（`cname` 已是原名） |
| 全局变量 | `mu_<unit>_<name>`（`GlobalVar.cname`），`extern` 用原名 |
| 结构体 | `mu_<unit>_<Name>` |
| 枚举类型 | `mu_<unit>_<Name>`；成员常量 `mu_<unit>_<Enum>_<Member>` |
| 局部变量 / 参数 / 临时量 | 自己分配，避免与关键字和重名冲突（`<name>_<n>`、`mu_t<n>`） |

`main` 的 `cname` 已由 sema 设为 `main`。

## 3. 类型映射

| mu-lan | C |
|---|---|
| `bool` | `bool`（`<stdbool.h>`） |
| `byte` | `int8_t` |
| `ubyte` / `char` | `uint8_t` |
| `short` / `ushort` | `int16_t` / `uint16_t` |
| `int` / `uint` | `int32_t` / `uint32_t` |
| `long` / `ulong` | `int64_t` / `uint64_t` |
| `usize` / `isize` | `size_t` / `ptrdiff_t` |
| `float` / `double` | `float` / `double` |
| `void` | `void` |
| `string` | `mu_string` |
| `T*` | `<ctype(T)>*`（`readonly` 忽略） |
| `T[N]` | `mu_arr_<mangle(T)>_<N>` = `typedef struct { <ctype(T)> e[N]; } ...;` |
| `T[]` | `mu_slice_<mangle(T)>` = `typedef struct { <ctype(T)>* ptr; size_t len; } ...;` |
| `struct S` | `mu_<unit>_<S>` |

`mangle(T)`：把类型名里的非字母数字替换成 `_`，例如 `int` → `int32_t`、`Point*` → `mu_example_Point_ptr`、
`int*` → `int32_t_ptr`。用 `str(t)` 生成即可，保证稳定唯一。

类型定义按依赖顺序输出：先所有 struct 前置声明（`typedef struct mu_X mu_X;` 打破指针循环），
再用 DFS `ensure_type(t)` 输出依赖（struct → 字段类型，数组/切片 → 元素类型），最后输出自身。

## 4. 表达式lowering

`emit_expr(e, expected=None) -> str`，`expected` 用于隐式转换（string→char*、数组→切片、整数字面量）。

| AST | C |
|---|---|
| `IntLit` | 十进制字面量；若 `e.ty` 是 64 位无符号加 `ULL` 后缀；必要时 `(ctype)e.ty` |
| `FloatLit` | `1.0`；`e.ty=float` 加 `f` 后缀 |
| `CharLit` | `((uint8_t)97)` 或直接整数字面量 |
| `StringLit` | `(mu_string){"...", <字节数>}`（C 转义 `\"` `\\` `\n` `\t`，其余非可打印字节用 `\xNN`） |
| `BoolLit` | `true` / `false` |
| `NullLit` | `NULL` |
| `Name` | `kind=local`：`sym.cname`（若 `sym.by_ref` 则 `(*cname)`）；`global`：`sym.cname`；`enum_const`：`mu_<unit>_<Enum>_<Member>` |
| `Member` | `kind=field`：`obj.field`；`kind=global`（跨 unit）：`sym.cname` |
| `Index` | 数组：`base.e[i]`；切片：`base.ptr[i]`；指针：`base[i]`；string：`base.ptr[i]` |
| `Unary -,+` | `-x` / `+x` |
| `Unary &` | `&expr`（数组取址只在 `arr[i]` 上出现，自然变成 `&arr.e[i]`） |
| `Unary *` | `(*x)` |
| `Unary !`（按位取反） | `(~x)` |
| `Not` | `(!x)` |
| `Binary` | 原样；`and`→`&&`，`or`→`||`；两边可加括号保证优先级 |
| `Cast` | `((<ctype(dst)>)expr)`（`readonly` 去除是 no-op，仍可强转） |
| `Call`（func） | `fname(args...)` |
| `Call`（ctor） | `((<ctype(S)>){a0, a1, ...})`，按 `field_order` 重排；无参 → `((<ctype(S)>){0})` |
| `Call`（builtin） | `alloc`→`malloc`、`realloc`→`realloc`、`free`→`free` |
| `Sizeof` | `sizeof(<ctype>)`（`e.which=="alignof"` → `((size_t)offsetof(struct { char c; <ctype> x; }, x))`） |
| `Len` | 数组 → `((size_t)<N>)`；切片 → `(x.len)`；string → `(x.len)` |

隐式转换（`emit_expr(e, expected)` 中判定）：
- `expected` 是指针/`readonly char*` 且 `e.ty` 是 `string` → `(<expr>).ptr`
- `expected` 是 `T[]` 且 `e.ty` 是 `T[N]` → `(mu_slice_T){(<expr>).e, N}`
- `expected` 是数值且 `e.ty` 与之不同、`e` 是 `IntLit`/`FloatLit` → `((ctype)literal)`

## 5. 语句lowering

| AST | C |
|---|---|
| `Block` | `{ ... }` |
| `LocalVar` | `<ctype> <cname> [= init];`，`const` 加 `const`，`by_ref` 的变量不需要这里处理 |
| 零值初始化（`node.zero`） | `= {0}` |
| `InitExpr` | `= <expr>`（char[N] 且表达式是 string：`mu_fill_cbuf(<cname>.e, N, <expr>)`） |
| `InitAggregate`（struct） | `= {按字段声明顺序的表达式}`（命名字段按 `StructType.fields` 重排） |
| `InitAggregate`（数组） | `= {{e0, e1, ...}}`（外层是包装 struct，内层是数组） |
| `InitAggregate`（切片） | `= {(<T>[]){...}, <n>}`，元素类型 `T`，n = 元素个数 |

> 聚合初始化可嵌套：每个槽位本身可以是 `InitAggregate`，按槽位类型递归生成；
> 槽位是 `char[N]` 且给的是字符串字面量时，直接输出 `{"..."}`（C 会补零）。
| `AssignStmt` | `lhs op rhs;`（`=` 与复合赋值原样） |
| `ExprStmt` | `expr;`（void 调用等） |
| `IfStmt` | `if (cond) {...} else ...` |
| `WhileStmt` | `while (cond) {...}` |
| `BreakStmt` / `ContinueStmt` | `break;` / `continue;` |
| `ReturnStmt` | `return [expr];` |

### for-in

- `loop_kind == "range"`：
  ```c
  for (<ctype(var_ty)> v = (<ctype>)lo; v < (<ctype>)hi; v += 1) { ... }
  ```
  （`var_ty` 由 sema 给出，整数）
- `array`：
  ```c
  for (size_t mu_iN = 0; mu_iN < <N>; mu_iN += 1) {
      <ctype(elem)> v = <arr>.e[mu_iN];     // 值绑定
      ...
  }
  ```
  `by_ref` 时：`<ctype(elem)>* v = &<arr>.e[mu_iN];`，并在语义上该变量使用点解引用
  （给 `lvar.by_ref = True`，`Name` 渲染时输出 `(*cname)`）。
- `slice`：同上，元素访问 `<s>.ptr[mu_iN]`
- `string`：`uint8_t v = <s>.ptr[mu_iN];`

`for-in` 循环体包一层 `{ ... }` 保证变量作用域。

## 6. 其它要求

- 生成的 C 必须能在 `gcc -std=c99 -Wall` 下无错误通过（未使用变量类的警告可接受）。
- 所有函数先出原型，再出定义，因此允许调用定义在后的函数。
- 全局变量：`is_static` 加 `static`，`is_const` 加 `const`，`is_extern` 不加（并在原型区 `extern` 声明）。
  注意 `const` + `static` 组合成 `static const`。
- `is_extern` 的全局变量**只生成声明**（`extern <ctype> name;`），绝不能带初始化器（sema 已保证没有 init）。
- 每个函数的局部变量名唯一：用 `名字 + "_" + 序号`，序号在函数内递增；临时量 `mu_t<序号>`、循环下标 `mu_i<序号>`。
- 字符串字面量要正确转义字节（UTF-8 原样输出为 `\xNN` 或可见 ASCII）。
- `mu_fill_cbuf` 用 `static` 函数定义在预置区，超过容量时 `fprintf(stderr, ...)` + `abort()`。

## 7. 验收

```powershell
build\mulanc.exe build example.mu --run
```

必须与 `example.mu` 的预期输出一致（见 tests/expected/example.txt）。
另外 `build\mulanc.exe build tests/cases/*.mu` 的产物需全部通过 `tests/run_tests.py`。

## 8. 禁止事项

- 不要修改前端（`src/parser.cpp` / `src/sema.cpp` / `src/ast.hpp` / `src/types.cpp`）；如发现前端 bug，在最终报告里说明。
- 不要引入第三方依赖；后端只用 C++ 标准库 + LLVM（C 后端不依赖 LLVM）。
- 不要用 `printf` 输出程序结果（只能用于运行时错误）。
