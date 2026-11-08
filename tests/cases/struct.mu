// struct.mu — 结构体值语义与各种初始化方式
// 注意：`struct` 是关键字，所以 unit 名用 struct_case（文件名保持 struct.mu）。
unit struct_case;

extern int puts(readonly char* s);
extern int putchar(int c);

struct Point {
    int x, y;
}

void print_int(int v) {
    if v == 0 {
        putchar('0');
        return;
    }
    if v < 0 {
        putchar('-');
        v = -v;
    }
    int[20] digits;
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

// 值参数：函数内修改的是副本
void nudge(Point p) {
    p.x += 100;
}

Point make_point(int x, int y) {
    auto p = Point();    // 无参构造：全零
    p.x = x;
    p.y = y;
    return p;
}

int main() {
    // ---------- 位置初始化 {3, 4} 与值语义拷贝 ----------
    Point p = {3, 4};
    Point q = p;                 // 拷贝
    q.x = 10;                    // 不影响 p
    print_int(p.x); putchar(' ');
    print_int(q.x); putchar('\n');   // 3 10

    // ---------- 命名初始化 {x = 1, y = 2}，名字顺序可乱 ----------
    Point r = {x = 1, y = 2};
    Point s = {y = 9, x = 8};
    print_int(r.y); putchar(' ');
    print_int(s.x); putchar('\n');   // 2 8

    // ---------- 零值初始化 {T z = 0} ----------
    Point z = 0;
    print_int(z.x); putchar(' ');
    print_int(z.y); putchar('\n');   // 0 0

    // ---------- 表达式构造 Point(...) / Point() ----------
    auto c = Point(5, 6);
    auto zero = Point();
    print_int(c.y); putchar(' ');    // 6
    print_int(zero.x); putchar('\n'); // 0

    // ---------- 返回结构体、函数内装配 ----------
    auto made = make_point(7, 8);
    print_int(made.x); putchar(' ');
    print_int(made.y); putchar('\n');   // 7 8

    // ---------- 值传参：函数内改动不回传 ----------
    nudge(p);
    print_int(p.x); putchar('\n');   // 仍是 3

    // ---------- 整体赋值 = 按字节拷贝 ----------
    Point m = {1, 1}, n = {2, 2};
    m = n;
    n.y = 99;                        // 不影响 m
    print_int(m.y); putchar('\n');   // 2

    if p.x == 3 and q.x == 10 and r.x == 1 and s.y == 9 and made.y == 8 {
        puts("struct ok");
    }

    return 0;
}
