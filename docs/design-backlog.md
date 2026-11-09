# 设计待定清单（backlog）

> 规则：用户提出的修改建议**先记录在这里**，不急于实现；等相关问题一起敲定后再统一实现。
> 已完成项在末尾留档，便于回顾。

## 待定（等决策）

### U1. Unicode 码点类型
- 选项：
  - A. 直接用 `int`（i32）表示码点（推荐，Go `rune=int32` 先例）；
  - B. 增加**别名** `rune` / `char32` = `int`（只提升可读性，同一类型）；
  - C. 增加**独立** `char32` 类型（强类型校验，代价是到处 `as`）。
- 命名注意：`unichar` 在 ICU/Objective-C 中通常指 **UTF-16 码元（16 位）**，不建议用这个名字表示 32 位码点。
- 待定：选 A/B/C；若 B/C，最终名字用 `rune` 还是 `char32`。

### U2. Unicode 字面量与转义
- `'\u{1F600}'` / `"\u{4E2D}"` 是否支持；是否允许代理区/非法码点（应拒绝）。
- 非 ASCII 字符字面量 `'中'` 当前报错；是否长期保持"只允许单字节字面量"。
- 是否引入 `u8'...'` / `u32'...'` 之类前缀区分字节与码点。

### U3. 字符串的码点/字节视图与迭代
- 是否需要 `s.chars()`（按 UTF-8 解码，产出码点 + 字节偏移）。
- 是否需要 `readonly byte[]` / `readonly u8[]` 视图（当前 `s[i]` 返回有符号 `char`，原始字节要 `as byte`）。
- `for c in s` 目前按**字节**遍历；是否保留，还是改成按码点（会破坏现有语义，需谨慎）。

### U4. `s[i]` 的返回类型
- 现状：`char`（有符号 i8），高字节为负。
- 选项：保持 `char`（C 风格）／改为 `byte`（无符号，UTF-8 字节更自然，但与 `'a'` 的 `char` 比较需字面量适配）。

### U5. 8 位类型命名收敛
- 现状：`byte`（u8），`ubyte`/`uchar` 是别名；`char`（i8）。
- 待定：是否只保留一个无符号名字（`byte` 或 `uchar`），还是继续保持别名。

### U6. 字符串构建与从字节构造
- 标准库 `StringBuilder`（内部 `byte[]`，`append(string/char/int)`、`build() -> string`）。
- 从 `byte[]` 构造 `string` 的入口：校验 UTF-8 还是允许 `unsafe` 显式构造。

### U7. readonly class 视图 / readonly 方法
- SPEC 表 98 已标"第一阶段不支持"；是否重新启用、语法如何写（`readonly Cat`、`readonly int f()` 与方法调用的交互）。

### U8. `ref` 参数
- SPEC 表 99 已标"第一阶段不支持"；语法 `void f(ref Point p)`，只借 struct、不可存储/返回。
- 实现前需定：与 `for ref` 的借用规则、与 RC/字段写入的交互。

### U9. interface → 派生 interface 向下转换
- SPEC 表 100 已标不支持；若启用需要运行期接口身份检查（desc/itable 里没有接口 id）。
- 需先定表示：是否在 desc 加接口 id 表。

### U10. 弱引用运行时
- 当前实现：全局 `{target, field}` 链表 + 自旋锁；目标销毁时清零。
- 待定：内存/锁竞争评估；是否改为每对象弱引用链表；多线程压力测试。

### U11. 多线程 RC 验证
- 原子 RC 只是基础；需要并发对象共享/析构的压测与内存序审查（relaxed 是否足够）。
- `--single-thread` 快速路径（SPEC 待定）是否要做。

### U12. 异常 / 泛型 / 并发
- 异常：非检查 + `finally`，Windows unwind ABI 需 spike（SPEC 3.14）。
- 泛型：单态化（未来）。
- 并发：`spawn/await/shared/atomic` 等保留字已预留。

## 已完成（留档）

- 0.0.19：`char` 改为有符号 8 位（C 风格）；`byte` 改为无符号 8 位；`ubyte`/`uchar` 作为别名；
  `'\xNN'` 按位模式解释；`s[i]` 返回 `char`（有符号）。
- 0.0.14：`weak` 目标销毁自动置 null（全局注册表 + 自旋锁）。
- 0.0.15：`readonly` class 视图 / `ref` 参数 / interface 向下转换正式列为第一阶段不支持（SPEC 表 98–100）。
- 0.0.6：`as` 向下转换（class→派生、interface→class）沿 desc 基类链运行期检查。
