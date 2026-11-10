// example.mu — mu-lan 第一阶段语法示例（草案）
// 设计目标：精巧、现代的类 C 语言；值语义；手动内存管理。
// 本文件使用规范草案 v0.1 中「已定/建议」的特性，尚未保证可编译。

unit example;

use math;

// ---------- C ABI 互操作 ----------
// extern 声明的函数由 C 运行时提供；const 只影响语言层的写检查，不影响 ABI。
extern int puts(const char* s);
extern int putchar(int c);

// ---------- 值类型结构体 ----------
// 字段可多个名字共享类型；赋值按字节拷贝。
struct Point {
    int x, y;
}

// unit 级变量默认 private；未初始化时零初始化。
int calls;

// ---------- 函数 ----------
// 返回类型必须写；无返回值必须显式写 void；空参数列表写 ()。
// 同一 unit 内允许先调用、后定义。
int frac(int n) {
    if n <= 1 {
        return 1;
    }
    return frac(n - 1) * n;
}

// for-in 遍历半开区间 0..n，替代 C 风格 for。
int sum_to(int n) {
    int sum = 0;
    for i in 0..n {
        sum += i;
    }
    return sum;
}

// while 的条件必须是 bool，没有整数真值转换。
int count_down(int n) {
    int count = 0;
    while n > 0 {
        n -= 1;
        count += 1;
    }
    return count;
}

// const int*：通过 p 不能写入；仅作接口契约，不承诺优化。
int sum_ptr(const int* p, usize n) {
    int s = 0;
    for i in 0..n {
        s += p[i];
    }
    return s;
}

// string 是内建不可变 UTF-8 视图；s[i] 取字节。
void print_string(string s) {
    for i in 0..len(s) {
        putchar(s[i]);
    }
}

// 仅用于示例输出：用 putchar 打印十进制整数。
void print_int(int v) {
    if v == 0 {
        putchar('0');
        return;
    }
    if v < 0 {
        putchar('-');
        v = -v;
    }
    int[20] digits;        // 可先声明后赋值；读取未写入的元素是 UB
    int i = 0;
    while v > 0 {
        digits[i] = v % 10;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putchar('0' + digits[i]);
    }
}

// ---------- 主函数 ----------
int main() {
    // 声明语法：类型在前，一个类型可声明多个变量，全部共享该类型。
    // int a = 0, b = 1;
    // int* p, q;       // p、q 都是 int*
    // int[5] x, y;     // x、y 都是 int[5]
    // 条件和循环头的括号可选；规范写法省略。C 式 if (cond) 仍然合法。
    const string greeting = "hello, mu-lan";
    puts(greeting);

    calls += 1;                // unit 级变量，零初始化后从 0 开始
    print_int(calls); putchar('\n');

    // const：不可变绑定；一整条声明中的变量都可以带初始化器。
    const int a = frac(10), b = sum_to(100), c = count_down(5);
    print_int(a); putchar('\n');
    print_int(b); putchar('\n');
    print_int(c); putchar('\n');

    // struct 是值类型：赋值即拷贝。
    // 声明中的聚合初始化用 = {...}；表达式构造用 Point(...)。
    // Point p = Point(3, 4) 也合法，RHS 的 prvalue 直接构造到目标，无额外拷贝。
    Point p = {3, 4};
    Point q = p;               // 拷贝
    q.x = 10;                  // 不影响 p
    if p.x == 3 and q.x == 10 {
        puts("struct is a value type");
    }

    Point r = {x = 1, y = 2};          // 命名聚合初始化
    Point s = {x = 5, y = 6};
    Point t = {7, 8}, u = Point(9, 10); // 多个声明符：花括号 / 表达式
    if r.x == 1 and s.y == 6 and t.x == 7 and u.y == 10 {
        puts("struct init ok");
    }

    // 零值初始化：聚合类型（数组/结构体/切片/string）用 {}，标量用各自常量（0 / null 等）。
    Point z = {};
    int zeroed = 0;
    int[3] zeroarr = {};
    string empty_text = {};
    if z.x == 0 and z.y == 0 and zeroed == 0 and zeroarr[2] == 0 and len(empty_text) == 0 {
        puts("zero-init ok");
    }

    // 字符串：内建 string 视图；字符串字面量可直接用于 string 参数。
    print_string("string view: ");
    print_string("mu-lan");
    putchar('\n');

    // 定长 char[N] 从 string 初始化：字节拷贝 + NUL。
    char[20] namebuf = "mu-lan";
    puts(&namebuf[0]);

    // 多变量共享类型：指针和数组都一样（与 C 的声明符规则不同）。
    int x = 1, y = 2;
    int* px = &x, py = &y;                 // px、py 都是 int*
    int[3] m = {1, 2, 3}, n = {4, 5, 6};   // m、n 都是 int[3]
    if *px + *py == 3 and m[0] + n[0] == 5 {
        puts("multi-declarator ok");
    }

    // 固定长度数组是值类型；for-in 默认按值绑定，ref 绑定可修改元素。
    int[5] arr = {1, 2, 3, 4, 5};
    int total = 0;
    for v in arr {
        total += v;
    }
    for ref v in arr {
        v *= 2;
    }
    print_int(total); putchar('\n');

    // 用索引遍历：len(arr) 返回 usize；0..len(arr) 是半开区间。
    for i in 0..len(arr) {
        print_int(arr[i]); putchar(' ');
    }
    putchar('\n');

    // auto：仅局部变量，必须有初始化器；多声明符推断结果必须一致。
    auto inferred_point = Point(11, 12);
    auto inferred_len = len(arr);
    auto inferred_sum = inferred_point.x + inferred_point.y;
    auto i2 = 0, j2 = 1;
    if inferred_len == 5 and inferred_sum == 23 and i2 + j2 == 1 {
        puts("auto inference ok");
    }

    // const 视图：arr 以只读指针传入。
    print_int(sum_ptr(&arr[0], len(arr)));
    putchar('\n');

    // break / continue：continue 进入下一次迭代，break 退出当前循环。
    for i in 0..10 {
        if i == 3 {
            continue;
        }
        if i == 7 {
            break;
        }
        print_int(i); putchar(' ');
    }
    putchar('\n');

    // 手动内存管理：alloc/free 是内建函数，底层对应 C 运行时分配器。
    int* buf = alloc(len(arr) * sizeof(int)) as int*;
    if buf == null {
        return 1;
    }
    for i in 0..len(arr) {
        buf[i] = arr[i];
    }
    int acc = 0;
    for i in 0..len(arr) {
        acc += buf[i];
    }
    free(buf as void*);
    print_int(acc); putchar('\n');

    // 指针可以为 null；解引用前必须自行检查。
    int* empty = null;
    if empty == null {
        puts("null pointer check");
    }

    // 切片字面量：声明中的花括号在当前作用域创建匿名数组，并绑定切片视图。
    double[] nums = {1.0, 2.0, 3.0, 4.0, 5.0};
    if len(nums) == 5 {
        puts("slice literal ok");
    }

    // 跨 unit 调用：use math; 后以 math.symbol 访问。
    print_int(math.square(7)); putchar('\n');
    print_int(math.abs(-42)); putchar('\n');

    // while + 手动推进；continue 在 while 中会跳到条件判断。
    int k = 0;
    while k < 3 {
        print_int(k); putchar(' ');
        k += 1;
    }
    putchar('\n');

    return 0;
}
