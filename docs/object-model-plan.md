# 对象模型实现计划（v0.4）

目标：在现有 LLVM 唯一后端上落地 SPEC 3.8–3.14 的对象模型。
原则：分里程碑推进，每步保持现有回归全绿；新能力单独加用例。

## 地基决定（已定）

- **RC**：原子计数（relaxed），对象可跨线程共享；析构器可能在任何线程执行。
- **异常**：非检查型；`Exception` 基类；支持 `finally`；实现前先做 Windows EH ABI spike。
- **泛型**：单态化（未来里程碑，当前只预留 mangling/desc/保留字）。
- **保留字**：`where try catch throw finally async await spawn shared atomic move unsafe typeof match`。
- **对象头**：`[rc][desc*]`；desc 含 `size/align/type_id/flags/base/vtable/destroy/itables`；
  构造未完成用 `constructed` 标志，销毁时跳过用户析构器。
- **清理路径**：从 M2/M3 起就用统一的 CleanupScope（RC + 析构），为异常展开预留。

## 里程碑

### M1 前端语法（已完成）

- 词法：`class/interface/new/del/weak/this/super/override/operator`、`~`、`:`
- AST：`ClassDecl` / `InterfaceDecl` / `MethodDecl` / `CtorDecl` / `DtorDecl` /
  `NewExpr` / `DelStmt`
- 语法：C++ 式 `class A : Base, I1, I2`、`interface I : J, K`、方法/抽象方法、
  构造器、`~T()`、`operator+` / `operator[]` / `operator[]=`、`new`/`del`、`this`/`super`

### M2 前端语义（已完成，98/98）

类型模型与注册：

- `ClassType` / `InterfaceType`（字段、方法签名、构造器参数表、基类、接口、抽象标记、weak 字段）
- class/interface 注册、字段解析（继承字段扁平化）、基类/接口解析、跨 unit 限定类型
- struct 禁止 class/interface 字段与自身按值嵌套

成员与继承：

- 方法/构造器/析构器签名与函数体检查、`this`（不可重绑定）、`super(...)` 必须首条语句
- `override` 必须显式；返回类型必须一致；覆盖/实现基类方法漏写 `override` 报错
- 字段与方法同名、派生字段遮蔽基类方法、重复构造器/方法/析构器、重复接口均报错
- 继承环（class / interface）检测

抽象与接口：

- 抽象类推导：继承链或接口中的抽象方法未实现 → 类抽象；抽象类不能 `new`
- 接口默认方法：冲突且类未覆盖时报错（含基类链上的接口）
- 抽象方法不能通过 `super` 调用

重载与运算符：

- 函数/方法/构造器重载：名字 + 实参个数筛候选；精确 0 / 隐式加宽 1；唯一最优、歧义报错
- 方法重载合并基类同名方法；同签名覆盖去重（派生优先）
- 运算符自由函数：二元、一元 `+`/`-`、`operator[]` / `operator[]=`（读写分离）
- `class`/`interface` 的 `==`/`!=` 固定身份比较；比较运算符必须返回 bool
- 运算符查找范围：当前 unit + `use` 的 unit + 操作数类型所属 unit（ADL 风格）

表达式与语句：

- `new` / `del`（`del` 目标规则、不能 `del this`）、类成员访问、隐式字段访问
- class 静态数组 `Cat[N]`；class/interface 切片禁止
- `null` 与 class/interface 的赋值、`c == null` 身份比较
- 隐式转换：class → 基类 / 接口；接口 → 父接口

测试：

- `tests/errors/` 74 个必须报错用例
- `tests/check/` 4 个仅语义检查的成功用例（codegen 尚未覆盖）
- 合计 98/98

明确**暂缓**（待语法/规则确定）：

- `readonly Cat` 视图与 `readonly` 方法（`readonly` 目前仅指针/切片）
- `ref` 参数（仅 for-in 的 `ref` 已实现）
- 接口静态转换 `as` 的向下转型语义
- 两个接口同签名、返回类型不同的冲突细化

### M3 LLVM 代码生成

- 对象布局：`[rc][class_desc*][fields...]`
- `class_desc`：vtable、destroy、itable 目录
- 方法调用：类虚派发、接口 itable 派发
- 构造：分配/清零、逐层 vtable、`super`
- 析构：用户析构器 → 字段逆序释放 → 基类析构器
- `new`/`del`：malloc + RC；`del` 置 null
- 类数组：逐元素 retain/release、赋值、销毁
- 运算符：按签名调用；`a[i]` / `a[i] = v` / 复合赋值
- 接口值：`{obj, desc}`、向上转换、`as` 向下转换
- 验收：对象模型示例可编译运行，输出与预期一致

### M4 测试与收尾

- 新增成功用例：class/构造/析构/继承/接口/重载/运算符/类数组
- 新增报错用例：抽象类实例化、override 签名错误、接口缺失、重载歧义、struct 含 class 引用等
- 文档更新 + 版本号（正式构建后 build +1）
- 与 struct/值语义、C 互操作、RC/weak 的交叉检查

## 当前状态

- M1、M2 均已完成并合入主干（`tests/` 98/98 全绿）。
- codegen 尚未识别 class/interface：含对象语法的文件仍只能 `check`，不能 `build`。
- 下一步：M3 LLVM 代码生成（对象头、desc、vtable/itable、构造/析构、RC、CleanupScope）。
