// arrays.mu — int[N] 聚合初始化与补零、len、for-in 值绑定与 ref 绑定
unit arrays;

extern int puts(const char* s);
extern int putchar(int c);

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

// 数组参数是值拷贝：函数内改动不回传
int sum5(int[5] xs) {
    int s = 0;
    for i in 0..5 {
        s += xs[i];
    }
    xs[0] = 999;    // 只影响副本
    return s;
}

int main() {
    // ---------- 聚合初始化，未给出的元素补零 ----------
    int[5] a = {1, 2, 3};
    print_int(a[0]); putchar(' ');   // 1
    print_int(a[2]); putchar(' ');   // 3
    print_int(a[3]); putchar(' ');   // 0（补零）
    print_int(a[4]); putchar('\n');  // 0（补零）

    // ---------- 数组是值类型：拷贝互不影响 ----------
    int[5] b = a;
    b[0] = 100;
    print_int(a[0]); putchar(' ');   // 1
    print_int(b[0]); putchar('\n');  // 100

    // ---------- 传参拷贝 ----------
    print_int(sum5(a)); putchar('\n');   // 1+2+3+0+0 = 6
    print_int(a[0]); putchar('\n');      // 1（没被改成 999）

    // ---------- for v in arr：值绑定，改 v 不影响数组 ----------
    for v in a {
        v = 0;
    }
    print_int(a[1]); putchar('\n');      // 2

    // ---------- 最简形式：先声明后逐个写入，再读 ----------
    int[5] c;
    for i in 0..5 {
        c[i] = i * i;
    }
    int total = 0;
    for ref v in c {        // ref 绑定：可修改元素
        total += v;
        v += 10;
    }
    // total = 0+1+4+9+16 = 30；c 变成 10 11 14 19 26
    print_int(total); putchar('\n');
    for v in c {
        print_int(v); putchar(' ');
    }
    putchar('\n');

    // ---------- len(arr) 是数组长度 N ----------
    auto n = len(a);
    if n == 5 {
        puts("len ok");
    }
    int s2 = 0;
    for i in 0..len(a) {
        s2 += a[i];
    }
    print_int(s2); putchar('\n');    // 1+2+3+0+0 = 6

    return 0;
}
