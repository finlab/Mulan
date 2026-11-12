# 版本记录

## 规则

- 版本格式：`主版本号.次版本号.build号`，从 **0.0.1** 开始。
- **每次正式构建 + 全量测试通过后，build 号 +1**；由维护者记录在本文件。
- **主版本号 / 次版本号只在明确要求时提高**，平时保持不变。
- 版本号的唯一来源是仓库根的 `VERSION` 文件；CMake、编译器 `--version`、生成的 C 代码都从它读取。

## 记录

### 0.0.79 — 2026-11-12

**版本记录改为章节格式**：长条目在表格里容易被竖线撑破（转义不彻底就会错列），改为 `### 版本 — 日期` + 摘要 + `**测试**` 的结构；78 条记录按版本号倒序重排（最新在前）。无代码改动

**测试**：384/384（87 成功 + 281 报错 + 16 check）

### 0.0.78 — 2026-11-12

**文档整理收尾**：`versions.md` 收回 `docs/`（对外发布，读者需要变更记录）；修复 0.0.57 条目中未转义的竖线（诊断片段 `` `  |` `` 曾撑破表格列，导致后续列错位）。AGENTS 与 README/README.en 文档表同步。无代码改动

**测试**：384/384（87 成功 + 281 报错 + 16 check）

### 0.0.77 — 2026-11-12

**文档边界收紧**：`docs/` 只保留 `SPEC.md`（对外规范）；代码地图、后端说明、对象模型计划、跨平台方案、版本记录、可见性设计、std.string 布局全部移入 `agents/`（gitignore）。README 与 SPEC 不再链接未发布文件。无代码改动

**测试**：384/384

### 0.0.76 — 2026-11-12

**文档目录整理（文档）**：`docs/` 定为对外文档目录，`SPEC.md` 移入 `docs/SPEC.md`；新增 `agents/`（**gitignore，不入库**）存放本地 AI 协作产物——`design-backlog.md`、`design-review.md`、`TODO.md`。同步更新 README / README.en / AGENTS / SECURITY 与文档间相对链接（含 SPEC 内的 `examples/`、`visibility-design` 路径），README 版本号刷新至 0.0.75。`docs/design-review.md` 不再对外发布。无代码改动

**测试**：384/384（87 成功 + 281 报错 + 16 check）

### 0.0.75 — 2026-11-12

**明确不做「值类型作接口值」（文档决策）**：经评估，无借用检查时值类型→接口值必然装箱，成本≈`new` 一个 class，且丧失显式生命周期；运行时多态由 `class` 覆盖，异构值集合用和类型。关闭 backlog U33 S3/S5 与 U34 M5b；SPEC 新增 3.15.8，3.15.7 与表 110 改为「明确不做」并写明理由与「借用检查落地后可重评」的条件。无代码改动

**测试**：384/384（87 成功 + 281 报错 + 16 check）

### 0.0.74 — 2026-11-12

**U33 S1+S2：`struct S : I` 静态满足**：`:` 后只能列接口（class/struct 报错）；接口方法（含继承链与默认方法）必须显式实现并写 `override`，缺失/漏 `override` 各有明确诊断。`struct` 方法支持前置 `override`（此前只有 `class` 支持），`MethodSig::is_override` 如实传递。静态满足不产生值类型、不生成 vtable，codegen 零改动。新增 `struct_iface` 端到端 + 2 个报错用例

**测试**：384/384（87 成功 + 281 报错 + 16 check）

### 0.0.73 — 2026-11-12

**SPEC 泛型章节（文档）**：新增 **3.15 泛型**（单态化策略、声明/使用语法、实例化规则、约束的类型集合 + 成员集模型、`&`/`|` 对偶、结构化满足与精确匹配、`Self`、当前限制）；同步更新 0 速览、2.3（`Self` 上下文关键字）、3.10（泛型接口只作约束）、7（函数支持泛型）、10（移出非目标）、12 表 95/110/111、14 实现路线。文档与 0.0.64–0.0.72 的实现对齐

**测试**：381/381（86 成功 + 279 报错 + 16 check）

### 0.0.72 — 2026-11-12

**泛型接口作约束（M5a）**：`instantiate_generic` 支持 `InterfaceDecl`（`make_interface`、成员签名含 `is_operator/ret_self/params_self`、`Self` 解析、默认方法体检查），`T: ISeq<int>` 通过结构化形状匹配生效；接口实例不产生值类型/itable。`interface ISeq<T>` + `sum_all<T: ISeq<int>>(T)` 端到端可用。限制：泛型接口继承与「接口作值类型」仍待 M5b。新增 `generic_iface` 用例

**测试**：381/381（86 成功 + 279 报错 + 16 check）

### 0.0.71 — 2026-11-12

**泛型继承（M3f）**：`class Derived<T> : Base<T>`。`bases` 旁新增 `base_specs`（`vector<TypeSpecPtr>`，与字符串列表平行），parser 同时填充、克隆/替换时一并处理；`instantiate_generic` 的 class 分支支持单个基类（实例化后设 `ty->base` 并扁平化字段），构造器 `super(...)`、方法继承、vtable/desc 全部沿用普通 class 路径。仍不支持泛型接口与 `const` 成员。新增 `generic_inherit` 用例

**测试**：380/380（85 成功 + 279 报错 + 16 check）

### 0.0.70 — 2026-11-12

**泛型 M3e：泛型 class 实例化**：`instantiate_generic` 统一支持 struct/class（字段按 `field_visibility`、方法/构造器/析构器签名、`is_abstract`、vtable/desc 沿用普通 class 路径）；`check_class` 接入（上下文保存/恢复）。`class Box<T>`、`Counter<T>`（含默认构造器）端到端可用。限制：泛型 class 暂不支持基类、接口与 `const` 成员。新增 `generic_class` 用例

**测试**：379/379（84 成功 + 279 报错 + 16 check）

### 0.0.69 — 2026-11-12

**泛型构造器调用语法**：`Pair<int,long>(3, 9)` / `Vec2<int>(3, 4)`；`check_call` 的 `type_args` 分支先查泛型函数，否则按泛型 struct 值构造实例化（`instantiate_generic_named`），复用现有 `struct_ty` 构造检查。新增 `find_generic_template` 统一模板查找。`Pair<A,B>`、`Vec2<T>` 端到端可用。新增 `generic_ctor` 用例

**测试**：378/378（83 成功 + 279 报错 + 16 check）

### 0.0.68 — 2026-11-12

**泛型 M3d：泛型 struct 的方法/构造器/析构器**：解除「仅字段」限制；实例化时解析方法/构造器签名、注册 `ty->methods`/`ty->ctors`、生成符号（`method_cname`/`ctor_cname`/`dtor_cname` 对 owner 做 `ident_safe`，`Vec<int>` → `Vec_int_`）、跑 `check_struct` 检查方法体（带检查上下文保存/恢复，支持嵌套实例化）。`Vec<T>` 的 `length()/at()/fill()`、`Wrap<T>::twice()` 端到端可用。限制：泛型 struct 仍不支持 `const` 成员与接口基类；构造器调用语法 `Pair<int,long>(...)` 待后续。新增 `generic_methods` 用例

**测试**：377/377（82 成功 + 279 报错 + 16 check）

### 0.0.67 — 2026-11-12

**泛型 M3a+M3b：接口运算符成员 + 结构化约束**：接口允许 `T operator+(T a, T b);`（`MethodDecl::is_operator`）；`Self` 不再改写为接口名，改为 `TypeSpec::is_self` + `MethodSig::ret_self/params_self` 标记，sema 在接口签名解析时正确替换。**约束检查改为结构化**：`T: I` 按成员形状匹配，方法查方法表（含基类链/接口默认），运算符查内置或可见 unit 的 `operator`（精确签名，不做隐式转换）；`&` 全满足、`|` 任一。内置类型零成本满足（`int: IAddable` 因内置 `+`）。失败诊断给出缺失项。新增 `generic_bounds` 端到端、`generic_bound_missing` 报错用例

**测试**：376/376（81 成功 + 279 报错 + 16 check）

### 0.0.66 — 2026-11-12

**泛型 M2d：自由函数单态化**：调用点 `f<int>(x)`（`parse_postfix_tail` 中 `Name<` 试探解析，失败回退比较表达式）；`Call::type_args`；`instantiate_func`（实例缓存、签名解析、检查上下文保存/恢复以支持嵌套调用、`func_cname` 符号安全化）；`mangle.cpp` 对实例名做 `ident_safe`（`Box<int>` → `Box_int_`）。`T max<T>(T,T)`、`T twice<T>(T)` 端到端可用。新增 `generic_func` 端到端用例、`generic_arity` 报错用例

**测试**：374/374（80 成功 + 278 报错 + 16 check）

### 0.0.65 — 2026-11-12

**泛型 M2：struct 单态化端到端**：AST 深拷贝（`src/ast_clone.cpp`）+ 类型参数替换（`src/ast_subst.cpp`）+ 实例化器（`src/sema_generic.cpp`，惰性实例化 + 缓存 + 递归防护）。`struct Box<T> { T v; }`、`Pair<K,V>`、`Box<Box<int>>` 均可实例化并生成可执行文件；产物是普通 struct，`same_type` / mangle / 布局 / codegen 零改动。当前限制：泛型 struct 仅支持字段（方法/构造/接口/`align(N)` 待 M3）；自由函数泛型待 M2d。新增端到端用例 `generic_struct`，删除 M1 的「未实现」报错用例

**测试**：372/372（79 成功 + 277 报错 + 16 check）

### 0.0.64 — 2026-11-12

**泛型（M1：语法层）**：`<T>` 类型参数声明（自由函数 / struct / class / interface 均可）；`Vec<int>` 类型实参与 `Vec<Vec<int>>` 的 `>>` 拆分；约束 `T: I` / `T: A & B` / `T: int | long`（混用报错，提示拆分参数）；`Self` 在接口体内改写为接口名占位；泛型基类 `: ISeq<T>` 占位；`struct S : I` 语法前置（backlog U33）。模板在 sema 收集阶段移入 `Unit::templates`，不检查/不发射；使用处报「实例化未实现」。实例化架构（克隆 AST + 重命名，codegen 零改动）与全部决策见 backlog U34

**测试**：372/372（78 成功 + 278 报错 + 16 check）

### 0.0.63 — 2026-11-11

**借用返回 + 借用接收者（ref 语义补全）**：①**`ref T` 返回类型**：`ref T f()` / `ref const T f()`（自由函数、struct/class 方法、接口方法、运算符）；借用来源必须是 `ref` 参数、`this`、全局/静态，或路径经过指针/切片的堆存储——借局部、借按值参数（含内联字段）、借临时值均编译错误；结果不可存储（`ref T r = f();` 报错），只能当场写穿（`f() = v`、`f() += 1`、`f().field = v`）、作 `ref` 实参或 `for ref` 绑定；`ref const` 位置不能以可变 `ref` 返回。②**`ref` 参数放宽到标量**：基本类型/枚举/指针也可借用（`void inc(ref int x)`），标量借用赋值即写穿；struct 借用仍按「不可重绑定」要求写字段。③**借用接收者**：运算符参数支持 `ref`（`ref int operator[](ref C c, usize i)`），修掉「值接收者写进拷贝、静默丢写」；方法借用返回用隐式 `this`（`ref int at(usize i) { return v[i]; }`）。④**`[]` 返回借用**：`c[i]` 直接是可写左值（`=`/`+=`/取用/传 `ref`/`for ref` 全通），`[]=` 不再必需。⑤**`for ref` 支持用户类型**：索引形态要求 `[]` 返回 `ref T`。⑥符号名编码参数借用位（`f(T)` vs `f(ref T)` 不再撞名）；`ref` 出现在变量/字段/元素位置给出「绑定模式不是类型」诊断。新增用例：ref_return（1 成功）、ref_return_local / ref_return_param_copy / ref_return_const / ref_return_assign_const / ref_return_extern / ref_return_void / ref_var_decl / ref_loop_const / ref_const_to_mutable（9 报错）；ref_param_class / forin_ref 期望文本更新

**测试**：370/370（78 成功 + 277 报错 + 15 check）；冷编译零 warning

### 0.0.62 — 2026-11-11

**语言特性批（TODO 全部执行）**：①**类型别名** `type A = B;`（上下文关键字，声明起始位置 `type Ident =` 才解析；每条一个别名；透明别名、支持别名套别名/前向引用/循环检测；unit 级 + `public` 跨 unit；`TypeAliasDecl` + `UnitInfo::aliases` + 惰性解析）。②**`ref` 参数**（仅自由函数）：`ref T p` 可变借用、`ref const T p` 只读借用；只借纯值 struct（无 RC 字段/无析构器/非 union）；实参须可寻址左值、不收临时值、`const` 实参不能绑可变借用；不可重绑定/存储/返回；C 侧传指针（参数槽存指针、调用点传地址）；方法/构造器/`extern` 明确报错。③**range 扩展**：`..=` 含上界、`for i in a..b step n`（非零编译期常量；负 step 倒序；`a..b` 不自动反序）；索引形态的 `i` 改为迭代计数；switch 分支区间支持 `..=`、`step` 报错。④**花括号区间填充**：`int[10] a = {1, 2, 5..9, 10};`（端点须编译期常量、仅定长整数数组、展开后参与补零/超长检查）。⑤**位域明确拒绝**：struct/union/packed 里的 `int x : 3;` 给出明确诊断；U16 关闭。⑥**文档**：`switch` 扩展定案不做（6.6）、`ref` 是绑定模式不引入 `T&`、`string` 归为库内容（新增 `docs/lib-std-string.md`，SPEC 只保留语言可见契约）、class 机制收尾（3.8 标题、表 74/87 与 94 统一、U9/U10/U23 关闭）。新增用例：type_alias（1 成功 + 4 报错 + 1 check）、ref_param（1 成功 + 8 报错）、range_ext / switch_incl（2 成功）、bitfield（3 报错）、init_range（4 报错）

**测试**：360/360（77 成功 + 268 报错 + 15 check）；冷编译零 warning

### 0.0.61 — 2026-11-11

**`const` 语义收敛（U7 关闭）**：明确**不做** `const` class 视图与 `const` 方法；`const` 只保留两种含义——命名类型上的**只读绑定标记**（局部 / 参数：不可重绑、不可经它直写字段；class 句柄复制出去仍可写、经它仍可调用修改方法），以及指针 / 切片的 **pointee 只读**。**修正参数位置的 `const` 被静默丢弃**（此前 `void f(const Cat c)` 内可重绑定、可直接写字段；现与局部 `const` 一致），参数签名不含 `const`、不影响重载。SPEC 3.7 / 3.8 / 3.11 / 表 98 与 backlog U7 同步；新增 4 个报错用例 + 1 个 check 用例

**测试**：333/333（73 成功 + 246 报错 + 14 check）

### 0.0.60 — 2026-11-11

**源码拆分（纯物理搬移，不改逻辑）**：单文件全部 ≤1000 行。`parser.cpp`(1896) → `parser_impl.hpp` + `parser_unit/stmt/expr.cpp`；`codegen_llvm.cpp`(3912) → `codegen_impl.hpp` + `codegen_support/runtime/expr/expr2/stmt/class/func.cpp`；`sema.cpp`(4953) → `sema_impl.hpp` + `sema_decl/types/class1/class2/const/expr/call.cpp`。做法：内部类声明进 `*_impl.hpp`（`namespace muc_detail`）、成员函数按功能分段定义到各 TU、对外接口（`analyze` / `llvm_emit_*` / `parse_*`）与 `main.cpp` 完全不变；新增 `docs/code-map.md`（概念→文件:函数索引）与文件头索引；`AGENTS.md` 增加「代码结构」约定（≤1000 行/TU、辅助函数放置、默认参数只写声明）。

**测试**：328/328（73 成功 + 242 报错 + 13 check）；冷编译零 warning

### 0.0.59 — 2026-11-11

**类型级编译期常量 + 常量表达式求值（U28/U29 关闭）**：`struct`/`class` 体内 `const T NAME = <const-expr>;` 是**类型级编译期常量**——必须当场初始化、必须是常量表达式、**不占实例存储**（不进字段表，不影响 `sizeof`/`alignof`/布局/ABI/构造/析构/RC），用 `Type.NAME` 访问、声明类型的方法体内可裸写、沿基类链查找；`obj.NAME` / `this.NAME` / 赋值 / 取地址 / `ref` 目标一律报错；可见性随 `public:`/`private:`（class 默认 private），重名与字段/方法/构造器冲突报错，union/interface 不允许；类型限标量（bool/整型/浮点/usize/isize）与枚举。新增**编译期常量求值器**（惰性 + 记忆化 + 循环检测，支持前向引用）：字面量、一元 `- + not !`、算术/位/移位/比较/`and`/`or`、`as`、引用 unit 级 `const` / 枚举成员 / 类型级常量、`sizeof(T)`/`alignof(T)`；除零与循环依赖报错；同时接管 unit 级 `const` 初始化（`const int B = A + 1;`、`const int C = sizeof(int) as int;`），字符串/聚合常量保持旧路径。**实例字段不再接受 `const`**（`const int x;` → ``const member `x` must be initialized``），修掉此前「`&s.x` 意外是 `const int*`、`s.x = 2` 却合法」的半生效状态；不提供只读字段与 `readonly` 关键字。新增 const_member 成功用例与 23 个报错用例；**审计后修复**：常量求值的无符号 `>>` 改为与运行期一致（按位宽算术右移，`uint 0x80000000 >> 4` = `0xF8000000`）、`use` 同名常量不再静默取第一个（报歧义）、`char` 字面量 ≥128 在常量上下文按位模式接受、`evaluating` 标志改 RAII（异常路径不再误报 circular）、常量求值深度上限 512、诊断不再输出 `const int` 这类带限定符的类型名、**非 const 全局初始化器也走常量求值器**（`int g = 1 + 2;` 以前后端失败，现正常）、比较与浮点算术按 `usual_arith` 公共类型舍入（与运行期一致）。再补 const_global_init 成功用例与 6 个报错用例

**测试**：328/328（73 成功 + 242 报错 + 13 check）；冷编译零 warning

### 0.0.58 — 2026-11-11

**`\u{...}` Unicode 转义 + 码点类型定案**：字符/字符串字面量支持 `\u{` + 1–6 位十六进制 + `}`（不支持 `\uXXXX` 四位写法），范围 `U+0000`–`U+10FFFF`，拒绝代理区 `U+D800`–`U+DFFF` 与越界值；写在字符串里按 UTF-8 编码成 1–4 字节（`\u{0}` 与 `\0` 一样报错），写在字符字面量里限 `≤ U+FF`（超出提示改用字符串字面量）；Unicode **码点类型定为 `int`**（i32），不引入 `rune`/`unichar`/`char32` 别名；`s.chars()` 等码点视图留到字符串类实现时再定（backlog U3 暂缓，字符串仍按字节：`s[i]`/`for c in s`/`len(s)` 不变）。新增 unicode_escape 成功用例与 8 个报错用例（代理区/越界/非 hex/缺花括号/空 hex/超 6 位/字符超单字节/NUL）

**测试**：298/298（71 成功 + 214 报错 + 13 check）；冷编译零 warning

### 0.0.57 — 2026-11-11

**诊断与调试体验**：①诊断附带源码片段与插入符（`  |` + 行号 + `^`；tab 展开 4 列、CJK 按 2 列计算显示宽度）；②**多错误报告**——同一 unit 的全局/函数/class/struct/interface 检查逐项收集错误后统一输出（出错后重置作用域、循环深度与 auto 推断状态），单次上限 20 条；③`-g` / `--debug` 生成 **DWARF**（编译单元、函数 DISubprogram、语句级行号、局部变量与结构体成员），gdb 可断点、单步、`info locals`。新增 multi_errors（3 条错误一次报出）与 dbgvars 用例

**测试**：289/289（70 成功 + 206 报错 + 13 check）；冷编译零 warning

### 0.0.56 — 2026-11-11

**带参数的 `main`**：支持 `int main(int argc, char** argv)`（C 启动代码传入，`argv` 以 null 结尾、`argv[argc] == null`）；参数类型必须精确为 `(int, char**)`，1 个参数 / `char*` / 非 `int` 返回均编译错误；`char**` 而非 `string[]`（argv 不参与 RC）。新增 main_args 成功用例与 main_bad_argv 报错用例，main_params 期望值更新；SPEC 7、§12 #15 同步

**测试**：287/287（69 成功 + 205 报错 + 13 check）；冷编译零 warning

### 0.0.55 — 2026-11-11

**C 可变参数调用（`extern` 的 `...`）+ `std.cstdio`**：`extern` 声明可带 `...`（必须最后、至少一个命名参数），实参按 C 默认提升（`float→double`、小于 `int` 的整型→`int`、枚举→底层整型），`string` 自动传底层 `const char*`；含 RC 的实参编译错误；Mulan 内不支持自定义 varargs。codegen 用 `FunctionType::isVarArg` + `promoteVararg()`，ABI 由 LLVM 处理。另：`extern` 声明现在隐式 `public`（此前跨 unit 调用会被判未公开）；常量初值支持一元 `+`/`-`（`const int EOF = -1;`）；新增 `libs/std/cstdio.mu`（stdio 函数与常量绑定）。新增 varargs / cstdio 成功用例与 5 个报错用例；SPEC 新增 7.1，backlog U31

**测试**：285/285（68 成功 + 204 报错 + 13 check）；冷编译零 warning

### 0.0.54 — 2026-11-10

**联合体 `union`**：POD 值类型，所有成员偏移 0，`sizeof = max(成员)` 向上对齐到最大成员对齐；支持 `packed` / `align(N)`（与 struct 同规则）；成员限于 POD——禁止 `string`、动态数组 `T[]`、class/interface 引用、含引用字段或含用户析构器的 struct；无方法/构造器/析构器/继承，成员恒 public；初始化只有 `{}` 零初始化与具名 `{ member = value }`（恰好一个成员），不提供位置初始化与 `T(...)` 构造；读非活跃成员按 C 语义字节重解释。LLVM 侧用「对齐载体 + 尾部填充」表示、成员访问统一走偏移 0。新增 union_basic 成功用例与 11 个报错用例；SPEC 新增 3.4.1，backlog U14 关闭

**测试**：278/278（66 成功 + 199 报错 + 13 check）；冷编译零 warning

### 0.0.53 — 2026-11-10

**非法字符提示改用反引号**：`quote_char()` 输出由 `'x'` 改为 `` `x` ``，与其它诊断的代码片段风格统一；反斜杠 / 反引号本身 / 控制字符分别输出 `` `\` ``、`` `\`` ``、`` `\x0b` ``。新增 `invalid_char` 报错用例（此前该诊断无覆盖）

**测试**：266/266（65 成功 + 188 报错 + 13 check）；冷编译零 warning

### 0.0.52 — 2026-11-10

**诊断消息与 CLI 输出全部英文化**：`src/` 中 787 处字符串字面量（lexer/parser/sema/codegen/main/loader）改为英文，风格对齐 clang/rustc——小写开头、不加句号、`cannot`/`expected X but got Y`/`unknown`/`invalid` 等惯用表达，代码标识符仍用反引号；`--help` 重写为 `Usage:` / `build options:` / `Environment:` 三段式；代码注释保持中文不变。`tests/errors/*.err` 187 个期望文件同步为对应的英文消息片段（按原「第 N 条诊断」一一映射，`strings` 等成功用例不受影响）

**测试**：265/265（65 成功 + 187 报错 + 13 check）；冷编译零 warning

### 0.0.51 — 2026-11-10

**修复 Windows 控制台中文乱码**：`muc` 启动时若控制台输出代码页不是 UTF-8（简体中文系统默认 936/GBK），临时切到 `CP_UTF8` 并用 `atexit` 在退出时恢复原值。源码中的中文消息一直是 UTF-8 字节，此前被 936 控制台按 GBK 解释而显示为乱码；输出重定向到文件或管道时字节不变（仍是 UTF-8）

**测试**：265/265（65 成功 + 187 报错 + 13 check）；冷编译零 warning

### 0.0.50 — 2026-11-10

**编译器改名 `mulanc` → `muc`**：可执行目标与 CMake `project()` 名改为 `muc`（产物 `build/muc.exe`），`--version` / `--help` 输出同步；环境变量 `MULANC_CMD`/`MULANC_LIBS`/`MULANC_BACKEND`/`MULANC_LLVM_TRACE` 改为 `MUC_CMD`/`MUC_LIBS`/`MUC_BACKEND`/`MUC_LLVM_TRACE`，编译宏 `MULANC_VERSION` → `MUC_VERSION`；测试运行器默认编译器改为 `muc`；README / SPEC / docs / AGENTS 中的命令示例同步（`versions.md` 历史条目保留旧名）。另：新增 `LICENSE`（Apache-2.0）与 `NOTICE`，语言定名 Mulan（木兰），17 个源文件加 SPDX 头；示例与测试中的 `"mu-lan"` 字面量改为 `"Mulan"`（strings 用例派生的长度 / 截断期望值同步改为 5 / `Mu`）

**测试**：265/265（65 成功 + 187 报错 + 13 check）

### 0.0.49 — 2026-11-10

**U24：禁止含引用字段的 struct 取址**：`&s`、`&arr[i]`（元素为含引用 struct）编译报错——裸指针可绕过逐字段 retain/release；标量字段 `&s.x` 仍允许；含用户析构器的 struct 例外（本来就只能通过指针使用）。`dynarray` 用例的 `void keep(S* out, ...)` 改为按值返回；新增 3 个取址报错用例，`addr_rules` 覆盖允许形式（RC struct 的标量字段、析构器 struct 取址）

**测试**：265/265（65 成功 + 187 报错 + 13 check）

### 0.0.48 — 2026-11-10

**for-in 索引形态 + 用户类型迭代 + U21 落地 + 左值判定修复**：①`for i, x in seq` / `for i, ref x`（`i` 为 usize、循环内只读；内置序列的 `i` 是真实下标，区间是相对 `lo` 的偏移）；用户类型（struct/class）提供 `length() -> usize` + `operator[](C, usize) -> T`（按 usize 精确匹配）即走索引形态降级，`length()` 只求值一次，元素按拥有型局部变量逐轮 retain/release；多维数组 `T[m,n]`/`T[,]` 明确禁用 for-in（改用下标循环 + `len(a,k)`）；`for ref` 不支持用户类型；SPEC 新增 6.4.1–6.4.4 与 backlog U30。②**U21**：禁止对 class/interface 引用取地址（`&c`、`&i`）以及经引用访问的成员/元素取地址（`&c.x`、`&h.items[0]`、`&catArray[i]`、`&h.v[0]`），纯值 struct / 定长数组元素 / 指针变量取址不受影响；layout2 改用同源模型 struct 镜像验证 class 布局。③修复 `is_lvalue` 把与 unit 同名的局部/全局变量误判为非左值（`unit t; S t = ...; t.x = 1;` 报「赋值左侧必须是可赋值的对象」）。新增 forin_index / unit_name_var / addr_rules 成功用例与 12 个报错用例

**测试**：262/262（65 成功 + 184 报错 + 13 check）

### 0.0.47 — 2026-11-10

**参数改为拥有型局部变量 + 移除 `del`**：函数/方法/构造器/接口方法参数在进入时 retain、退出时 release（覆盖 return/break/continue 路径），函数体内可自由重绑定参数；`for-in` 值绑定同样按拥有型局部变量处理（每轮 retain/release，覆盖 break/continue）；由此修复「`c = null` / `c = new C()` 赋给参数或循环变量会把调用方对象提前释放（use-after-free）或泄漏」的内存安全缺陷；`del` 语句删除（`del x;` ≡ `x = null;`，无独立语义），保留关键字仅用于迁移诊断；迁移全部测试/示例中的 `del`，删除 6 个 del 专用报错用例与 class_del_null，新增 del_removed 报错用例与 param_ownership 回归用例；SPEC 3.8/4/8.3 与决策表 #69/#70 同步

**测试**：247/247（62 成功 + 172 报错 + 13 check）

### 0.0.46 — 2026-11-10

**条件表达式 `?:`**：`cond ? a : b`，右结合、优先级低于 `or`；条件必须 `bool`；只求值被选中的分支；两侧类型按「同一类型 → 隐式加宽 → `null` 适配」收敛，都不成立要求 `as`；结果为 prvalue（struct/string/定长数组逐字段 retain，class/interface 借用分支 +1、`new`/调用结果转移所有权，动态数组按句柄语义），结果槽在 entry 块物化；结果不是左值（`(c ? a : b) = x`、`&(c ? a : b)` 报错），不参与编译期常量；两侧皆 `void` 时只能作表达式语句；顺带修复 `double d = 1;`（整数字面量适配浮点类型）在后端 `cast<IntegerType>` 断言崩溃；新增 ternary 成功用例与 5 个报错用例；SPEC 新增 5.2，#14/#104 定案

**测试**：252/252（62 成功 + 177 报错 + 13 check）

### 0.0.45 — 2026-11-10

**`switch`（无 fallthrough）**：`switch expr { 模式列表: { } }`，括号可选；分支体强制花括号、各自独立作用域；`else` 至多一个且必须最后；模式为编译期常量（整数字面量 / 字符字面量 / 带一元 `-` `+` 的字面量 / 枚举成员），`a, b` 多值任一命中；区间 `a..b` 半开且必须非空；枚举必须穷尽（缺成员报错；`else` 与之互斥，全覆盖时 warning）；枚举不支持区间与整数隐式转换；模式重复/重叠（含区间相交）编译错误；被匹配值只求值一次；`break`/`continue` 仍归最近循环；codegen 密集值走 LLVM `switch`（跳表）、区间走比较链；新增 switch_basic 成功用例与 15 个报错用例；SPEC 新增 6.6，`match` 取消保留字

**测试**：246/246（61 成功 + 172 报错 + 13 check）

### 0.0.44 — 2026-11-10

**显式 `this`**：class/struct 方法、构造器、析构器内可显式写 `this`；`this` 是当前对象的引用（struct 为对象本身的左值别名，非指针），支持 `this.field` 读写、`this.method()`、作为实参传递与按值返回；`this` 不可赋值、不可取地址（`&this` 报错）、不能 `del`；新增 explicit_this 成功用例与 this_addr / this_outside 报错用例，删除 struct_this 报错用例

**测试**：230/230（60 成功 + 157 报错 + 13 check）

### 0.0.43 — 2026-11-10

**参数声明不得共享类型（SPEC 表 26 定案）**：每个参数必须单独写 `类型 名称`，`int f(int a, b)` 为编译错误；解析器在 `parse_params` 中提前识别 C 式共享类型写法（`Ident` 紧跟 `,`/`)`），给出「每个参数单独声明类型」的明确诊断（此前只在 `)` 处报含糊的「期望参数名」）；类型后缀只作用于单个参数；变量/字段声明的多名字共享类型不变（SPEC 4.1 加交叉引用）；新增 param_shared_type 报错用例

**测试**：228/228（59 成功 + 156 报错 + 13 check）

### 0.0.42 — 2026-11-10

**布局修饰符 `packed` / `align(N)`**：`packed` 无参数（= `pack(1)`，无内部/尾部填充，`alignof` 1）、`align(N)`（2 的幂常量，覆盖整体对齐、`sizeof` 补齐到 N 的倍数、不改变字段偏移、`N < 自然对齐` 报错），二者正交可组合；布局单一真源 `type_align`/`type_size`（sema 与后端共用）；packed 用 LLVM packed struct（普通 struct 的 ABI 不变），`align(N)` 插显式填充并给 alloca/全局/成员访问设置对齐（packed 链一律 `align 1`）；class 对象改用同源「模型布局 + 填充」（对象对齐 = max(16, 字段最大对齐)，>16 用 `_aligned_malloc`）；切片分配点拒绝元素 `align > 8`（数据区在 24 字节头之后）；新增 layout / layout2 成功用例与 9 个报错用例

**测试**：227/227（59 成功 + 155 报错 + 13 check）；冷编译零 warning

### 0.0.41 — 2026-11-10

**空花括号 `{}` 零初始化**：声明处聚合类型（struct / 定长数组 / 切片 / `string`）用 `{}` 表示零值，聚合 `= 0` 报错并提示改用 `{}`；标量（数值/bool/char/指针/class 引用）写 `{}` 报错，改用各自常量（`0`/`0.0`/`false`/`null`）；codegen 空聚合走 `zeroinitializer` / 清零，全局空聚合用零常量；新增 empty_brace 成功用例与 brace_scalar / zero_aggregate 报错用例

**测试**：216/216（57 成功 + 146 报错 + 13 check）

### 0.0.40 — 2026-11-10

**枚举成员限定引用**：`Color.Red` 与 `Red` 等价，跨 unit 可写 `unit.Color.Red`；sema 在成员访问处先解析类型名（本 unit → use 的 public → std.string），成员是常量（不可赋值/取地址），非枚举类型写 `T.m` 报错；codegen 把该节点按 `enum_const` 直接折叠为常量；enum_cmp 与 vis_import_ok 覆盖限定引用，新增 4 个报错用例

**测试**：213/213（56 成功 + 144 报错 + 13 check）

### 0.0.39 — 2026-11-10

**枚举成为名义类型**：与整数双向无隐式转换（都要 `as`）；同枚举支持 `== != < <= > >=`（序 = 底层整数值）；跨枚举比较、枚举算术/位运算/一元 `-+~`/复合赋值、下标/区间端点用枚举一律报错；`usual_arith` 拒绝枚举，codegen 比较按 int 有符号序生成 `icmp`；新增 enum_cmp 端到端与 8 个报错用例，迁移 extras/enumfield/vis_import_ok

**测试**：209/209（56 成功 + 140 报错 + 13 check）

### 0.0.38 — 2026-11-10

**位取反改为 `~`**：与 C 一致，支持 `operator~` 重载（1 个参数）；`!` 不再使用并给出迁移提示；迁移 bitwise 用例，新增 op_neg 端到端与 bang_removed 报错用例

**测试**：200/200（55 成功 + 132 报错 + 13 check）

### 0.0.37 — 2026-11-10

**++/-- 表达式上下文统一报错**：函数实参、if/while 条件、return、初始化器、数组下标、new 参数、括号内都给出「没有返回值」的明确提示；新增 inc_call_expr / inc_return_expr 报错用例

**测试**：198/198（54 成功 + 131 报错 + 13 check）

### 0.0.36 — 2026-11-10

**禁止 `*(p++)`**：`++`/`--` 无返回值，括号内出现也报明确错误；`*p++` 仍解析为 `(*p)++`；新增 inc_paren_expr 报错用例并在 inc 中覆盖 `*p++`

**测试**：196/196（54 成功 + 129 报错 + 13 check）

### 0.0.35 — 2026-11-10

**++/-- 语句支持**：前缀/后缀 `++`/`--` 只能作为独立语句（无返回值，不能出现在表达式里）；语义降级为 `x += 1` / `x -= 1`；`i++; j++;` 合法；新增 inc 端到端与 inc_expr 报错用例

**测试**：195/195（54 成功 + 128 报错 + 13 check）

### 0.0.34 — 2026-11-10

**明确不提供 `T* const`**：`const` 只作基类型前缀（值/pointee 只读），不引入指针绑定只读与逐层 const；SPEC 3.7、#25 改为「不提供」并说明理由

**测试**：193/193（53 成功 + 127 报错 + 13 check）

### 0.0.33 — 2026-11-10

**const 改为 C 模型**：删除 `readonly`；`const` 成为类型限定符（前缀修饰基类型，`const int**` 表示最内层只读）；`T* → const T*` 隐式、反向需显式 `as`；写检查与隐式转换按 const 层判断；全仓库迁移 `readonly` → `const`；暂不支持 `T* const`

**测试**：193/193（53 成功 + 127 报错 + 13 check）

### 0.0.32 — 2026-11-10

**移除 `static`**：unit 级变量直接声明（默认 private，`public` 跨 unit）；`extern` 保留；全局变量 linkage 由 public/private 决定；顶层/函数内使用 `static` 给出明确迁移错误；迁移全部用例与示例，删除 unit_var_bare

**测试**：193/193（53 成功 + 127 报错 + 13 check）

### 0.0.31 — 2026-11-10

**string 对象化重写**：`string` 保留值类型 struct（canonical 布局/字面量/`s[i]`/`len`/`==`/for-in 降级不变），字段 private；新增构造器 `string(readonly char*)` 与成员方法 `length/empty/is_empty/at/cstr/sub/concat/find/starts_with/ends_with/trim`；删除 `str_*` 自由函数，`operator+` 薄封装 `concat`；放宽 `= 0` 允许有构造器的 struct 零值初始化；新增 string_methods 端到端用例，删除 struct_ctor_zero

**测试**：194/194（53 成功 + 128 报错 + 13 check）

### 0.0.30 — 2026-11-10

**public/private 取代 pub**：private=unit 内、public=跨 unit；struct 成员默认 public、class 成员默认 private；struct/class 支持 public:/private: 分段标签与逐成员前缀；struct 字段可 private，跨 unit 聚合初始化要求字段全 public；全仓库迁移并新增 4 个用例

**测试**：194/194（52 成功 + 129 报错 + 13 check）

### 0.0.29 — 2026-11-10

**C# 式导入可见性**：`use` 后 `pub` 符号可直接无前缀使用（类型/函数/全局变量/枚举常量/struct 构造）；本地声明遮蔽导入；多来源同名在使用点报歧义并要求限定名；函数重载跨 unit 合并；parser 用声明形状试探支持无前缀类型名；新增 6 个导入用例

**测试**：191/191（52 成功 + 128 报错 + 11 check）

### 0.0.28 — 2026-11-10

**多段 unit 名限定访问**：表达式 `lib.a.b.f()` 按最长已导入 unit 前缀折叠为单个 Name；类型 `lib.a.b.T` 在 sema 按最长前缀匹配；parser 的类型解析支持任意段点号；`std.string` 视为隐式已 use；新增 2 个 check 用例

**测试**：186/186（52 成功 + 125 报错 + 9 check）

### 0.0.27 — 2026-11-10

**可见性机制**：新增关键字 `pub`；顶层声明/class 字段与方法/构造器默认仅 unit 内可见，`pub` 跨 unit；`pub unit` 跨库导出；库边界 = unit 名第一段（跨库 `use` 需 `pub unit`）；`pub` 签名连锁检查；非 `pub` 符号 LLVM `internal` linkage；迁移 std.string/math/lib_* 并新增 11 个可见性用例

**测试**：184/184（52 成功 + 125 报错 + 7 check）

### 0.0.26 — 2026-11-09

**标准库目录与类型改名**：新增 `libs` 默认库搜索路径（入口目录、`-I`、`MULANC_LIBS`、当前目录及祖先的 `libs`/`libs/std`）；`libs/std/string.mu`（unit `std.string`）替代编译器内嵌 prelude；`unit`/`use` 支持点号名（`std.string` → `libs/std/string.mu`）；字符串结构体由 `Str` 改名为 `string`；测试/示例/文档同步

**测试**：172/172（52 成功 + 115 报错 + 5 check）

### 0.0.25 — 2026-11-09

**字符串改为 std.string.string struct，移除内建 `string`**：`libs/std/string.mu`（unit `std.string`）中的 struct `string { byte* p; usize n; byte[] owner; }` + 字符串库；字面量类型为 string（owner=null，rodata，零分配/零 RC）；`s[i]`/`len`/`==`/for-in 按 canonical 布局内置降级；复制/字段/返回走 U24 逐字段 RC；std.string 提供 `str_from_cstr`/`str_cstr`/`str_sub`/`str_concat`/`operator+` 与方法；移除 lexer/parser/sema 的 `string` 关键字，迁移全部测试与示例；新增 `str_struct` 用例；生成 IR 后运行 LLVM `-O2` 标准流水线

**测试**：172/172（52 成功 + 115 报错 + 5 check）

### 0.0.24 — 2026-11-09

**动态数组 `T[]` + 多维数组**：`new T[n]` 创建带 RC 头的堆数组（句柄 8 字节，O(1) 复制共享）；静态矩形 `T[m,n]`（嵌套定长数组、`a[i,j]`/`a[i][j]`）、动态矩形 `T[,]` + `new T[m,n]`、锯齿 `T[][]` + `new T[m][]`（一次一维，循环填行）；`len(a)`/`len(a,k)`（静态 axis 字面量、动态可运行期）；`T[N]→T[]`、`T[m,n]→T[,]` 分配复制；元素支持 class/interface 与含引用 struct，逐元素 release；null 句柄 len=0；extern 递归拒绝 `T[]`；含析构器 struct 仍禁作元素；移除 class/interface 切片禁用与旧借用视图语义

**测试**：171/171（51 成功 + 115 报错 + 5 check）

### 0.0.23 — 2026-11-09

**U24：struct 引用字段 + 逐字段 RC**：struct 可含 class/interface 字段（含数组/嵌套）；拷贝/传参 retain、赋值先 retain 新再 release 旧、析构逐字段/逐元素 release；返回值借用转 owned；调用临时 struct 语句结束释放；未初始化本地 struct 自动清零；extern/全局递归拒绝；含用户析构器的 struct 仍禁拷贝；新增 `struct_rc_fields` 成功用例 + 2 个报错用例，移除过时的“struct 不能含 class 字段”用例

**测试**：163/163（49 成功 + 109 报错 + 5 check）

### 0.0.22 — 2026-11-09

**struct 构造/析构**：构造器重载、`S s;` 默认构造、`S s = S(args)` 就地构造；析构器在作用域/return/break/continue 自动调用；含析构器的 struct 暂禁按值拷贝/传参/返回/数组/字段/全局；新增 1 个成功用例 + 11 个报错用例

**测试**：161/161（48 成功 + 108 报错 + 5 check）

### 0.0.21 — 2026-11-09

**struct 方法**：静态派发、隐式 `this`、方法重载、数组/字段接收者、隐式方法调用；方法不得与字段同名；显式 `this` 暂不支持；新增 1 个成功用例 + 4 个报错用例

**测试**：149/149（47 成功 + 97 报错 + 5 check）

### 0.0.20 — 2026-11-09

按定稿收敛 8 位类型：`char`=i8（C 风格）、`byte`=u8 且为唯一无符号 8 位名（**删除 `ubyte`/`uchar` 别名**）；`s[i]`/字符串 for-in 返回 `byte`；字符字面量默认 `byte` 并按上下文适配 `char`/`byte`；新增别名移除用例

**测试**：144/144（46 成功 + 93 报错 + 5 check）

### 0.0.19 — 2026-11-09

**`char` 改为有符号 8 位（C 风格）；`byte` 改为无符号 8 位，`ubyte`/`uchar` 为其别名（同一类型）**；`'\xNN'` 按位模式解释；新增 char/byte 范围报错用例

**测试**：143/143（46 成功 + 92 报错 + 5 check）

### 0.0.18 — 2026-11-09

明确字符字面量语义：`char` 为无符号 8 位单字节，非 ASCII 字面量编译报错；SPEC 补第 101 条；新增 `char_bytes`（UTF-8 字节视图）与 `char_nonascii` 用例

**测试**：141/141（46 成功 + 90 报错 + 5 check）

### 0.0.17 — 2026-11-09

外链 C 禁传 class/interface 检查；新增接口返回接口、综合大用例（接口数组/for-in/向下转换/弱引用）

**测试**：139/139（45 成功 + 89 报错 + 5 check）

### 0.0.16 — 2026-11-09

新增比较/下标单次求值与临时释放回归用例（验证 0.0.14 的双重求值修复）

**测试**：136/136（43 成功 + 88 报错 + 5 check）

### 0.0.15 — 2026-11-09

SPEC 正式降级：`readonly` class 视图、`ref` 参数、interface→派生接口向下转换列为第一阶段不支持（表 98–100）；新增 `extern` 不可重载/不可与普通函数同名、`main` 不可重载检查

**测试**：135/135（42 成功 + 88 报错 + 5 check）

### 0.0.14 — 2026-11-09

**weak 目标销毁自动置 null**：全局 weak 注册表（目标, 字段地址）+ 自旋锁，赋值登记、覆盖/持有者析构注销、目标 destroy 时清零；顺带修复二元比较重复求值（函数调用在比较中执行两次、临时引用泄漏）

**测试**：132/132（42 成功 + 85 报错 + 5 check）

### 0.0.13 — 2026-11-09

清除冷编译最后一个 warning（unused variable）；冷编译零 warning

**测试**：131/131（41 成功 + 85 报错 + 5 check）

### 0.0.12 — 2026-11-09

`super.method` 只解析 class 链（接口默认方法经 super 调用报错）；`del null` 空操作；多 unit 接口/继承 check 用例（lib_ 依赖跳过）；下标复合赋值报错用例

**测试**：131/131（41 成功 + 85 报错 + 5 check）

### 0.0.11 — 2026-11-09

class 字段解析改为按基类依赖的拓扑顺序（同一 unit 内基类可后声明且字段布局正确）；新增前向基类、接口数组用例

**测试**：127/127（40 成功 + 83 报错 + 4 check）

### 0.0.10 — 2026-11-09

引用数组的逐元素 RC：拷贝 retain、覆盖/离开作用域 release、按值返回与临时实参逐元素转移（修复共享数组提前释放/泄漏）；新增 2 个端到端用例

**测试**：125/125（38 成功 + 83 报错 + 4 check）

### 0.0.9 — 2026-11-09

修复运算符/下标重载返回引用被当作借用导致的泄漏；override 返回类型与所有基类/接口声明比对（冲突报错）；新增 2 个用例

**测试**：123/123（36 成功 + 83 报错 + 4 check）

### 0.0.8 — 2026-11-09

收紧 `as`：指针/切片不再互相转换，切片转换要求元素类型相同；新增 2 个报错用例

**测试**：121/121（35 成功 + 82 报错 + 4 check）

### 0.0.7 — 2026-11-09

构造器省略 `super(...)` 时隐式调用基类无参构造器（C++ 语义）；新增链表（strong next + weak prev）与嵌套 `new`/基类数组用例

**测试**：119/119（35 成功 + 80 报错 + 4 check）

### 0.0.6 — 2026-11-09

`as` 向下转换：class→派生 class、interface→class 沿 desc 基类链运行期检查，失败得 null；desc 增加 base_desc；新增 downcast 端到端用例

**测试**：116/116（32 成功 + 80 报错 + 4 check）

### 0.0.5 — 2026-11-09

`new`/`len`/`sizeof`/一元表达式后可接后缀（`new C().f()`、`len(s) as int`）；`sizeof` 支持 class/interface/enum；禁止 class 指针、`del` 运算符下标、析构器 `return`；相关引用类型间身份比较；新增 4 个端到端用例与 3 个报错用例

**测试**：115/115（31 成功 + 80 报错 + 4 check）

### 0.0.4 — 2026-11-09

局部引用清理（块/return/break/continue）、临时引用释放、接口默认方法经类变量派发、接口数组布局修正（16 字节）、`del` 参数/循环变量限制；新增 4 个端到端用例

**测试**：107/107（26 成功 + 77 报错 + 4 check）

### 0.0.3 — 2026-11-09

对象模型端到端：class/接口/继承/构造析构/虚派发/引用计数/`weak` 的语义与 LLVM 代码生成；重载与运算符派发；新增 4 个端到端 class 用例、21 个报错用例、4 个 check 用例

**测试**：103/103（24 成功 + 75 报错 + 4 check）

### 0.0.2 — 2026-11-08

移除 C99 后端，LLVM 成为唯一后端；清理 `--emit-c`/`-DMULANC_WITH_LLVM`；版本机制接入 `--version` 与 LLVM 模块标志

**测试**：LLVM 后端 58/58；冷编译零 warning

### 0.0.1 — 2026-11-08

基线：C++20 编译器（lexer/parser/sema）、C99 后端、LLVM 后端（LLVM C++ API → COFF）、58 个回归用例、引用/对象模型设计讨论

**测试**：C 后端 58/58；LLVM 后端 58/58；冷编译零 warning
