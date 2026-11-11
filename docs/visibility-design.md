# 可见性设计（v1）

> 状态：**已实现（0.0.30）**。
> 关联：`SPEC.md` §9、§12 表 #17；本文件取代 §9 旧的「`private` 待定」表述。

## 0. 决策摘要

| 问题 | 决定 |
|---|---|
| 关键字 | **`public` / `private`**（`pub` 已废弃） |
| 档位 | `private` = 本 unit 内可见；`public` = 跨 unit 可见 |
| 顶层默认 | `private`（unit 内） |
| struct 默认 | `public`（字段/方法/构造器），符合 C 交互习惯 |
| class 默认 | `private`（unit 内），符合封装习惯 |
| 分段标签 | 支持 `public:` / `private:`，作用于后续成员 |
| 逐成员前缀 | 支持，覆盖当前段的默认值 |
| 析构器 | 不可修饰 |
| interface 方法 | 不可修饰，恒随 interface 可见性 |
| 库边界 | unit 名第一段；跨库 `use` 要求 `public unit` |
| 导入可见性 | `use` 后目标 unit 的 `public` 声明可直接无前缀使用 |
| 后端符号 | 非 `public` 顶层符号用 LLVM `internal` linkage |

`private` 的边界是 **unit**，不是类内。原因见 §2.3。

## 1. 非目标

- 不做类内私有、`protected`、友元、测试后门。
- 不做预编译库 / 接口文件（`.mui`、`.mli` 式）。当前只支持源码分发。
- 可见性**不是安全边界**：语言有 `void*`、`as`、手工内存，知道布局即可绕过。它是编译期 API 契约。

## 2. 模型

### 2.1 两个档位

| 档位 | 含义 | 默认出现在 |
|---|---|---|
| `private` | 本 unit 内所有代码可见 | 顶层声明、class 成员 |
| `public` | 跨 unit 可见（跨库还需 `public unit`） | struct 成员 |

```c
private int helper(int x);       // 显式 private（与默认相同，用于文档）
public int api(int x);           // 跨 unit 可调用

public struct Vec2 {             // struct 成员默认 public
    float x, y;
}

class Cat {                      // class 成员默认 private（unit 内）
    string name;
    public string get_name() { return name; }
}
```

### 2.2 默认值

| 位置 | struct | class | 顶层 |
|---|---|---|---|
| 字段 | `public` | `private` | —— |
| 方法 | `public` | `private` | `public`/`private` 由声明决定 |
| 构造器 | `public` | `private` | —— |
| 析构器 | 不可修饰 | 不可修饰 | —— |
| 常量 / 全局变量 | —— | —— | `private` |

### 2.3 为什么 `private` = unit 内

- 运算符只能是自由函数（`SPEC.md` 3.11），`std.string` 的 `operator+` 必须能访问 `string` 的字段。
- 同 unit 继承、同 unit 测试天然可用。
- 一条规则「无修饰 = 默认值」同时覆盖顶层与成员。

类内私有需要 `friend` 或给运算符开例外，等于又回到 unit 级，因此不采用。

### 2.4 分段标签

struct / class 体内可以用 C++ 式分段：

```c
public class Box {
public:
    int value;
    public Box(int v) { value = v; }
    int get() { return value; }

private:
    int secret;
    int hidden() { return secret; }
}
```

规则：

- 段标签 `public:` / `private:` 切换**后续成员**的默认值，直到下一个标签。
- 成员前缀（`public int x;`）覆盖当前段默认值。
- class 初始段 = `private`，struct 初始段 = `public`。
- 段标签只作用于字段/方法/构造器；析构器始终不可修饰。

### 2.5 字段可见性与聚合初始化

- struct 字段默认 `public`；显式 `private` 的字段对外不可见。
- 跨 unit 的聚合初始化（`S s = {...}` / `S(...)`）要求**所有字段都是 `public`**，否则报错。
- 同 unit 内不受限（`private` = unit 内可见）。
- 有构造器的 struct 不能用聚合初始化（既有规则）。

### 2.6 库边界：unit 名的第一段

| unit 声明 | 库 |
|---|---|
| `unit std.string;` | `std` |
| `unit acme.detail.scratch;` | `acme` |
| `unit math;` | 根库（应用） |

- 同库 unit 之间可直接 `use`；跨库 `use` 要求 `public unit`。
- 边界由名字决定，不由文件位置决定。
- 一个项目的 unit 命名应统一（全无前缀，或统一前缀）。

### 2.7 导入可见性（C# 式无前缀）

`use foo;` 之后，`foo` 的所有 `public` 声明可以直接无前缀使用：

```c
use lib_imp_ok;

int main() {
    Pair p = {1, 2};
    int a = add2(3);
    Color c = Red;
}
```

查找顺序（前面的遮蔽后面的）：

1. 局部变量 / 参数 / 当前类型字段
2. 当前 unit 顶层声明
3. 直接 `use` 的 unit 的 `public` 声明
4. `std.string`（隐式依赖）

规则：

- **本地优先**：当前 unit 有同名函数时完全遮蔽导入。
- **冲突在使用点报错**：多个 `use` 的 unit 提供同名类型/变量/枚举常量时要求写限定名；同签名函数重载报歧义。
- **只导入 `public`**；`use` 不传递。
- 类型名同样适用：parser 对 `Ident Ident` 做声明试探，sema 按上述顺序解析。

## 3. 跨 unit 访问规则

1. `use other;` 后，`other` 的 `public` 声明可直接无前缀使用，也可写 `other.f` 限定。
2. 跨 unit 访问 `private` 声明报「未声明为 public」。
3. 跨库：`lib_of(other) != lib_of(cur)` 时，`other` 必须 `public unit`。
4. `use` 不传递。
5. 冲突时要求写限定名。

## 4. 连锁规则（public 不能泄露非 public）

在 `sema.cpp::check_public_signatures()` 里检查：

1. `public` 函数的参数/返回类型必须是 `public` 且所在 unit 可见。
2. `public` 字段的类型必须 `public`。
3. `public` class/struct 的基类、接口必须 `public`。
4. `public class` 的 `public` 方法签名必须 `public`。
5. `public interface` 的方法签名必须 `public`。
6. 非 `public` 声明可以自由引用任何可见类型。

## 5. 实现

| 位置 | 改动 |
|---|---|
| `src/lexer.cpp` | 关键字 `public` / `private`（`pub` 移除） |
| `src/ast.hpp` | `Decl::is_public`、`MethodDecl/CtorDecl::is_public`、`StructDecl/ClassDecl::field_visibility`、`Unit::is_public` |
| `src/parser.cpp` | 顶层 `public`/`private`；struct/class 分段标签与成员前缀；默认值（struct public / class private） |
| `src/types.hpp` | `Field::is_public`、`MethodSig::is_public`、`Type::ctor_is_public` |
| `src/sema.cpp` | `lib_of` / `unit_visible` / `decl_visible` / `type_is_public`；导入查找与歧义；聚合初始化字段检查；`check_public_signatures` |
| `src/codegen_llvm.cpp` | 非 `public` 顶层符号 → `InternalLinkage`；`public`/`extern`/`main` 外部链接 |

## 6. 迁移

| 位置 | 改动 |
|---|---|
| `libs/std/string.mu` | `pub` → `public` |
| `examples/math.mu` / `examples/example.mu` | `pub` → `public` |
| `tests/check`、`tests/errors` 的 `lib_*` | `pub` → `public` |
| 全部文档 | `pub` → `public`/`private` |

## 7. 测试

全部 194 个用例通过（52 成功 + 129 报错 + 13 check）。

可见性/导入相关用例：

| 类型 | 用例 |
|---|---|
| 成功 | `vis_pub_ok`、`vis_cross_lib`、`vis_multi_seg`、`vis_multi_seg_type`、`vis_import_ok`、`vis_import_shadow`、`vis_struct_default_public`、`vis_sections_ok` |
| 报错 | `vis_call_private`、`vis_type_private`、`vis_field_private`、`vis_method_private`、`vis_ctor_private`、`vis_struct_field_private`、`vis_sections_private`、`vis_use_hidden_unit`、`vis_signature_leak`、`vis_interface_method_pub`、`vis_extern_pub`、`vis_import_ambig_*` |

## 8. 已知限制与开放问题

| # | 问题 | 状态 |
|---|---|---|
| V1 | 多段 unit 名限定访问 | 已修（0.0.28） |
| V2 | `override` 可见性一致性（接口方法恒公开 → 实现方法应 public）未强制 | 待定 |
| V3 | 预编译库（`.o` + 接口描述）的库边界 | 二期 |
| V4 | 符号名中的点号 unit（`mu_std.string_...`）是否需要转义 | 当前工具链可用 |
| V5 | `private` 用于局部变量时报错信息不够友好 | 待改 parser 提示 |
| V6 | 同签名跨 unit 函数冲突只报「重载歧义」，未列出具体 unit | 可增强错误信息 |
