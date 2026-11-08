# 对象模型实现计划（v0.3）

目标：在现有 LLVM 唯一后端上落地 SPEC 3.8–3.13 的对象模型。
原则：分里程碑推进，每步保持现有 58 个回归用例全绿；新能力单独加用例。

## 里程碑

### M1 前端语法（已完成）

- 词法：`class/interface/new/del/weak/this/super/override/operator`、`~`、`:`
- AST：`ClassDecl` / `InterfaceDecl` / `MethodDecl` / `CtorDecl` / `DtorDecl` /
  `NewExpr` / `DelStmt`
- 语法：C++ 式 `class A : Base, I1, I2`、`interface I : J, K`、方法/抽象方法、
  构造器、`~T()`、`operator+` / `operator[]` / `operator[]=`、`new`/`del`、`this`/`super`
- 验收：`mulanc dump` 可解析含这些语法的文件；既有 58 用例全绿

### M2 类型与语义

- `mtypes`：`ClassType` / `InterfaceType`（含字段、方法表、基类、接口集合）
- 名字解析：class/interface 注册；成员字段/方法查找；`this`/`super`
- 继承：单继承检查、字段唯一、override 签名检查、抽象类检查
- 接口：实现检查、默认方法、接口多继承、冲突覆盖
- 构造器：重载解析、`super(args)` 位置检查、字段先零初始化
- 析构器：每类最多一个，RC 归零语义
- `new`/`del`：类型检查、抽象类禁止 `new`、`del` 目标规则
- 类数组：元素逐元素 RC 标注；struct 禁止
- 重载：候选集、转换等级、歧义；方法合并基类重载
- 运算符：`operator+` 等自由函数的 0/1/2 元解析、`[]`/`[]=` 读写区分
- 验收：`check` 对合法/非法对象模型用例给出正确诊断

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
