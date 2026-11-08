# 语言边界审查（2026-11-08）

目标：用测试把实现与 `SPEC.md` 的边界压出来，回答两个问题——
1. 各种边界条件下实现是否会误报/漏报？
2. 语言设定本身是否有冲突或漏洞？

## 1. 测试方法

- **成功用例** `tests/cases/*.mu`：编译、运行、逐行比对 `tests/expected/*.txt`。
- **必须报错用例** `tests/errors/*.mu`：`mulanc check` 必须失败，且 `*.err` 中每行子串都要出现在错误输出里。
- 另有手工压测：跨 unit 结构体、嵌套结构体、结构体含 string/数组字段、指针算术、`as` 截断、字面量溢出、错误路径 10 类。

当前规模：**20 个成功用例 + 37 个必须报错用例**，C++ 版 57/57 全绿。

## 2. 首轮扫描结果：13 类非法程序被放行

| 用例 | 违规内容 | SPEC 依据 |
|---|---|---|
| `const_assign` | `const int y = 1; y = 2;` | §3.7 `const` 绑定不可变 |
| `const_global_noinit` | unit 级 `const int c;` 未初始化 | §3.7 必须初始化 |
| `readonly_ptr_write` | `readonly int* p; p[0] = 1;` | §3.7 通过 readonly 视图不能写 |
| `readonly_slice_write` | `readonly int[] s; s[0] = 1;` | 同上 |
| `readonly_ref_loop` | `for ref v in <readonly 切片>` | 同上 |
| `string_write` | `string s = "a"; s[0] = 120;` | §3.3 string 不可变 |
| `auto_multi_type` | `auto a = 1, b = 2.0;` | §4 多声明符必须同一类型 |
| `enum_dup` | `enum E { A, B, A }` | §3.5 成员唯一 |
| `void_ptr_arith` | `void* p; p + 1` | §3.2 void* 不允许算术 |
| `int_to_enum` | `Color c = 1;` | §3.6 仅无损加宽（枚举不是 int 的加宽目标） |
| `float_narrow` | `float f = 1.5;` | 见 §4 决策：改为允许 |
| `string_eq_null` | `string s = 0; s == null` | 见 §4 决策：禁止，改支持 `s == ""` |
| `missing_return` | `int f() { }` | §7 见 §4 决策：编译错误 |

**全部已修复**（`src/sema.cpp`、`src/types.cpp`、`src/lexer.cpp`），并固化在 `tests/errors/`。

此外修掉：
- 整数字面量溢出静默回绕（`99999999999999999999` 曾被当成合法值）→ 现在报「整数字面量超出 64 位范围」。
- `int[0] a;` 报错信息从「意外的 `int`」改为「数组长度必须大于 0」。
- `string` 与 `null` 比较会在后端生成非法 C —— 现在前端直接拒绝。

## 3. 本轮确认并落实的语义（用户拍板）

| 主题 | 结论 |
|---|---|
| 非 void 函数末尾掉出 | **编译错误**（`main` 例外，等价 `return 0`） |
| `float f = 1.5;` | **允许**：字面量按上下文适配；变量间 `double→float` 仍需 `as` |
| `string == ""` | **支持**：按字节值比较（含长度）；**不允许**与 `null` 比较 |

## 4. 仍然存在的设计冲突 / 空白（需要你决定）

### 4.1 结构体含数组字段无法嵌套初始化 —— 已解决

```c
struct Box { int[3] v; }
Box b = {{1, 2, 3}};        // 现在合法
Pt[2] ps = {{4, 5}, {6, 7}}; // 数组元素是结构体
Rec r = {p = {1, 2}, name = "hi", nums = {1, 2, 3}}; // 命名字段嵌套
```

实现：`InitAggregate` 的成员从表达式改为 `Init`（可递归），parser 递归解析花括号，
sema 递归检查，codegen 递归生成嵌套初始化。`char[N]` 字段可以直接用字符串字面量初始化（补零）。

### 4.2 `for-in` 序列「只求值一次」—— 已解决

- 简单左值（变量、成员访问链）直接遍历；
- 其他表达式（如 `rows[idx()].v`）先求值到临时量再遍历，`idx()` 只调用一次；
- `for ref` 的序列必须是简单左值，否则编译错误（需要可寻址）。

### 4.3 枚举参与算术/比较

当前 `EnumType` 被当作 `int` 参与 `usual_arith`：

```c
enum A { X } enum B { Y }
if X == Y { }      // 合法（都按 int 比较）
int n = X + 1;     // 合法
```

C 也这样，但类型安全性弱。可选：只允许同枚举比较，跨枚举/枚举与 int 的算术需要 `as`。

### 4.4 指向数组的指针

`int[3]* p` 在类型系统里合法（`*p` 得到 `int[3]` 值，`p[i]` 得到 `int[3]`），但规范没写它的用途与索引语义，也没有测试覆盖。建议第一阶段禁止（或明确 `p[i]` 表示第 i 个数组元素）。

### 4.5 全局变量初始化限制

实现目前要求 unit 级初始化器是**常量表达式**（拒绝 `static int x = f();`）。`SPEC.md` §4 对 `const` 只写"建议要求编译期可求值（待定）"。需要确认：阶段 A 是否就按「常量表达式」锁定（C 静态存储期本来也只能常量初始化）。

### 4.6 可能想要但不必须的警告（当前一律不报）

- 非 `void` 函数有路径未返回值——已改为错误；但 `while true {}` 死循环会被误判（建议加"无限循环视为终止"识别）。
- 切片字面量逃逸（返回局部切片）——UB，无诊断。
- 读取未初始化局部变量——UB，无诊断（需要数据流分析，阶段 B 再做）。
- 大数组按值传参（`int[1000]`）——语义正确但性能陷阱，可考虑 lint。

### 4.7 其它小项

- `!`（按位取反）作用于枚举时返回枚举类型，结果类型更像 `int`。
- `sizeof(expr)` 未支持（`SPEC.md` 待定），目前只能 `sizeof(T)`。
- `char` / `byte` / `ubyte` 三个 8 位类型并存，`char` 参与算术提升到 `int`；确认这是想要的三分法。
- `main` 带参数（`argc/argv`）仍待定。

## 5. 建议的下一步

1. ~~嵌套聚合初始化~~（已实现）
2. ~~`for-in` 求值一次~~（已实现）
3. 枚举类型安全（4.3）：只允许同枚举比较，跨枚举算术需要 `as`。
4. 数组指针（4.4）：第一阶段禁止，或明确索引语义。
5. 警告/诊断基础设施（4.6）：切片逃逸、未初始化读、大数组按值传参。
6. 阶段 B（LLVM）前，先补 `sizeof(expr)` 与 `main(argc, argv)` 的决定。

## 6. 回归入口

```powershell
python -m cmake --build build
$env:MULANC_CMD="build/mulanc.exe"; python tests/run_tests.py    # 58 / 58
```
