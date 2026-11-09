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
- **已定（待实现 chars()）**：码点用 `int`（i32）：范围 0–0x10FFFF、算术/比较自然、`-1` 可作非法哨兵。
  `s.chars()` 枚举码点时返回 `int`（不用 `uint`：与整数字面量混算会变无符号，且无法表示 −1）。
  不新增 `unichar`/`rune` 类型（用户要求简洁、无别名）。

### U2. Unicode 字面量与转义
- `'\u{1F600}'` / `"\u{4E2D}"` 是否支持；是否允许代理区/非法码点（应拒绝）。
- 非 ASCII 字符字面量 `'中'` 当前报错；是否长期保持"只允许单字节字面量"。
- 是否引入 `u8'...'` / `u32'...'` 之类前缀区分字节与码点。

### U3. 字符串的码点/字节视图与迭代
- 是否需要 `s.chars()`（按 UTF-8 解码，产出码点 `int` + 字节偏移）。
- 是否需要 `readonly byte[]` / `readonly char[]` 视图（当前 `s[i]` 返回 `byte`；`for c in s` 按字节）。
- `for c in s` 目前按**字节**遍历；是否保留，还是改成按码点（会破坏现有语义，需谨慎）。

#### B1（实现缺口，待批量修）
- **`string` 隐式转 `readonly byte[]` / `readonly char[]`：sema 允许，但 codegen 没有转换分支**，
  会在 `build` 时报 `无法转换 string → readonly byte[]`。
- 修法二选一：codegen 按 `{ptr,len}` 同布局重建 slice；或在实现前先让 sema 拒绝（避免"check 通过、build 失败"）。
- `string + string` 目前不支持（concatenation 属于 U6 `StringBuilder`/库函数范畴）。

### U4. `s[i]` 的返回类型与字符字面量
- **已实现（0.0.20）**：
  - `char` 保持**有符号** i8（C 风格）；`byte` 是唯一无符号 8 位名；
  - **`s[i]` 与字符串 for-in 元素改为 `byte`**（UTF-8 字节处理）；
  - `'a'` 字符字面量是**可适配字面量**（默认 `byte`，可按上下文变为 `char`）：`s[i] == 'a'`、`char c = 'a'` 都自然。

### U5. 8 位类型命名收敛
- **已实现（0.0.20）**：不要别名；`byte` 是唯一无符号 8 位名，`ubyte`/`uchar` 已删除；
  `char` 为 i8（有符号）。

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

### U13. `string` 的功能边界：内置类型 vs 标准库（建议）
- **建议：类型内置、功能进标准库（minimal core）**。
  - 内置（编译器必须知道）：`string` 类型与布局 `{ptr, usize}`、字面量/NUL、零值、
    `s[i]` 读 `byte`、`len`、字符串 for-in、`==`/`!=`、C 互操作、`char[N]` 初始化；
  - 标准库（`str` / `utf8` / `fmt` 等 unit）：find/split/trim/replace/case/format/join/parse、
    UTF-8 编解码、`s.chars()`（码点 `int`）、字节视图、`StringBuilder`。
- 理由：
  - 布局必须被编译器知道（struct/class 字段、全局常量、C ABI、字面量静态存储）；
  - 字面量与 `==`/索引需要语法/类型规则，即使做成库类也要编译器特判（Java/C# 的 `String` 就是特判）；
  - 数据结构简单、算法易变（Unicode 表/大小写/分词），放库里升级成本低；
  - 避免核心语言不断加关键字/方法。
- 待定：库函数返回**新字符串**的所有权/生命周期（无 GC）：
  调用方提供缓冲、显式 allocator、还是 `StringBuilder` 持有 `byte[]`（见 U6）。
- 现状限制：`struct` 没有方法、`class` 是堆+RC，所以"纯库实现的 string"不可行；
  内置视图 + 库函数是可行路线。

### U14. C 互操作：`union`
- 结论（建议）：**值得加 `union` 值类型**，优先级高于位域。
- 典型 C API：Win32 `INPUT`、`VARIANT`/`PROPVARIANT`，Vulkan `VkClearColorValue`，
  以及很多「tag + union」的 tagged union 结构。
- 设计建议：
  - `union` 与 `struct` 同属 unit 级值类型，无方法/构造/析构；
  - 所有成员偏移 0，size = max(成员) 向上对齐到最大 align；
  - 成员限 POD：标量、指针、枚举、POD struct、定长数组；**禁止 class/interface**（同 struct 的理由）；
    第一阶段建议也禁止 `string`/切片（视图重叠语义易错），后续再议；
  - `= 0` 全零；聚合初始化先只支持 `= 0` + 显式成员赋值（C 的"初始化第一个成员"语义待定）；
  - 读写非最后写入的成员 = 未定义行为（C 语义），文档注明；可考虑 `unsafe` 标记，但先不加。
- 未定：是否支持匿名 union（C11，`u.i` 直接访问）；是否允许 union 嵌套在 class 字段中（布局上可以）。

### U15. C 互操作：packed / align
- 网络协议、部分 C API 需要 `#pragma pack(1)`。建议后续加 **`packed struct`**（或 `align(N)` 属性）：
  布局规则明确（无填充、align 1），比位域安全得多。
- 与 C 侧 `__attribute__((packed))` 的 ABI 需交叉测试。

### U16. C 互操作：位域（bit-fields）
- **建议第一阶段不支持**。原因：C 位域的分配顺序/跨单元边界是**实现定义**的（MSVC 与 GCC 不同），
  与"可预测布局"冲突，也增加目标 ABI/移植风险。
- 需要时用：
  1. 显式掩码 + 移位（跨平台最稳）；
  2. `byte[N]` + offset 常量；
  3. 生成 C shim/accessor（编译器已用 gcc 链接，可编入 C 胶水）；
  4. 以后若必须：设计**显式位宽**语法（类似 Zig packed struct / Rust bitfield crate），
     规定固定布局，而不是采用 C 的位域规则。

### U17. C 类型映射表（互操作易错点）
- Windows x64（LLP64）下 `long` 是 **32 位**，而本语言 `long` 是 64 位；写 `extern` 时必须映射：
  | C（Windows x64） | 本语言 |
  |---|---|
  | `char` / `signed char` | `char`（i8） |
  | `unsigned char` | `byte` |
  | `short` | `short` |
  | `int` / `long` | `int` |
  | `long long` | `long` |
  | `size_t` | `usize` |
  | `SSIZE_T` | `isize` |
  | `float` / `double` | `float` / `double` |
  | `_Bool` | `bool`（需验证 ABI：i1 内存 1 字节，但寄存器传参要交叉测试） |
  | `wchar_t`（16 位） | 暂用 `ushort`（无 `wchar` 类型） |
- 待定：是否提供 `c_int`/`c_long`/`c_size` 之类别名（用户当前要求"不要别名"，可能只在文档给映射表）；
  或做一个 `extern "C"` 头文件映射工具。

## 已完成（留档）

- 0.0.20：`char`=i8（C 风格）、`byte`=u8 且为唯一无符号 8 位名（删除 `ubyte`/`uchar` 别名）；
  `s[i]`/字符串 for-in 返回 `byte`；字符字面量默认 `byte` 且按上下文适配 `char`/`byte`。
- 0.0.19：`char` 改为有符号 8 位（C 风格）；`byte` 改为无符号 8 位；`'\xNN'` 按位模式解释。
- 0.0.14：`weak` 目标销毁自动置 null（全局注册表 + 自旋锁）。
- 0.0.15：`readonly` class 视图 / `ref` 参数 / interface 向下转换正式列为第一阶段不支持（SPEC 表 98–100）。
- 0.0.6：`as` 向下转换（class→派生、interface→class）沿 desc 基类链运行期检查。
