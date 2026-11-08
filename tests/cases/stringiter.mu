unit adv2;

extern int puts(readonly char* s);
extern int putchar(int c);

struct P { int x, y; }

P mk(int a, int b) {
    return P(a, b);
}

void show(string s) {
    for c in s {
        putchar(c);
    }
    putchar('\n');
}

int main() {
    // 字符串按字节遍历
    show("hi");

    // 空切片
    int[] e = 0;
    int n = 0;
    for v in e { n += 1; }
    if len(e) == 0 and n == 0 { puts("empty slice ok"); }

    // 零值构造
    P z = P();
    if z.x == 0 and z.y == 0 { puts("ctor zero ok"); }

    // 结构体返回值 + 再次赋值
    P a = mk(5, 6);
    P b = a;
    b.x = 9;
    if a.x == 5 and b.x == 9 { puts("return value ok"); }

    // 全局 string 常量
    const string g = "global";
    const string h = g;
    if len(h) == 6 and h[0] == 'g' { puts("const string ok"); }

    // 32 位边界字面量
    uint u = 0xFFFFFFFF;
    if u == 4294967295 { puts("uint literal ok"); }

    // 长整型
    long big = 1;
    for i in 0..40 { big *= 2; }
    if big == 1099511627776 { puts("long ok"); }
    return 0;
}
