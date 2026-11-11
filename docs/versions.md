# 版本记录

## 规则

- 版本格式：`主版本号.次版本号.build号`，从 **0.0.1** 开始。
- **每次正式构建 + 全量测试通过后，build 号 +1**；由维护者记录在本文件。
- **主版本号 / 次版本号只在明确要求时提高**，平时保持不变。
- 版本号的唯一来源是仓库根的 `VERSION` 文件；CMake、编译器 `--version`、生成的 C 代码都从它读取。

## 记录

| 版本 | 日期 | 内容摘要 | 测试结果 |
|---|---|---|---|
| 0.0.1 | 2026-11-08 | 基线：C++20 编译器（lexer/parser/sema）、C99 后端、LLVM 后端（LLVM C++ API → COFF）、58 个回归用例、引用/对象模型设计讨论 | C 后端 58/58；LLVM 后端 58/58；冷编译零 warning |
| 0.0.2 | 2026-11-08 | 移除 C99 后端，LLVM 成为唯一后端；清理 `--emit-c`/`-DMULANC_WITH_LLVM`；版本机制接入 `--version` 与 LLVM 模块标志 | LLVM 后端 58/58；冷编译零 warning |
| 0.0.3 | 2026-11-09 | 对象模型端到端：class/接口/继承/构造析构/虚派发/引用计数/`weak` 的语义与 LLVM 代码生成；重载与运算符派发；新增 4 个端到端 class 用例、21 个报错用例、4 个 check 用例 | 103/103（24 成功 + 75 报错 + 4 check） |
| 0.0.4 | 2026-11-09 | 局部引用清理（块/return/break/continue）、临时引用释放、接口默认方法经类变量派发、接口数组布局修正（16 字节）、`del` 参数/循环变量限制；新增 4 个端到端用例 | 107/107（26 成功 + 77 报错 + 4 check） |
| 0.0.5 | 2026-11-09 | `new`/`len`/`sizeof`/一元表达式后可接后缀（`new C().f()`、`len(s) as int`）；`sizeof` 支持 class/interface/enum；禁止 class 指针、`del` 运算符下标、析构器 `return`；相关引用类型间身份比较；新增 4 个端到端用例与 3 个报错用例 | 115/115（31 成功 + 80 报错 + 4 check） |
| 0.0.6 | 2026-11-09 | `as` 向下转换：class→派生 class、interface→class 沿 desc 基类链运行期检查，失败得 null；desc 增加 base_desc；新增 downcast 端到端用例 | 116/116（32 成功 + 80 报错 + 4 check） |
| 0.0.7 | 2026-11-09 | 构造器省略 `super(...)` 时隐式调用基类无参构造器（C++ 语义）；新增链表（strong next + weak prev）与嵌套 `new`/基类数组用例 | 119/119（35 成功 + 80 报错 + 4 check） |
| 0.0.8 | 2026-11-09 | 收紧 `as`：指针/切片不再互相转换，切片转换要求元素类型相同；新增 2 个报错用例 | 121/121（35 成功 + 82 报错 + 4 check） |
| 0.0.9 | 2026-11-09 | 修复运算符/下标重载返回引用被当作借用导致的泄漏；override 返回类型与所有基类/接口声明比对（冲突报错）；新增 2 个用例 | 123/123（36 成功 + 83 报错 + 4 check） |
| 0.0.10 | 2026-11-09 | 引用数组的逐元素 RC：拷贝 retain、覆盖/离开作用域 release、按值返回与临时实参逐元素转移（修复共享数组提前释放/泄漏）；新增 2 个端到端用例 | 125/125（38 成功 + 83 报错 + 4 check） |
| 0.0.11 | 2026-11-09 | class 字段解析改为按基类依赖的拓扑顺序（同一 unit 内基类可后声明且字段布局正确）；新增前向基类、接口数组用例 | 127/127（40 成功 + 83 报错 + 4 check） |
| 0.0.12 | 2026-11-09 | `super.method` 只解析 class 链（接口默认方法经 super 调用报错）；`del null` 空操作；多 unit 接口/继承 check 用例（lib_ 依赖跳过）；下标复合赋值报错用例 | 131/131（41 成功 + 85 报错 + 5 check） |
| 0.0.13 | 2026-11-09 | 清除冷编译最后一个 warning（unused variable）；冷编译零 warning | 131/131（41 成功 + 85 报错 + 5 check） |
| 0.0.14 | 2026-11-09 | **weak 目标销毁自动置 null**：全局 weak 注册表（目标, 字段地址）+ 自旋锁，赋值登记、覆盖/持有者析构注销、目标 destroy 时清零；顺带修复二元比较重复求值（函数调用在比较中执行两次、临时引用泄漏） | 132/132（42 成功 + 85 报错 + 5 check） |
| 0.0.15 | 2026-11-09 | SPEC 正式降级：`readonly` class 视图、`ref` 参数、interface→派生接口向下转换列为第一阶段不支持（表 98–100）；新增 `extern` 不可重载/不可与普通函数同名、`main` 不可重载检查 | 135/135（42 成功 + 88 报错 + 5 check） |
| 0.0.16 | 2026-11-09 | 新增比较/下标单次求值与临时释放回归用例（验证 0.0.14 的双重求值修复） | 136/136（43 成功 + 88 报错 + 5 check） |
| 0.0.17 | 2026-11-09 | 外链 C 禁传 class/interface 检查；新增接口返回接口、综合大用例（接口数组/for-in/向下转换/弱引用） | 139/139（45 成功 + 89 报错 + 5 check） |
| 0.0.18 | 2026-11-09 | 明确字符字面量语义：`char` 为无符号 8 位单字节，非 ASCII 字面量编译报错；SPEC 补第 101 条；新增 `char_bytes`（UTF-8 字节视图）与 `char_nonascii` 用例 | 141/141（46 成功 + 90 报错 + 5 check） |
| 0.0.19 | 2026-11-09 | **`char` 改为有符号 8 位（C 风格）；`byte` 改为无符号 8 位，`ubyte`/`uchar` 为其别名（同一类型）**；`'\xNN'` 按位模式解释；新增 char/byte 范围报错用例 | 143/143（46 成功 + 92 报错 + 5 check） |
| 0.0.20 | 2026-11-09 | 按定稿收敛 8 位类型：`char`=i8（C 风格）、`byte`=u8 且为唯一无符号 8 位名（**删除 `ubyte`/`uchar` 别名**）；`s[i]`/字符串 for-in 返回 `byte`；字符字面量默认 `byte` 并按上下文适配 `char`/`byte`；新增别名移除用例 | 144/144（46 成功 + 93 报错 + 5 check） |
| 0.0.21 | 2026-11-09 | **struct 方法**：静态派发、隐式 `this`、方法重载、数组/字段接收者、隐式方法调用；方法不得与字段同名；显式 `this` 暂不支持；新增 1 个成功用例 + 4 个报错用例 | 149/149（47 成功 + 97 报错 + 5 check） |
| 0.0.22 | 2026-11-09 | **struct 构造/析构**：构造器重载、`S s;` 默认构造、`S s = S(args)` 就地构造；析构器在作用域/return/break/continue 自动调用；含析构器的 struct 暂禁按值拷贝/传参/返回/数组/字段/全局；新增 1 个成功用例 + 11 个报错用例 | 161/161（48 成功 + 108 报错 + 5 check） |
| 0.0.23 | 2026-11-09 | **U24：struct 引用字段 + 逐字段 RC**：struct 可含 class/interface 字段（含数组/嵌套）；拷贝/传参 retain、赋值先 retain 新再 release 旧、析构逐字段/逐元素 release；返回值借用转 owned；调用临时 struct 语句结束释放；未初始化本地 struct 自动清零；extern/全局递归拒绝；含用户析构器的 struct 仍禁拷贝；新增 `struct_rc_fields` 成功用例 + 2 个报错用例，移除过时的“struct 不能含 class 字段”用例 | 163/163（49 成功 + 109 报错 + 5 check） |
| 0.0.24 | 2026-11-09 | **动态数组 `T[]` + 多维数组**：`new T[n]` 创建带 RC 头的堆数组（句柄 8 字节，O(1) 复制共享）；静态矩形 `T[m,n]`（嵌套定长数组、`a[i,j]`/`a[i][j]`）、动态矩形 `T[,]` + `new T[m,n]`、锯齿 `T[][]` + `new T[m][]`（一次一维，循环填行）；`len(a)`/`len(a,k)`（静态 axis 字面量、动态可运行期）；`T[N]→T[]`、`T[m,n]→T[,]` 分配复制；元素支持 class/interface 与含引用 struct，逐元素 release；null 句柄 len=0；extern 递归拒绝 `T[]`；含析构器 struct 仍禁作元素；移除 class/interface 切片禁用与旧借用视图语义 | 171/171（51 成功 + 115 报错 + 5 check） |
| 0.0.25 | 2026-11-09 | **字符串改为 std.string.string struct，移除内建 `string`**：`libs/std/string.mu`（unit `std.string`）中的 struct `string { byte* p; usize n; byte[] owner; }` + 字符串库；字面量类型为 string（owner=null，rodata，零分配/零 RC）；`s[i]`/`len`/`==`/for-in 按 canonical 布局内置降级；复制/字段/返回走 U24 逐字段 RC；std.string 提供 `str_from_cstr`/`str_cstr`/`str_sub`/`str_concat`/`operator+` 与方法；移除 lexer/parser/sema 的 `string` 关键字，迁移全部测试与示例；新增 `str_struct` 用例；生成 IR 后运行 LLVM `-O2` 标准流水线 | 172/172（52 成功 + 115 报错 + 5 check） |
| 0.0.26 | 2026-11-09 | **标准库目录与类型改名**：新增 `libs` 默认库搜索路径（入口目录、`-I`、`MULANC_LIBS`、当前目录及祖先的 `libs`/`libs/std`）；`libs/std/string.mu`（unit `std.string`）替代编译器内嵌 prelude；`unit`/`use` 支持点号名（`std.string` → `libs/std/string.mu`）；字符串结构体由 `Str` 改名为 `string`；测试/示例/文档同步 | 172/172（52 成功 + 115 报错 + 5 check） |
| 0.0.27 | 2026-11-10 | **可见性机制**：新增关键字 `pub`；顶层声明/class 字段与方法/构造器默认仅 unit 内可见，`pub` 跨 unit；`pub unit` 跨库导出；库边界 = unit 名第一段（跨库 `use` 需 `pub unit`）；`pub` 签名连锁检查；非 `pub` 符号 LLVM `internal` linkage；迁移 std.string/math/lib_* 并新增 11 个可见性用例 | 184/184（52 成功 + 125 报错 + 7 check） |
| 0.0.28 | 2026-11-10 | **多段 unit 名限定访问**：表达式 `lib.a.b.f()` 按最长已导入 unit 前缀折叠为单个 Name；类型 `lib.a.b.T` 在 sema 按最长前缀匹配；parser 的类型解析支持任意段点号；`std.string` 视为隐式已 use；新增 2 个 check 用例 | 186/186（52 成功 + 125 报错 + 9 check） |
| 0.0.29 | 2026-11-10 | **C# 式导入可见性**：`use` 后 `pub` 符号可直接无前缀使用（类型/函数/全局变量/枚举常量/struct 构造）；本地声明遮蔽导入；多来源同名在使用点报歧义并要求限定名；函数重载跨 unit 合并；parser 用声明形状试探支持无前缀类型名；新增 6 个导入用例 | 191/191（52 成功 + 128 报错 + 11 check） |
| 0.0.30 | 2026-11-10 | **public/private 取代 pub**：private=unit 内、public=跨 unit；struct 成员默认 public、class 成员默认 private；struct/class 支持 public:/private: 分段标签与逐成员前缀；struct 字段可 private，跨 unit 聚合初始化要求字段全 public；全仓库迁移并新增 4 个用例 | 194/194（52 成功 + 129 报错 + 13 check） |
| 0.0.31 | 2026-11-10 | **string 对象化重写**：`string` 保留值类型 struct（canonical 布局/字面量/`s[i]`/`len`/`==`/for-in 降级不变），字段 private；新增构造器 `string(readonly char*)` 与成员方法 `length/empty/is_empty/at/cstr/sub/concat/find/starts_with/ends_with/trim`；删除 `str_*` 自由函数，`operator+` 薄封装 `concat`；放宽 `= 0` 允许有构造器的 struct 零值初始化；新增 string_methods 端到端用例，删除 struct_ctor_zero | 194/194（53 成功 + 128 报错 + 13 check） |
| 0.0.32 | 2026-11-10 | **移除 `static`**：unit 级变量直接声明（默认 private，`public` 跨 unit）；`extern` 保留；全局变量 linkage 由 public/private 决定；顶层/函数内使用 `static` 给出明确迁移错误；迁移全部用例与示例，删除 unit_var_bare | 193/193（53 成功 + 127 报错 + 13 check） |
| 0.0.33 | 2026-11-10 | **const 改为 C 模型**：删除 `readonly`；`const` 成为类型限定符（前缀修饰基类型，`const int**` 表示最内层只读）；`T* → const T*` 隐式、反向需显式 `as`；写检查与隐式转换按 const 层判断；全仓库迁移 `readonly` → `const`；暂不支持 `T* const` | 193/193（53 成功 + 127 报错 + 13 check） |
| 0.0.34 | 2026-11-10 | **明确不提供 `T* const`**：`const` 只作基类型前缀（值/pointee 只读），不引入指针绑定只读与逐层 const；SPEC 3.7、#25 改为「不提供」并说明理由 | 193/193（53 成功 + 127 报错 + 13 check） |
| 0.0.35 | 2026-11-10 | **++/-- 语句支持**：前缀/后缀 `++`/`--` 只能作为独立语句（无返回值，不能出现在表达式里）；语义降级为 `x += 1` / `x -= 1`；`i++; j++;` 合法；新增 inc 端到端与 inc_expr 报错用例 | 195/195（54 成功 + 128 报错 + 13 check） |
| 0.0.36 | 2026-11-10 | **禁止 `*(p++)`**：`++`/`--` 无返回值，括号内出现也报明确错误；`*p++` 仍解析为 `(*p)++`；新增 inc_paren_expr 报错用例并在 inc 中覆盖 `*p++` | 196/196（54 成功 + 129 报错 + 13 check） |
| 0.0.37 | 2026-11-10 | **++/-- 表达式上下文统一报错**：函数实参、if/while 条件、return、初始化器、数组下标、new 参数、括号内都给出「没有返回值」的明确提示；新增 inc_call_expr / inc_return_expr 报错用例 | 198/198（54 成功 + 131 报错 + 13 check） |
| 0.0.38 | 2026-11-10 | **位取反改为 `~`**：与 C 一致，支持 `operator~` 重载（1 个参数）；`!` 不再使用并给出迁移提示；迁移 bitwise 用例，新增 op_neg 端到端与 bang_removed 报错用例 | 200/200（55 成功 + 132 报错 + 13 check） |
| 0.0.39 | 2026-11-10 | **枚举成为名义类型**：与整数双向无隐式转换（都要 `as`）；同枚举支持 `== != < <= > >=`（序 = 底层整数值）；跨枚举比较、枚举算术/位运算/一元 `-+~`/复合赋值、下标/区间端点用枚举一律报错；`usual_arith` 拒绝枚举，codegen 比较按 int 有符号序生成 `icmp`；新增 enum_cmp 端到端与 8 个报错用例，迁移 extras/enumfield/vis_import_ok | 209/209（56 成功 + 140 报错 + 13 check） |
| 0.0.40 | 2026-11-10 | **枚举成员限定引用**：`Color.Red` 与 `Red` 等价，跨 unit 可写 `unit.Color.Red`；sema 在成员访问处先解析类型名（本 unit → use 的 public → std.string），成员是常量（不可赋值/取地址），非枚举类型写 `T.m` 报错；codegen 把该节点按 `enum_const` 直接折叠为常量；enum_cmp 与 vis_import_ok 覆盖限定引用，新增 4 个报错用例 | 213/213（56 成功 + 144 报错 + 13 check） |
| 0.0.41 | 2026-11-10 | **空花括号 `{}` 零初始化**：声明处聚合类型（struct / 定长数组 / 切片 / `string`）用 `{}` 表示零值，聚合 `= 0` 报错并提示改用 `{}`；标量（数值/bool/char/指针/class 引用）写 `{}` 报错，改用各自常量（`0`/`0.0`/`false`/`null`）；codegen 空聚合走 `zeroinitializer` / 清零，全局空聚合用零常量；新增 empty_brace 成功用例与 brace_scalar / zero_aggregate 报错用例 | 216/216（57 成功 + 146 报错 + 13 check） |
| 0.0.42 | 2026-11-10 | **布局修饰符 `packed` / `align(N)`**：`packed` 无参数（= `pack(1)`，无内部/尾部填充，`alignof` 1）、`align(N)`（2 的幂常量，覆盖整体对齐、`sizeof` 补齐到 N 的倍数、不改变字段偏移、`N < 自然对齐` 报错），二者正交可组合；布局单一真源 `type_align`/`type_size`（sema 与后端共用）；packed 用 LLVM packed struct（普通 struct 的 ABI 不变），`align(N)` 插显式填充并给 alloca/全局/成员访问设置对齐（packed 链一律 `align 1`）；class 对象改用同源「模型布局 + 填充」（对象对齐 = max(16, 字段最大对齐)，>16 用 `_aligned_malloc`）；切片分配点拒绝元素 `align > 8`（数据区在 24 字节头之后）；新增 layout / layout2 成功用例与 9 个报错用例 | 227/227（59 成功 + 155 报错 + 13 check）；冷编译零 warning |
| 0.0.43 | 2026-11-10 | **参数声明不得共享类型（SPEC 表 26 定案）**：每个参数必须单独写 `类型 名称`，`int f(int a, b)` 为编译错误；解析器在 `parse_params` 中提前识别 C 式共享类型写法（`Ident` 紧跟 `,`/`)`），给出「每个参数单独声明类型」的明确诊断（此前只在 `)` 处报含糊的「期望参数名」）；类型后缀只作用于单个参数；变量/字段声明的多名字共享类型不变（SPEC 4.1 加交叉引用）；新增 param_shared_type 报错用例 | 228/228（59 成功 + 156 报错 + 13 check） |

| 0.0.44 | 2026-11-10 | **显式 `this`**：class/struct 方法、构造器、析构器内可显式写 `this`；`this` 是当前对象的引用（struct 为对象本身的左值别名，非指针），支持 `this.field` 读写、`this.method()`、作为实参传递与按值返回；`this` 不可赋值、不可取地址（`&this` 报错）、不能 `del`；新增 explicit_this 成功用例与 this_addr / this_outside 报错用例，删除 struct_this 报错用例 | 230/230（60 成功 + 157 报错 + 13 check） |

| 0.0.45 | 2026-11-10 | **`switch`（无 fallthrough）**：`switch expr { 模式列表: { } }`，括号可选；分支体强制花括号、各自独立作用域；`else` 至多一个且必须最后；模式为编译期常量（整数字面量 / 字符字面量 / 带一元 `-` `+` 的字面量 / 枚举成员），`a, b` 多值任一命中；区间 `a..b` 半开且必须非空；枚举必须穷尽（缺成员报错；`else` 与之互斥，全覆盖时 warning）；枚举不支持区间与整数隐式转换；模式重复/重叠（含区间相交）编译错误；被匹配值只求值一次；`break`/`continue` 仍归最近循环；codegen 密集值走 LLVM `switch`（跳表）、区间走比较链；新增 switch_basic 成功用例与 15 个报错用例；SPEC 新增 6.6，`match` 取消保留字 | 246/246（61 成功 + 172 报错 + 13 check） |
| 0.0.46 | 2026-11-10 | **条件表达式 `?:`**：`cond ? a : b`，右结合、优先级低于 `or`；条件必须 `bool`；只求值被选中的分支；两侧类型按「同一类型 → 隐式加宽 → `null` 适配」收敛，都不成立要求 `as`；结果为 prvalue（struct/string/定长数组逐字段 retain，class/interface 借用分支 +1、`new`/调用结果转移所有权，动态数组按句柄语义），结果槽在 entry 块物化；结果不是左值（`(c ? a : b) = x`、`&(c ? a : b)` 报错），不参与编译期常量；两侧皆 `void` 时只能作表达式语句；顺带修复 `double d = 1;`（整数字面量适配浮点类型）在后端 `cast<IntegerType>` 断言崩溃；新增 ternary 成功用例与 5 个报错用例；SPEC 新增 5.2，#14/#104 定案 | 252/252（62 成功 + 177 报错 + 13 check） |
| 0.0.47 | 2026-11-10 | **参数改为拥有型局部变量 + 移除 `del`**：函数/方法/构造器/接口方法参数在进入时 retain、退出时 release（覆盖 return/break/continue 路径），函数体内可自由重绑定参数；`for-in` 值绑定同样按拥有型局部变量处理（每轮 retain/release，覆盖 break/continue）；由此修复「`c = null` / `c = new C()` 赋给参数或循环变量会把调用方对象提前释放（use-after-free）或泄漏」的内存安全缺陷；`del` 语句删除（`del x;` ≡ `x = null;`，无独立语义），保留关键字仅用于迁移诊断；迁移全部测试/示例中的 `del`，删除 6 个 del 专用报错用例与 class_del_null，新增 del_removed 报错用例与 param_ownership 回归用例；SPEC 3.8/4/8.3 与决策表 #69/#70 同步 | 247/247（62 成功 + 172 报错 + 13 check） |
| 0.0.48 | 2026-11-10 | **for-in 索引形态 + 用户类型迭代 + U21 落地 + 左值判定修复**：①`for i, x in seq` / `for i, ref x`（`i` 为 usize、循环内只读；内置序列的 `i` 是真实下标，区间是相对 `lo` 的偏移）；用户类型（struct/class）提供 `length() -> usize` + `operator[](C, usize) -> T`（按 usize 精确匹配）即走索引形态降级，`length()` 只求值一次，元素按拥有型局部变量逐轮 retain/release；多维数组 `T[m,n]`/`T[,]` 明确禁用 for-in（改用下标循环 + `len(a,k)`）；`for ref` 不支持用户类型；SPEC 新增 6.4.1–6.4.4 与 backlog U30。②**U21**：禁止对 class/interface 引用取地址（`&c`、`&i`）以及经引用访问的成员/元素取地址（`&c.x`、`&h.items[0]`、`&catArray[i]`、`&h.v[0]`），纯值 struct / 定长数组元素 / 指针变量取址不受影响；layout2 改用同源模型 struct 镜像验证 class 布局。③修复 `is_lvalue` 把与 unit 同名的局部/全局变量误判为非左值（`unit t; S t = ...; t.x = 1;` 报「赋值左侧必须是可赋值的对象」）。新增 forin_index / unit_name_var / addr_rules 成功用例与 12 个报错用例 | 262/262（65 成功 + 184 报错 + 13 check） |
| 0.0.49 | 2026-11-10 | **U24：禁止含引用字段的 struct 取址**：`&s`、`&arr[i]`（元素为含引用 struct）编译报错——裸指针可绕过逐字段 retain/release；标量字段 `&s.x` 仍允许；含用户析构器的 struct 例外（本来就只能通过指针使用）。`dynarray` 用例的 `void keep(S* out, ...)` 改为按值返回；新增 3 个取址报错用例，`addr_rules` 覆盖允许形式（RC struct 的标量字段、析构器 struct 取址） | 265/265（65 成功 + 187 报错 + 13 check） |
| 0.0.50 | 2026-11-10 | **编译器改名 `mulanc` → `muc`**：可执行目标与 CMake `project()` 名改为 `muc`（产物 `build/muc.exe`），`--version` / `--help` 输出同步；环境变量 `MULANC_CMD`/`MULANC_LIBS`/`MULANC_BACKEND`/`MULANC_LLVM_TRACE` 改为 `MUC_CMD`/`MUC_LIBS`/`MUC_BACKEND`/`MUC_LLVM_TRACE`，编译宏 `MULANC_VERSION` → `MUC_VERSION`；测试运行器默认编译器改为 `muc`；README / SPEC / docs / AGENTS 中的命令示例同步（`versions.md` 历史条目保留旧名）。另：新增 `LICENSE`（Apache-2.0）与 `NOTICE`，语言定名 Mulan（木兰），17 个源文件加 SPDX 头；示例与测试中的 `"mu-lan"` 字面量改为 `"Mulan"`（strings 用例派生的长度 / 截断期望值同步改为 5 / `Mu`） | 265/265（65 成功 + 187 报错 + 13 check） |

| 0.0.51 | 2026-11-10 | **修复 Windows 控制台中文乱码**：`muc` 启动时若控制台输出代码页不是 UTF-8（简体中文系统默认 936/GBK），临时切到 `CP_UTF8` 并用 `atexit` 在退出时恢复原值。源码中的中文消息一直是 UTF-8 字节，此前被 936 控制台按 GBK 解释而显示为乱码；输出重定向到文件或管道时字节不变（仍是 UTF-8） | 265/265（65 成功 + 187 报错 + 13 check）；冷编译零 warning |

| 0.0.52 | 2026-11-10 | **诊断消息与 CLI 输出全部英文化**：`src/` 中 787 处字符串字面量（lexer/parser/sema/codegen/main/loader）改为英文，风格对齐 clang/rustc——小写开头、不加句号、`cannot`/`expected X but got Y`/`unknown`/`invalid` 等惯用表达，代码标识符仍用反引号；`--help` 重写为 `Usage:` / `build options:` / `Environment:` 三段式；代码注释保持中文不变。`tests/errors/*.err` 187 个期望文件同步为对应的英文消息片段（按原「第 N 条诊断」一一映射，`strings` 等成功用例不受影响） | 265/265（65 成功 + 187 报错 + 13 check）；冷编译零 warning |

| 0.0.53 | 2026-11-10 | **非法字符提示改用反引号**：`quote_char()` 输出由 `'x'` 改为 `` `x` ``，与其它诊断的代码片段风格统一；反斜杠 / 反引号本身 / 控制字符分别输出 `` `\` ``、`` `\`` ``、`` `\x0b` ``。新增 `invalid_char` 报错用例（此前该诊断无覆盖） | 266/266（65 成功 + 188 报错 + 13 check）；冷编译零 warning |

| 0.0.54 | 2026-11-10 | **联合体 `union`**：POD 值类型，所有成员偏移 0，`sizeof = max(成员)` 向上对齐到最大成员对齐；支持 `packed` / `align(N)`（与 struct 同规则）；成员限于 POD——禁止 `string`、动态数组 `T[]`、class/interface 引用、含引用字段或含用户析构器的 struct；无方法/构造器/析构器/继承，成员恒 public；初始化只有 `{}` 零初始化与具名 `{ member = value }`（恰好一个成员），不提供位置初始化与 `T(...)` 构造；读非活跃成员按 C 语义字节重解释。LLVM 侧用「对齐载体 + 尾部填充」表示、成员访问统一走偏移 0。新增 union_basic 成功用例与 11 个报错用例；SPEC 新增 3.4.1，backlog U14 关闭 | 278/278（66 成功 + 199 报错 + 13 check）；冷编译零 warning |

| 0.0.55 | 2026-11-11 | **C 可变参数调用（`extern` 的 `...`）+ `std.cstdio`**：`extern` 声明可带 `...`（必须最后、至少一个命名参数），实参按 C 默认提升（`float→double`、小于 `int` 的整型→`int`、枚举→底层整型），`string` 自动传底层 `const char*`；含 RC 的实参编译错误；Mulan 内不支持自定义 varargs。codegen 用 `FunctionType::isVarArg` + `promoteVararg()`，ABI 由 LLVM 处理。另：`extern` 声明现在隐式 `public`（此前跨 unit 调用会被判未公开）；常量初值支持一元 `+`/`-`（`const int EOF = -1;`）；新增 `libs/std/cstdio.mu`（stdio 函数与常量绑定）。新增 varargs / cstdio 成功用例与 5 个报错用例；SPEC 新增 7.1，backlog U31 | 285/285（68 成功 + 204 报错 + 13 check）；冷编译零 warning |

| 0.0.56 | 2026-11-11 | **带参数的 `main`**：支持 `int main(int argc, char** argv)`（C 启动代码传入，`argv` 以 null 结尾、`argv[argc] == null`）；参数类型必须精确为 `(int, char**)`，1 个参数 / `char*` / 非 `int` 返回均编译错误；`char**` 而非 `string[]`（argv 不参与 RC）。新增 main_args 成功用例与 main_bad_argv 报错用例，main_params 期望值更新；SPEC 7、§12 #15 同步 | 287/287（69 成功 + 205 报错 + 13 check）；冷编译零 warning |

| 0.0.57 | 2026-11-11 | **诊断与调试体验**：①诊断附带源码片段与插入符（`  |` + 行号 + `^`；tab 展开 4 列、CJK 按 2 列计算显示宽度）；②**多错误报告**——同一 unit 的全局/函数/class/struct/interface 检查逐项收集错误后统一输出（出错后重置作用域、循环深度与 auto 推断状态），单次上限 20 条；③`-g` / `--debug` 生成 **DWARF**（编译单元、函数 DISubprogram、语句级行号、局部变量与结构体成员），gdb 可断点、单步、`info locals`。新增 multi_errors（3 条错误一次报出）与 dbgvars 用例 | 289/289（70 成功 + 206 报错 + 13 check）；冷编译零 warning |

> 后续条目只递增 build 号，例如 0.0.2、0.0.3……；主/次版本变更会单独注明。
