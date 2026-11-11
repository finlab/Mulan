# 代码地图（code map）

> 目的：改某个功能时，**先看这里定位到文件和函数**，不要整读大文件。
> 约定：每个 `.cpp` ≤ 1000 行；`*_impl.hpp` 只放**内部**类声明与跨 TU 辅助声明，对外接口在 `*.hpp`。

## 1. 总览

```
main.cpp         CLI：参数解析、读文件、调用 loader → parser → sema → codegen
loader.cpp       多 unit 加载与 use 解析（unit 名 → 文件）
lexer.cpp        词法（关键字、字面量、\u{...}、注释）
parser*.cpp      语法：token → AST（ast.hpp）
sema*.cpp        语义：名字解析、类型检查、常量求值、多错误收集
codegen*.cpp     LLVM 后端：AST → IR → COFF；链接交给 gcc
types.cpp        Type / TypeKind、布局 type_size / type_align
mangle.cpp       符号名修饰
```

## 2. 文件索引

### 前端 · 词法与语法

| 文件 | 内容 | 关键函数 |
|---|---|---|
| `lexer.cpp` | 词法分析 | `tokenize`、`escape`、`unicode_codepoint` |
| `parser_impl.hpp` | `Parser` 类声明 + 内部辅助声明 | — |
| `parser_unit.cpp` | unit/struct/class/interface/枚举/函数签名 | `parse_unit`、`parse_struct`、`parse_class`、`parse_method`、`parse_operator_func` |
| `parser_stmt.cpp` | 语句与局部声明 | `parse_stmt`、`parse_local`、`parse_if/while/for/switch`、`parse_func` |
| `parser_expr.cpp` | 类型语法与表达式（优先级爬升） | `parse_type`、`parse_expr`、`parse_unary/postfix/call/primary` |
| `parser.cpp` | 对外接口 | `parse_source`、`parse_file` |

### 中端 · 语义

| 文件 | 内容 | 关键函数 |
|---|---|---|
| `sema_impl.hpp` | `Sema` 类声明 + 小工具/枚举常量池 | — |
| `sema_decl.cpp` | 入口流程、unit 收集、可见性、布局修饰符（`packed`/`align`）、诊断助手 | `run`、`collect`、`place_structs`、`check_layouts`、`resolve_unit_types`、`err/warn`、`implicit_ok` |
| `sema_types.cpp` | 类型解析、作用域、unit 级检查、函数签名检查 | `resolve_type`、`find_type_named`、`push/pop`、`lookup_*`、`check_unit`、`check_global`、`check_func` |
| `sema_class1.cpp` | struct/class/interface 成员检查、方法/构造/析构、类型常量、继承环、语句与局部变量检查 | `check_struct`、`check_class`、`check_interface`、`check_method`、`check_const_members`、`check_stmt`、`check_local` |
| `sema_class2.cpp` | 聚合初始化、赋值语句、return/if/switch/for 的检查 | `check_aggregate`、`check_assign_stmt`、`check_switch`、`check_for`、`root_is_const` |
| `sema_const.cpp` | 赋值兼容 + **编译期常量求值器** | `check_assign`、`assignable`、`eval_const*`、`const_ok`、`normalize_int` |
| `sema_expr.cpp` | 表达式类型检查 | `check_expr`、`check_expr_impl`、`check_name/member/binary/unary/ternary` |
| `sema_call.cpp` | 调用/重载/构造/`new`/`super`、`main` 入口检查 | `check_call`、`choose_overload`、`choose_method`、`check_new`、`check_entry` |
| `sema.cpp` | 对外接口 | `analyze`、`type_str`、`mangle` |

### 后端 · LLVM 代码生成

| 文件 | 内容 | 关键函数 |
|---|---|---|
| `codegen_impl.hpp` | `Emitter` 类声明（含 `ArgPack` 等嵌套类型） | — |
| `codegen_support.cpp` | 类型映射、结构体/class 布局镜像、符号缓存、常量 | `lt`、`structOf`、`classFieldOffset`、`zeroConst`、`constValue`、`fail` |
| `codegen_runtime.cpp` | RC 运行时、切片/动态数组、weak 注册表、局部引用清理 | `emitRetain/Release`、`emitDynArrayAlloc`、`emitSlice*`、`ensureWeakReg`、`releaseAllScopes` |
| `codegen_expr.cpp` | 表达式求值（上）：转换、比较、二元/三元、实参准备 | `evalBinary`、`compareOp`、`coerce`、`prepareArgs`、`releaseTemps` |
| `codegen_expr2.cpp` | 表达式求值（下）：调用、方法调用、`new`、向下转换 | `eval`、`evalCall`、`emitUserCall`、`evalNew`、`emitDowncast` |
| `codegen_stmt.cpp` | 语句发射与内存访问 | `stmt`、`stmtImpl`、`emitFor`、`emitSwitch`、`storeAt/loadAt` |
| `codegen_class.cpp` | vtable / itable / 构造析构 / class desc | `vtableOf`、`emitVtable`、`emitItable`、`emitCtor`、`emitDtor`、`emitClassDesc` |
| `codegen_func.cpp` | 函数、全局变量、调试信息与优化管线 | `emitFunction`、`emitGlobals`、`initDebug`、`run` |
| `codegen_llvm.cpp` | 对外接口 | `llvm_emit_text`、`llvm_emit_object` |

## 3. 「要改 X 去哪里」

| 需求 | 位置 |
|---|---|
| 加关键字 / 运算符 / 字面量转义 | `lexer.cpp`（关键字表、`escape`）、`parser_expr.cpp`（优先级）、`sema_expr.cpp`（类型规则）、`codegen_expr.cpp`（求值） |
| 加语句 | `parser_stmt.cpp` → `ast.hpp` → `sema_class1.cpp:check_stmt` → `codegen_stmt.cpp` |
| 加类型 / 改布局 | `types.cpp`（`type_size`/`type_align`）、`sema_decl.cpp`（`check_layouts`）、`codegen_support.cpp`（LLVM 镜像） |
| 改可见性 / 跨 unit 规则 | `sema_decl.cpp`（`decl_visible`/`unit_visible`）、`sema_types.cpp` |
| 改重载 / 调用解析 | `sema_call.cpp`；方法派发镜像在 `codegen_class.cpp` |
| 改 RC / 引用计数 | `codegen_runtime.cpp`；语义侧在 `sema_class1.cpp`（`check_assign_stmt` 等） |
| 改常量求值 | `sema_const.cpp`（`eval_const*`、`convert_const_to`） |
| 加内建 / 库函数绑定 | `sema_decl.cpp`（`libFn` 相邻的 `is_str` 等）、`codegen_support.cpp`（`libFn`） |
| 加 CLI 选项 | `main.cpp` |
| 加测试 | `tests/cases|errors|expected`（见 `tests/README.md`） |

## 4. 拆分约定（新增代码请遵守）

1. **单文件 ≤ 1000 行**；超过时按上面的"功能边界"新建一个 TU，而不是继续往里堆。
2. 新 TU 的成员函数定义写成 `Ret Class::name(...)`，放在 `namespace muc_detail { ... }` 里；声明进 `*_impl.hpp`。
3. 只被**一个** TU 用的文件级辅助函数加 `static`；被多个 TU 用的放 `*_impl.hpp` 声明、单处定义（或 `inline`）。
4. 默认参数只写在**声明**里，定义中不得重复。
5. 模板成员函数必须定义在头文件里，不能拆到 TU。
6. 新增源文件记得加进 `CMakeLists.txt`。

## 5. 构建与验证

```powershell
cmake --build build --clean-first          # 冷编译（零 warning 基线）
$env:MUC_CMD = "build/muc.exe"
python tests/run_tests.py                  # 全量（当前 328/328）
python tests/run_tests.py --filter const   # 只跑相关用例
```
