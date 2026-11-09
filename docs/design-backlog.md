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

### U18. 拥有型字符串 / 构建器的语义（建议）
- `string` 保持**不可变视图**（值描述符 `{ptr,len}`，赋值/传参共享底层字节，不拷贝、不释放）。
- 需要"可增长/可修改"时用**独立类型**，不把 `string` 改成拥有型或 COW：
  - 建议 `StringBuilder`（class，内部 `byte[]`，RC 管理缓冲）；`build() -> string` 返回指向其缓冲的视图，
    文档注明生命周期；需要独立副本时提供 `to_string()`（显式拷贝）。
  - 或先用裸 `byte[]` + 手工长度。
- 不建议 COW 字符串：线程安全/原子计数/迭代器失效复杂度高，收益有限（不可变视图已经零拷贝共享）。
- 赋值顺序（若将来出现拥有型字符串/值类型）：
  - **先复制/创建新值，再销毁旧值**（或 move 转移缓冲），不要先释放旧值，避免自赋值/别名导致 UAF；
  - RC 引用类型（class）继续"先 retain 新、再 release 旧"（现有实现）。
- 子串/切片：共享缓冲（视图），不深拷贝；需要独立数据时显式 copy。

### U19. 共享缓冲的不可变字符串：谁维护 buffer？
- 现状：`string` 是**借用视图** `{ptr,len}`，**不拥有缓冲、不做 RC/生命周期检查**；
  字面量在只读段（进程生命期），其他来源由创建者负责存活。
- 可选模型：
  - **A. 借用视图（现状）**：零成本、C 友好；靠文档/约定保证 owner 存活（Rust `&str`/C++ `string_view` 路线，但没有借用检查）。
  - **B. 拥有型字符串类型**（class 持 `byte[]`，RC）：`StringBuilder` 产出 `String`；视图从 `String` 借出，
    仍要求 owner 存活；`to_string()` 显式拷贝。
  - **C. 自持有视图** `{ptr,len,owner*}`：`owner` 是引用计数控制块（字面量为 null），
    复制视图时 owner +1，销毁 -1（类似 `shared_ptr` 别名）。能防悬空，但每次复制有原子计数成本、24 字节描述符。
  - **D. Arena**：请求/解析期 arena 拥有所有临时字符串，视图在 arena 重置前有效；适合编译器/服务器。
- 建议：**短期保持 A**（零成本），构建用 B（`StringBuilder` + 显式 `to_string()` 拷贝）；
  若后续实际出现悬空/生命周期 bug，再评估 C（自持有视图）或 D（arena）。
- 明确规则（无论哪种）：**不得把指向临时/栈缓冲的 `string` 返回或存到生命周期更长的地方**；
  库 API 返回视图时必须文档化 owner 与有效期。

#### U19.1 静态区 vs 堆缓冲：怎么区分、有没有运行时开销
- 常见做法：
  - **类型区分**：拥有型 `String`（必堆、必释放）vs 借用 `&str`（不释放）——Rust 路线，零运行时标记；
  - **GC 堆范围**：Go/Java 运行时按地址判断指针是否落在堆 span 内；无每串标记（Go 还会用 interior pointer 保活）；
  - **引用计数控制块**：堆串带 `{refcount, free_fn}`；静态字面量用"永生"控制块或 `null` owner（`Arc<str>`/Swift/Obj-C 常量串）；
  - **tagged pointer / SSO 位**：低位标记"内联/静态/堆"（Swift 小字符串、libstdc++ SSO 用指针比较），只在释放/COW/写入时检查；
  - **借用视图不区分**：`string_view`/`&str`/Zig slice 根本不关心 buffer 在哪，因为从不释放；
  - **Arena**：由 region 统一释放，无需逐串标记。
- 开销结论：
  - 热路径（`len`/`s[i]`/比较/视图复制）**不应**做 static/heap 判断；
  - 判断只放在冷路径：释放、克隆/COW、追加/修改；
  - 若用"指针低位 tag"且解引用前要掩码，会变成**每次访问**的开销——应避免；
  - 若用额外字段（owner 指针/flag），代价是描述符变宽（16B→24B）与 ABI 变化，但读取无分支；
  - RC 的 `null` 检查本来就有（retain/release 已判空），用 `owner==null` 表示静态**不增加额外分支**。

### U20. 单一 `string` 类型：受管视图（managed view，建议）
- 动机：多数字符串是运行时生成的堆缓冲；"借用视图 + 拥有类型"两分法不美观；又不想引入 tracing GC。
- 方案：`string = { ptr, len, owner* }`（24 字节）
  - `ptr` 直接指向字节（`s[i]`/`len`/比较**无额外间接层、无 tag 判断**）；
  - `owner` 指向引用计数控制块 `{rc, capacity?, bytes...}`；**null 表示静态/借用**（字面量、C 字符串、栈缓冲）；
  - 复制 string 时：`owner != null` 则原子 `rc+1`（relaxed）；作用域结束/覆盖/`del` 时 release，归零释放控制块；
  - 字面量 owner=null：retain/release 的判空本来就有，**不增加额外分支**；
  - 子串共享 owner（保活整个缓冲），小串可选择复制；
  - C 互操作：直接取 `ptr`；owner 不传给 C（C 侧不得 free）；从 C `char*` 构造时 owner=null（借用）。
- 与两分法对比：
  - 优点：单一类型；运行期字符串安全共享（不会因 builder/局部缓冲销毁而悬空）；无 GC；确定性释放；
  - 代价：描述符 16B→24B；每次复制堆串有原子计数；运行期字符串一次分配（控制块+字节可合并在同一块）；
  - 热路径（索引/长度/比较）不变慢；只有复制/释放才碰 RC。
- 与"RC 字符串对象"对比：对象方案每次访问要经对象指针取 data/len，且字面量需要静态对象/特判；
  受管视图省一层间接并天然支持 borrowed C 字符串。
- 构建：`StringBuilder` 仍是独立构建器（不算第二种 string）；`build()` 可转移/共享缓冲，
  由 owner 计数保证已产出的 string 不被破坏。
- 未定：小字符串优化（SSO，需要 tag/分支）、子串是否默认复制、控制块是否缓存哈希、weak 视图。

#### U20.1 线程安全与原子 RC 成本（对比 Java/C#/Go）
- Java/C#/Go 的字符串**不可变 + tracing GC**：跨线程共享**读**安全；复制引用**没有 RC 原子操作**。
  代价是 GC（分配、追踪、写屏障、延迟），不是 refcount。
- 共享可变的"引用变量"仍需同步（Java `final`/`volatile`，C# `readonly`/`volatile`/lock）：
  不可变对象本身安全，但对变量的并发读写仍是 data race。
- 原子 RC（Rust `Arc<str>`、Swift 字符串存储、我们的受管视图 owner）：
  每次复制/释放都有原子 op；大量共享时控制块所在 cache line 会被多核反复争用（ping-pong），
  高并发下可明显退化。缓解：move 优化、尽量传借用、线程本地/arena、或 `--single-thread` 非原子构建。
- 结论：**Java/C# 不付原子 RC 代价，付的是 GC 代价**；受管视图要付原子计数，需要靠 move/借用优化压低。

### U21. 原生指针保留范围；RC 对象取地址 / 取成员地址
- 现状（sema 过宽，已确认）：
  - 声明 `C*` 被拒绝，但**表达式可以绕过**：`&c`、`&c.x`、`&c.d`（d 是 class 字段）、
    `void* v = &c` 都能通过 `check`，产生指向 RC 句柄/对象内部的裸指针。
  - `&c.x` 得到的 `int*` 在 `del c` 后悬空；`&c.d` 指向 class 句柄槽，写它可绕过 retain/release。
- 建议策略：
  - **保留**裸指针用于：`alloc/free/realloc`、C 互操作（非引用类型）、
    `&struct_var` / `&struct.field` / `&arr[i]`（元素非 class/interface）、指针变量取址（`int**`）、指针算术。
  - **禁止**：
    - 对 class/interface 句柄取地址（`&c`、`&this`、`&iface`）；
    - 对**经由 class/interface 引用访问的成员**取地址（`&c.x`、`&c.d`、`&h.items[0]`），
      即使字段是标量/struct——生命周期不可表达，易悬空；
    - 对 class/interface 数组元素取地址（元素是句柄）。
  - 理由：RC 生命周期无法用裸指针表达；内部指针可绕过 retain/release（字段是引用时尤其危险）；
    C 互操作本来就不允许 class 对象过界；没有借用检查器。
- 合法替代：
  - 字段/结构体拷到局部变量，调用后再赋回：`S tmp = c.s; api(&tmp); c.s = tmp;`
  - 标量 out 参数同理；
  - 真正需要就地借用时，后续加 **`ref` 参数/返回（仅 struct）** 或显式 `unsafe` 块（保留字已留），
    而不是裸指针。
- 实现要点（待做）：`check_unary` 的 `&` 分支增加"操作数类型是引用 / 操作数是经引用访问的成员/下标"检查；
  `&this`、`&c`、`&c.x`、`&c.d`、`&classArray[i]` 报清晰错误；struct/数组/指针变量的取址保持可用。
- 注意：`&p`（`p` 是 `int*` 变量）应继续允许（C 的 `int**` out 参数）；它不是 class 句柄。

### U22. 数组/切片语义规范（静态数组、动态数组、C 分配、取址）
- 目标：明确"值数组 / 受管动态数组 / C 缓冲 / 裸指针"四类，避免生命周期与 RC 冲突。
- **类型与存储**
  - `T[N]`：**内联值数组**，N 编译期常量；放在变量/struct/class 字段里；无 owner、无 RC；
    赋值/传参按值复制（元素级 memcpy；元素不能是 class/interface/引用数组）。
  - `T[]`：**受管切片** `{ptr, len, owner*}`：
    - `owner == null` → **借用视图**（静态数组、C 缓冲、栈缓冲、字段）；
    - `owner != null` → **拥有型动态数组**（`new T[N]` 分配，RC）；
    - 赋值复制描述符并对 owner 做 retain（null 则跳过）；覆盖/离开作用域/`del` 做 release；
    - 子切片 `s[a..b]` 共享 owner（ptr 偏移 + 新 len），不复制；
    - `len(s)` 是 len；`s[i]` 是元素。
  - `T*`：**裸指针**，无长度、无 owner；只能来自 `&lvalue`、指针算术、`alloc` 转换。
- **创建与释放**
  - `new T[N]` → 零初始化、owner rc=1 的拥有型 `T[]`；`del s` 释放 owner（归零时释放缓冲）。
  - 静态数组 → `T[]` 隐式借用视图（owner=null，不复制）；也可显式 `view(arr)`（待定）。
  - C `malloc/alloc`：返回裸内存；**不能直接 `int[] b = malloc(...)`**（没有长度/类型）。
    需显式 `T[] b = slice_from_raw(ptr, n);`（内建，按期望类型推断 T）或只用 `T*` + 长度参数。
    该视图 owner=null（借用），必须用 `free(ptr)` 释放，**不能 `del`**。
  - `del` 只对拥有型有意义；对 owner=null 的切片：**建议运行期报错/调试断言**（而不是静默 no-op），
    避免把 `del` 与 `free` 混用。`del null` 仍为空操作（类句柄语义）。
- **取址规则**
  - ✅ `&buf[i]`（静态数组元素）、`&s[i]`（切片元素，元素非引用）、`&p`（指针变量，C `T**`）；
  - ✅ 中间地址：`&buf[i] + k`、`&s[i] + k`；指针算术按元素大小缩放；一过尾（one-past）可形成但解引用 UB；
  - ❌ `&buf` / `&s`（整个数组/描述符取址；不引入"指向数组/切片的指针"类型）；
  - ❌ 对 class/interface 句柄及其成员取址（见 U21）；❌ class/interface 数组元素取址。
- **边界检查**
  - 静态数组：**常量下标越界编译期报错**；运行期下标建议 debug 检查（release 可关）。
  - 切片：`s[i]` debug 边界检查（策略与 SPEC 待定项一致）；
  - 裸指针：不做检查（C 语义，越界 UB）。
- **复制/克隆**
  - 静态数组赋值 = 深拷贝；切片赋值 = 共享（owner retain）；
    `T[] d = clone(s)` 深拷贝为拥有型（内建/库函数，待定）。
- **元素含 RC 引用**
  - 静态 class 数组（`Cat[N]`）已支持（逐元素 retain/release）；
  - `T[]` 切片/动态数组元素**暂限非引用类型**（class/interface 切片仍禁止）；
    `new Cat[n]` 暂不支持，后续需定义逐元素 RC 与切片共享语义。
- **C 互操作**
  - 切片/数组不直接作为 C 参数（ABI 不同）：传 `&s[0]` + `len(s)`；
  - `alloc/free` 与 `new/del` 两套内存管理**不得交叉**。
- **待定**
  - `del` 对借用切片是"运行期报错"还是"静默忽略"；
  - 是否提供 `slice_from_raw`/`clone`/`view` 内建及命名；
  - 动态数组容量/扩容（后续 `Vec<T>` class）；
  - 切片 of class/interface 的实现（逐元素 RC + owner 释放）；
  - 边界检查的默认策略（debug/release）。

### B2（实现缺口，重要）：extern C 的聚合参数/返回值 ABI 不一致
- 现象：`extern` C 函数按值接收 16 字节 struct 时，mulan 侧调用崩溃（访问冲突）。
- 复现：
  - C：`struct P { long long a,b; }; long long take_p(struct P p);`（gcc 编译）
  - mu-lan：`struct P { long a,b; } extern long take_p(P p); take_p(p)`
  - gcc 生成的 `take_p` 期望 **RCX = &p**（MS x64 ABI：非 1/2/4/8 字节的聚合按引用传递）；
  - mulan/LLVM 生成的是 `RCX=a, RDX=b`（两个寄存器），链接运行 → access violation。
- 原因：codegen 构造 `FunctionType` 时把聚合**按值**作为参数，但**没有加 `byval`/`sret` 属性**；
  LLVM 后端对 first-class aggregate 的 Windows 调用约定与 Clang（Clang 会发 `byval`）不同。
- 影响：所有 `extern C` 的「struct/数组按值传参/返回」；内部 mulan 调用自洽所以测试没暴露。
- 修法（待做）：
  - 按目标 ABI 做聚合分类：Win64 下大小 1/2/4/8 字节按整数寄存器；其余用 `byval(T)`；
    返回大小非 1/2/4/8 用 `sret` 隐藏指针；
  - extern 声明与定义、调用点统一；
  - 加 C 侧交叉测试（gcc 编译的 `sizeof/offsetof` 与按值传/返 struct）。
- 这也说明"按值传参更快"必须看 ABI：内部寄存器传参很快，但不等于 C ABI 兼容。

### U23. class 是否改为 C++ 式值/引用模型（栈/堆统一）
- 提议：`Cat c;` 在栈上构造；`Cat& r = ...` 引用堆对象；尽量少堆分配；`struct` 保留为 C 互操作/POD。
- 可行性：**可行，但这是替换对象模型，不是增量调整**。当前 class = RC 句柄（始终堆、共享、`del`、weak、接口 RC），
  改成值语义后这些语义都要重做。
- 需要同时补齐的机制：
  - 引用 `T&`（非空别名、不可重绑定）或指针 `T*`；
  - 拷贝构造/拷贝赋值、移动构造/移动赋值、析构（rule of 0/3/5）；
  - 对象切片规则（值拷贝派生→基类）；
  - vtable 放进对象（值对象也能虚派发）；
  - 堆所有权：`new` 返回指针/引用？`delete` 还是智能指针？
  - 接口引用表示（`I&`/`I*`/fat pointer）与生命周期；
  - 智能指针依赖泛型（当前没有泛型）→ 可能需要内建 `shared T`/`Ref<T>` 或先做泛型。
- 语法澄清：C++ 中 `Cat& r = new Cat()` **不合法**（`new` 返回 `Cat*`）；要么 `Cat* p = new Cat(); delete p;`，
  要么 `Cat& r = *new Cat();`（容易漏 delete）。引用是**非拥有别名**；堆所有权必须另有机制。
- 主要风险：失去 RC 的自动生命周期/weak/跨线程共享；悬空引用/指针（无借用检查）；大对象拷贝成本；
  接口/继承的切片与生命周期；现有 class 测试与 SPEC 3.8–3.14 需大改。
- 备选（按改动从小到大）：
  - A. 保持现状：struct=值（栈/内嵌/C 布局），class=RC 引用；栈/底层用 struct。
  - B. **逃逸分析/栈提升**：语法不变（`Cat c = new Cat()`），对不逃逸对象在栈上分配并省掉 RC（Go 式）；
    不改语言模型，但分析复杂、跨函数受限。
  - C. 增加 **`value class`**：值语义 class（栈/内嵌、支持方法/继承/虚函数、无 RC），
     堆用 `new` 指针 + `delete`，共享用未来 `shared`/泛型智能指针；`class` 仍是 RC 引用。
  - D. 全面 C++ 化：class 值类型 + 引用/指针 + new/delete + 拷贝/移动；改动最大，等于重写对象模型。
- 建议：若目标只是"减少堆分配"，A + B 成本最低；若确实要 C++ 式语义，选 C 或 D，作为大版本规划，
  先定引用/拷贝控制/堆释放/接口表示/泛型（智能指针）规则。

#### U23.1 "同名 class，栈布局无 RC/vtable、堆布局有" 的可行性分析
- 结论：**不推荐**。同一名义类型有两种布局/语义，会产生无法在类型系统里表达的歧义：
  - 函数按值接收 `Cat`：传的是值还是句柄？值不能虚调用；句柄又不能指向栈值。
  - 赋值/参数/返回：复制语义取决于对象来源（栈/堆），编译器无法静态知道。
  - 字段偏移/`sizeof`/ABI：有无 RC/vtable 头导致布局不同，指针跨上下文不安全。
  - 接口/继承：栈值无 vtable，无法动态派发；拷贝会切片。
  - 数组/容器：`Cat[N]` 存值还是句柄？`Cat` 的两种布局无法统一。
  - `del`/析构：栈值自动析构、堆值 RC；同一表达式语义依上下文而变。
  - `auto`/泛型：类型不携带"值/引用"信息，无法约束。
- 标准做法是**类型层面区分**（而不是按分配位置）：
  - C++：`T`（值）vs `T*`/`T&`（引用/指针）；对象布局相同，多态靠指针/引用；
  - C#/Swift：`struct`（值）vs `class`（引用）；
  - Rust：`T`/`Box<T>` vs `&T`/`dyn Trait`。
- 建议的简化模型（与用户限制一致）：
  - `class` 保持**堆 + RC + vtable + 多态/接口**；
  - 值对象用 `struct`（或新增 `value class`）：**内联/栈、无对象头 RC、无 vtable、无继承/虚函数**，
    可有静态派发的方法；字段若是 class/interface 引用，由生成的拷贝/析构做逐字段 retain/release；
  - `struct` 继续承担 C 互操作（POD 布局）；若加 `value class`，则 struct 保持纯 POD。
  - 值对象可取地址（无 RC 风险）；class 句柄仍按 U21 禁止取址。
- 备选：不改语言模型，用**逃逸分析/栈提升**把不逃逸的 `new` 对象放栈上并省略 RC 操作（布局保持一致，保留多态）。

### U24. struct 允许 class/interface 引用字段（非 POD 值类型）
- 结论：**可行**，但 struct 从"POD 值"变成"非平凡值类型"，必须补齐拷贝/析构/取址/C 边界规则。
- 必须定义的语义：
  - **拷贝** `S b = a;`：逐字段拷贝；class/interface 字段 retain（RC+1）；
  - **赋值** `b = a;`：先 retain 新、再 release 旧（自赋值安全）；
  - **析构**：作用域结束/宿主对象销毁时，逐字段 release（数组字段逐元素）；
  - **传参/返回**：按值传会产生副本，副本的引用字段需要 retain/release（调用方/被调方边界要明确）；
  - **零初始化**：含引用字段的 struct 若未初始化，析构会 release 垃圾 → 必须自动清零或要求 `= 0`。
- 推荐的第一版限制：
  - 允许字段：`class`/`interface` 引用、以及它们的静态数组；暂不允许 `weak` 字段（weak 仅 class 字段）；
  - 暂不允许"拥有型 `string`/受管 slice（owner RC）"作为 struct 字段（等其拷贝/析构规则确定；borrowed 视图可以但生命周期自负）；
  - struct 仍**没有用户构造器/析构器/方法**（编译器生成隐式拷贝/析构）；以后需要再加；
  - **禁止对含引用字段的 struct 取地址**（`&s`、`&s.ref_field`、`&arr[i]`），
    否则可通过裸指针写引用字段绕过 retain/release；纯值 struct 仍可 `&`；
  - **extern C 边界**：递归检查，含 class/interface 的 struct 不能作为 C 参数/返回值
    （C 不参与 RC；布局相同也不代表所有权安全）；
  - 数组/容器：`S[N]` 元素逐元素构造/拷贝/析构（需要新的清理路径）。
- C 互操作：把"可传给 C"定义成**递归 POD 属性**（所有字段都是标量/指针/枚举/POD struct/数组），
  而不是由 `struct` 关键字保证；非 POD struct 只能用于 mu-lan 内部。
- 实现影响：
  - sema：放宽 struct 字段的引用检查；新增"是否含引用"的递归查询；extern 签名递归拒绝；
  - codegen：struct 拷贝/赋值/析构/传参/返回的逐字段 RC；数组元素清理；零初始化；
  - 取址规则扩展（U21）：含引用 struct 不可取址；
  - 测试：拷贝 retain、赋值 release、自赋值、数组、class 字段、传参/返回、零初始化、extern 拒绝。
- 备选：若不想让 struct 变复杂，保留 struct POD，另加 `value class` 承载"值 + 引用字段"；两者语义等价，
  只是命名/关键字不同。

### U25. 用现有设施实现 `struct string`：缺口清单
- 若只是 **C 风格手工管理**（`struct Str { byte* data; usize n; usize cap; }` + 自由函数 + `alloc/realloc/free`）：
  **现在几乎可以**，但缺少/需注意（已实测）：
  - 名字冲突：内置 `string` 是关键字，不能 `struct string`；需叫 `Str`/`StringBuf`，或改名内置类型；
  - **关键字不能做字段名**：`len`/`free`/`alloc` 等是关键字，`int len;` 解析报错；用 `n`/`length`；
  - 无 `->`/指针自动解引用：`p.n` 报"类型 S* 没有成员"，必须 `(*p).n`；
  - 无 struct 方法/构造/析构：API 只能是自由函数（`str_init(&s)`、`str_free(&s)`）；
  - 无自动清理：忘了 `str_free` 就泄漏；无 RAII；
  - **按值复制 = 双 owner**：编译器逐字段拷贝，`Str b = a` 后两边都 free 会双重释放；只能约定"不要复制、用 `str_clone`"，编译器不阻止；
  - 无 `new byte[n]`：用 `alloc/realloc/free`；
  - 内置 `string` ↔ `Str` 无自动转换：需库函数逐字节拷贝/构造；
  - 无 `+=`/`+` 复合/二元重载派发（struct 复合赋值不支持），append 只能函数调用；
  - 无边界检查/UTF-8 校验，靠库。
- 若要 **RAII / 值语义 / 动态缓冲**（U18/U20/U22 的设计），还缺：
  1. struct 方法 / `this`（或 `ref` 参数/返回）；
  2. struct 用户构造器/析构器 + 拷贝/移动控制（rule of 3/5）；
  3. struct 允许 class/interface 字段 + 编译器生成逐字段 retain/release（U24）；
  4. 受管动态缓冲/切片（owner RC）或 `new byte[n]`（U20/U22）；
  5. 引用/`ref` 参数与返回（U8），用于无拷贝传参与子串视图；
  6. 子串/切片语法、`clone`、`slice_from_raw`、从内置 `string` 构造；
  7. 只读视图/const 方法（避免每次读都复制）；
  8. 分配策略（allocator/arena）与失败处理；
  9. C 互操作规则（struct 含引用不能过 extern；NUL 结尾策略）。
- 建议：如果目标是"字符串库"，继续用**内置 `string` + `StringBuilder` class + 自由函数**；
  若要把 `string` 变成用户定义的值类型，则需要上面 1–6，属于语言层改造（大版本）。

### U26. 用语言设施支持"完整的 string 类型"（依赖顺序）
- 目标模型三选一：
  - **A. 不可变值 string（struct）**：`Str { Buf b; usize off; usize n; }`，Buf 是 RC class；
    操作返回新 Str；无用户拷贝构造（字段级 RC 自动共享）；需要 struct 方法/构造/析构 + U24 + Buf。
  - **B. 可变值 string（struct）**：拥有缓冲、可变方法；必须有深拷贝或 COW；
    需要 **引用/ref**、拷贝构造/赋值、移动（或 COW 的 refcount 查询）——设施最多。
  - **C. 可变引用 string（class）**：`class String { byte* data; usize n; usize cap; ~String(){free} }`；
    class 已有方法/构造/析构/RC，复制=共享句柄，`clone()` 深拷贝；不需要 struct 新设施，最快能做到"完整"。
- 语言设施依赖顺序（从底层到字符串 API）：
  1. **struct 方法 + `this`**（指针接收者；可变方法只允许 lvalue 接收者；只读方法/const 后续）；
  2. **struct 构造器**（默认/重载；`S s;` 调用默认构造；`S s = S(args)`；与聚合初始化的关系）；
  3. **struct 析构器 + 自动调用**（作用域/return/break/continue/数组元素/字段/临时值；现有 CleanupScope 扩展）；
  4. **所有权与拷贝控制**：
     - A：U24（struct 含 class 字段 + 逐字段 retain/release）即可，无需用户拷贝构造；
     - B：U8 引用 + 拷贝构造/赋值/移动（特殊成员）或 COW refcount 查询；
  5. **动态缓冲**：`class Buf`（byte* + cap，alloc/realloc/free，class dtor 释放）或 U20/U22 受管 slice；
  6. **拼接/视图**：`+`/`==` free operator（已有 struct 运算符）；`[]`/`[]=`；`+=` 需要 struct 复合赋值派发；
     子串/视图需要 ref 返回或复制；
  7. **字符串 API**：从内置 `string`/`readonly char*` 构造、c_str、len/cap/empty、append/insert/replace/trim/find/split、
     UTF-8 校验/码点迭代、格式化；
  8. **迭代协议**：`for c in s` 目前只支持内置 string/数组/切片；用户 Str 需要索引循环或迭代器协议；
  9. **关键字冲突**：字段不能叫 `len`/`free` 等（`len` 已实测报错）；API 命名需绕开或允许关键字做字段名。
- 关键陷阱：
  - 有析构器但无拷贝构造 → 值拷贝会双 owner/双重释放；有拷贝构造但无移动 → 返回/传参大量深拷贝；
  - 拷贝构造签名需要引用（`Str(const Str&)`），没有引用就只能用指针/显式 clone，语法不自然；
  - 构造失败/早退时的析构与部分构造（无异常时也要规则）；返回时 RVO/move。
- 建议：先用 **C（class String）** 把"完整功能"跑通（改动最小），再按 A 做不可变值版本（U24），
  B（可变值）留到引用/拷贝控制就绪；不要一开始同时上 struct 方法 + 拷贝控制 + COW。

## 已完成（留档）

- 0.0.20：`char`=i8（C 风格）、`byte`=u8 且为唯一无符号 8 位名（删除 `ubyte`/`uchar` 别名）；
  `s[i]`/字符串 for-in 返回 `byte`；字符字面量默认 `byte` 且按上下文适配 `char`/`byte`。
- 0.0.19：`char` 改为有符号 8 位（C 风格）；`byte` 改为无符号 8 位；`'\xNN'` 按位模式解释。
- 0.0.14：`weak` 目标销毁自动置 null（全局注册表 + 自旋锁）。
- 0.0.15：`readonly` class 视图 / `ref` 参数 / interface 向下转换正式列为第一阶段不支持（SPEC 表 98–100）。
- 0.0.6：`as` 向下转换（class→派生、interface→class）沿 desc 基类链运行期检查。
