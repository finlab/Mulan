# Mulan（木兰）

[![CI](https://github.com/finlab/Mulan/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/finlab/Mulan/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**简体中文** | [English](README.en.md)

Mulan语言的设计思路：

1、在C语法基础上进行限制、修改、及扩充，使其更安全、更明确、更顺手；

2、编译型语言，具有接近c/c++的性能，保留C的底层表达能力；

3、提供基于值和引用的两种对象机制，以提供基于RAII和RC的自动内存管理；

4、异常、泛型、并发等其他现代语言特性

当前版本 **0.0.75**，尚未发布 1.0；语言规范见 [`docs/SPEC.md`](docs/SPEC.md)（草案 v0.2，逐条标注「已定 / 建议 / 待定」）。

## 设计取舍

语言特色不在"加了什么"，而在**每条规则都能说出为什么**：

| 决定 | 做法 | 理由 |
|---|---|---|
| **值与引用分离** | `struct` 值语义，`class` 引用语义，界限清晰 | 看到 `=` 就知道是拷贝还是共享 |
| **共享必须显式** | 遍历默认值绑定，要改元素写 `for ref v in a` | 没有隐藏别名，没有意外的原地修改 |
| **回收时机确定** | `class` 用原子引用计数；最后一个引用消失时析构立刻执行 | 无 GC 停顿，资源释放不靠"稍后" |
| **环靠 `weak` 断** | `weak T` 非拥有引用；环必须手动打断 | 不做环收集器，行为可预测 |
| **没有预处理器** | `unit` / `use` 模块 + `const` 常量 | 报错落在真实源码行，而不是展开后的宏 |
| **没有隐式转换** | 只有无损加宽；其余一律 `as` | 截断与重解释永远是你亲手写的 |
| **条件必须是 `bool`** | `if n` 编译错误，写 `n != 0` | 消灭 C 里 `=` / `==` 一类事故 |
| **循环只有两种** | `while` + `for-in`（区间 / 数组 / 切片 / `string`） | 不需要在 `for(;;)`、`do-while` 之间权衡边界 |
| **C ABI 是一等公民** | `extern` 直接声明 libc；`struct` 布局可控（`packed` / `align(N)`） | 和 C 共存，而不是取代 C |

## 一段能跑的代码

值类型、引用类型、接口多态、确定性析构，都在 30 行里：

```c
unit demo;

extern int puts(const char* s);
extern int printf(const char* fmt, ...);

int live = 0;

interface Shape {
    double area();
}

struct Rect {                 // 值类型：栈上分配，赋值即拷贝，无引用计数
    double w, h;
    double area() { return w * h; }
}

class Circle : Shape {        // 引用类型：原子引用计数，析构时机确定
    double r;
    Circle(double rr) { r = rr; live += 1; }
    ~Circle() { live -= 1; }
    override double area() { return 3.14159 * r * r; }
}

int main() {
    Rect box = {2.0, 3.0};

    Shape[2] shapes = { new Circle(1.0), new Circle(2.0) };

    double total = box.area();
    for s in shapes {
        total += s.area();
    }

    shapes[0] = null;         // 最后一个引用消失，两个 Circle 立即析构
    shapes[1] = null;
    if live == 0 and total > 21.70 and total < 21.72 {
        puts("21.71, no survivors");
    }
    return 0;
}
```

```console
$ muc build demo.mu --run
21.71, no survivors
```

这段代码里没有的是：预处理器、GC、隐式转换、`virtual` 关键字、`->` 运算符（统一用 `.`）。

更多示例：[`examples/example.mu`](examples/example.mu)（完整语法巡礼）、[`examples/math.mu`](examples/math.mu)（跨 unit 调用）。

## 语言现状

**已实现**

| 类别 | 内容 |
|---|---|
| 类型 | 整数 / 浮点 / `bool` / `byte` / `char`；指针；定长数组；动态数组 `T[]`；切片；`string`；`struct`；`union`；`enum` |
| 对象模型 | `class` + 单继承 + `interface`；默认虚方法；原子 RC；`weak`；构造器 / 析构器 `~T()`；运算符重载 |
| 控制流 | `if` / `while` / `for-in` / `switch`（枚举穷尽检查）/ `break` / `continue` / `?:` |
| 函数 | 递归、重载（有界规则）、`extern` 变参调用（0.0.57） |
| 模块 | `unit` / `use` / `public` / `private`，同名 unit 自动加载 |
| C 互操作 | `extern` 声明、C ABI、`struct` 布局控制（`packed` / `align(N)`） |
| 工具链 | `check` / `dump` / `build`、`--emit-ir`、`-g` 生成 DWARF 调试信息、一次报出多个错误 |

**尚未实现**（诚实清单）

- 异常 `try` / `catch` / `throw`（设计已定，未实现）
- 泛型（打算单态化，目前只预留了关键字与 mangling）
- 闭包、函数指针、`goto`
- 标准库（`libs/std/` 目前只有 `cstdio` 与 `string` 两个最小绑定）
- `weak` 引用自动置空（当前需自行判断对象是否还活着）
- 非 Windows 目标：后端固定 `x86_64-w64-windows-gnu`，产出 COFF（方案已记录：
  跨平台方案已记录（内部文档），等语言机制完善后实施）

## 快速开始

前置：**Windows + MSYS2**、CMake ≥ 3.20、Ninja、g++ ≥ 11（C++20）、gcc、LLVM ≥ 17 开发库。

```powershell
# 1. LLVM（MSYS2）
C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm mingw-w64-x86_64-llvm"

# 2. 构建编译器（CMake 用 LLVM_DIR 定位 LLVM，未指定时自动探测 MSYS2 路径）
python -m cmake -S . -B build -G Ninja
python -m cmake --build build          # 产物：build/muc.exe

# 3. 试一下
build\muc.exe check examples\example.mu         # 只做语义检查
build\muc.exe dump  examples\math.mu            # 打印 AST
build\muc.exe build examples\example.mu --run   # 编译 + 链接 + 运行
build\muc.exe build examples\example.mu -g      # 带 DWARF，可被 gdb 断点/单步
```

依赖缺失时：`python -m pip install cmake ninja`；LLVM 装在非默认位置用
`-DLLVM_DIR=<path>/lib/cmake/llvm` 指定。

## 文档

| 文件 | 内容 |
|---|---|
| [`docs/SPEC.md`](docs/SPEC.md) | 语言规范，逐条标注已定 / 建议 / 待定 |

> 设计草案、实现规划与决策记录属于内部工作文档，不随仓库发布。

## 许可

**Apache License 2.0**，全文见 [`LICENSE`](LICENSE)，第三方声明见 [`NOTICE`](NOTICE)。

- 用木兰编写的程序、以及 `muc` 生成的目标文件 / 可执行文件，**不是**编译器的衍生作品，可按你选择的条款分发。
- 以 LLVM 为后端分发时，同时遵守 `Apache-2.0 WITH LLVM-exception`。
- 源文件首行统一带 `SPDX-License-Identifier: Apache-2.0`。

<sub>项目名「Mulan / 木兰」与其它同名开源项目无关。</sub>
