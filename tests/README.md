# Mulan 回归测试

`run_tests.py` 对每个用例执行：

```
<MUC_CMD> build <case.mu> -o build/tests/<name>.exe --run
```

把程序的 stdout 与 `tests/expected/<name>.txt` 比较后给出 PASS/FAIL。
只用 Python 标准库；在**仓库根目录**运行：

```powershell
# 编译器命令由环境变量 MUC_CMD 决定（默认 muc）
$env:MUC_CMD = "build/muc.exe"
python tests/run_tests.py                 # 全部用例
python tests/run_tests.py --filter arith  # 只跑名字含 "arith" 的用例
python tests/run_tests.py -v              # 失败时打印完整实际输出
python tests/run_tests.py --keep-c        # 保留生成的中间产物（默认编完即删）
```

全通过退出码 0，任一失败退出码 1（编译器缺失/编译失败报 FAIL，不崩溃）。

## 怎么加用例

1. 写 `tests/cases/<name>.mu`：
   - 以 `unit <name>;` 开头（名字必须是合法标识符；`struct` 等关键字不能作 unit 名，
     例如 `struct` 用例的 unit 名是 `struct_case`）；
   - 自包含：不 `use` 其它 unit；需要输出时自己声明
     `extern int puts(readonly char* s);` / `extern int putchar(int c);`
     （`print_int` 可从现有用例复制）；
   - `main` 正常路径 `return 0;`；
   - 条件必须是 bool（`if x` 非法）、没有 `++`、没有 C 风格 for、
     `!` 是按位取反不是逻辑非（逻辑非是 `not`）、没有 `&&`/`||`（用 `and`/`or`）。
2. 写 `tests/expected/<name>.txt`：真实推导的期望输出（不要占位符）。
3. 验证：`build\muc.exe check tests/cases/<name>.mu`，然后跑运行器。

文件名即用例名；不需要在任何地方注册。`examples/example.mu` 由运行器
自动作为一个用例（期望输出 `tests/expected/example.txt`）。

## 比较规则 / 已知限制

- 忽略每行行尾空白、忽略 CRLF/LF、忽略末尾空行。
- `build --run` 会把编译器状态行（`+ gcc ...`、`ok: <exe>`）混入 stdout，
  运行器剔除以 `+ `、`ok: `、`error: ` 开头的行——
  **用例自身不要输出以这些前缀开头的行**。
- 期望输出在 32 位 `int`、补码、移位规则（`>>` 算术右移）语义下推导，
  避免任何未定义行为（越界、溢出、读未初始化）。

## 用例覆盖点

| 用例 | 覆盖点 |
|---|---|
| `arith` | 四则、`%`、移位、优先级（+ 比 `<< >>` 紧；`&`/`^`/`\|` 分层；位运算比比较紧）、while 累加、复合赋值 |
| `bool_ops` | `not a == b` ≡ `not (a==b)`；`not a and b` ≡ `(not a) and b`（De Morgan 对照）；`and` 比 `or` 紧；`==` 左结合；真值表 |
| `bitwise` | `& \| ^ !`（`!` 按位取反）、`<< >>`（含负数）、复合赋值 `&= \|= ^= <<= >>=`、置位/清零/翻转 |
| `control` | `if/elif/else`、`while` + `break/continue`（含 `while true`）、`for i in 0..n`、区间端点表达式、嵌套循环 |
| `struct` | 值语义（拷贝/传参互不影响）、`{x = 1, y = 2}`、`{3, 4}`、`= 0` 零初始化、`Point(...)`、`Point()` |
| `arrays` | `int[N]` 聚合初始化补零、值语义拷贝与传参、`len`、`for v in arr`、`for ref v in arr` 改元素 |
| `forin_index` | 索引形态用户容器（`length()` + `operator[](C, usize)`，struct/class）、`for i, x`（数组真实下标、区间相对偏移）、临时容器只求值一次、空容器 |
| `strings` | `len`/`s[i]`（含下标字面量、for-in 遍历）、`char[N] = "..."` 拷贝+NUL、`puts(&buf[0])`、写 NUL 截断 |
| `unicode_escape` | `\u{...}`：字符串按 UTF-8 编码（1–4 字节、字节值与 `len`）、原始 UTF-8 等价、字符字面量单字节码点（含 `U+FF` → `char` 为 −1）、大小写十六进制与前导零 |
| `const_member` | 类型级 `const`：各标量类型、常量互引/一元/比较、`sizeof`/`alignof`、`Type.Name`、方法内裸名、基类常量与派生类裸名、private 同 unit、unit 级 const 引用类型级常量、`sizeof` 不因常量改变、无符号 `>>` 常量与运行期一致、局部变量遮蔽常量 |
| `const_global_init` | 非 `const` 全局的常量初始化：`int g = 1 + 2;`、`int G = Cfg.K;`、枚举/浮点/`long`、引用 unit 级 const，且全局仍可变 |
| `ptr` | `&x`/`*p`（含复合赋值）、指针参数、`p[i]` 与指针算术、`alloc/free`、`null` 判断、`readonly int*`、`(*pp).field` |
