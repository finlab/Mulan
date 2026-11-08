# 对象模型实现计划（v0.3）

目标：在现有 LLVM 唯一后端上落地 SPEC 3.8–3.14 的对象模型。
原则：分里程碑推进，每步保持现有 58 个回归用例全绿；新能力单独加用例。

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
- 验收：`mulanc dump` 可解析含这些语法的文件；既有 58 用例全绿

### M2 类型与语义（进行中）

已完成：

- `ClassType` / `InterfaceType`（含字段、方法签名、构造器参数表、基类、接口）
- class/interface 注册、字段解析（含继承字段扁平化）、基类/接口解析
- 方法/构造器/析构器签名与函数体检查、`this`、`super(...)` 基本检查
- `new` / `del`、类成员访问、隐式字段访问、class 数组类型
- `override` 存在性检查、抽象类禁止 `new`、struct 禁止 class 字段
- 新增 7 个对象模型报错用例；回归 65/65

待完成：

- 重载解析（函数/方法/构造器）与歧义检查
- 运算符语义（`operator[]` / `operator[]=`、算术/比较派发）
- 接口实现完整性、抽象方法继承检查
- class 数组的逐元素 RC 语义（codegen）
- M3 的 LLVM 代码生成

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

- M1 已完成并合入主干；sema/codegen 尚未识别 class/interface（对含对象语法的文件会报“未知类型”）。
- 既有 58 个用例保持全绿。
