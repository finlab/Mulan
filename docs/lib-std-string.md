# std 库：`string`

> 本文档描述 `string` 的**实现细节与库 API**。
> 语言层可见契约（不可变、`len` 按字节、`s[i]` 是 `byte`、`==` 按字节、隐式转 `const char*` 等）
> 写在 [`SPEC.md` §3.3](../SPEC.md)，本文不重复；两者冲突以 SPEC 为准。
>
> 结论先行：**`string` 是库类型，不是语言机制**。语言只保证上面那组可见契约；
> 下面的布局、缓冲区、RC 与 API 都属于库，可以演进。

## 1. 布局（实现细节）

```c
struct string {
    byte* p;      // 指向 UTF-8 字节；NUL 结尾仅对完整字符串/字面量保证
    usize n;      // 字节长度（不含 NUL）
    byte[] owner; // 拥有型缓冲区（null = 字面量 / 借用视图）
}
```

- 3 个机器字，x64 上 24 字节；字段 private。
- 字面量 `"abc"`：`owner == null`，数据在只读段且 NUL 结尾；**零分配、零 RC**。
- 复制 = 24 字节值拷贝；`owner` 非 null 时 retain，析构时 release（编译器按普通 struct 字段 RC 处理）。
- `sub` 与原串共享 `owner`（retain），零分配；**不保证子串自身 NUL 结尾**。
- `concat` 分配新缓冲，返回新 `string`。
- `[]`、`==`/`!=`、`len`、`for-in`、隐式转 `const char*` 由编译器按 canonical 布局直接降级
  （内建特例，性能考虑）；`+` 是自由函数，内部调用 `concat`。

> `sizeof(string)` 属实现细节，**语言不承诺固定值**。跨 ABI 传字符串用 `const char*`。

## 2. 库 API

| 成员 | 语义 |
|---|---|
| `string(const char* c)` | 从 NUL 结尾的 C 串**复制**构造 |
| `length()` | 字节长度（与 `len(s)` 相同） |
| `empty()` / `is_empty()` | 等价；长度为 0 |
| `at(i)` | 取第 `i` 个字节；**越界返回 0**（不 panic） |
| `cstr()` | 返回 `const char*`（要求 NUL 结尾） |
| `sub(start, count)` | 子串，共享 `owner`（retain，零分配） |
| `concat(other)` | 拼接，分配新缓冲 |
| `find(needle)` | 查找；找不到返回 −1 |
| `starts_with(p)` / `ends_with(p)` | 前后缀判断 |
| `trim()` | 去掉首尾 ASCII 空白 |

UTF-8 细节：字符串按**字节**索引与遍历（`s[i]`、`for c in s`、`len(s)` 都不是码点语义）。
按码点遍历的 API（`chars()` 等）尚未实现，见 [design-backlog U3](./design-backlog.md)。

## 3. 现状与遗留

- 编译器目前把 `string` 当**内建特例**处理（字面量零 RC、`len`/`for-in`/`[]`/`==` 直连、
  隐式转 `const char*`），与「库类型」的文档定位不一致，属于**实现优化欠账**。
- 真正「库化」（去掉内建特例、改为普通 struct + 库定义成员）的前置条件：
  struct 用户析构器与拷贝/赋值语义（`design-backlog.md` U24 遗留的「含析构器 struct 仍禁拷贝」）。
- 在这之前，改动 `string` 布局需要同步改 codegen 的 canonical 布局假设。
