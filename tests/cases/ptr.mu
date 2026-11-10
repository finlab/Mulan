// ptr.mu — &x / *p、指针参数、p[i]、alloc/free、null 判断、const int*
unit ptr;

extern int puts(const char* s);
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

// 通过指针写调用方的变量
void add_to(int* p, int d) {
    *p += d;
}

void swap(int* a, int* b) {
    int t = *a;
    *a = *b;
    *b = t;
}

// 只读指针参数：接口契约
int sum_readonly(const int* p, int n) {
    int s = 0;
    for i in 0..n {
        s += p[i];
    }
    return s;
}

int main() {
    // ---------- &x / *p ----------
    int x = 10, y = 20;
    int* px = &x;
    print_int(*px); putchar('\n');   // 10
    *px = 15;
    print_int(x); putchar('\n');     // 15
    *px += 5;                        // 通过指针复合赋值
    print_int(x); putchar('\n');     // 20

    // ---------- 指针参数 ----------
    add_to(&x, 7);
    print_int(x); putchar('\n');     // 27
    swap(&x, &y);
    print_int(x); putchar(' ');      // 20
    print_int(y); putchar('\n');     // 27

    // ---------- 指针指向数组：p[i] 与指针算术 ----------
    int[4] arr = {5, 6, 7, 8};
    int* p = &arr[0];
    print_int(p[2]); putchar('\n');      // 7
    print_int(*(p + 3)); putchar('\n');  // 8
    p[0] += 1;                           // 指向数组本体，写穿透
    print_int(arr[0]); putchar('\n');    // 6

    // ---------- const int* ----------
    print_int(sum_readonly(&arr[0], 4)); putchar('\n');   // 6+6+7+8 = 27

    // ---------- alloc / free + null 检查 ----------
    int* buf = alloc(4 * sizeof(int)) as int*;
    if buf == null {
        puts("alloc failed");
        return 1;
    }
    for i in 0..4 {
        buf[i] = i * 3;
    }
    print_int(buf[3]); putchar('\n');    // 9
    print_int(sum_readonly(buf, 4)); putchar('\n');   // 0+3+6+9 = 18
    free(buf as void*);

    // ---------- null 指针 ----------
    int* empty = null;
    if empty == null {
        puts("null ok");
    }

    // ---------- 指向结构体：(*pp).field ----------
    Point pt = {1, 2};
    Point* pp = &pt;
    (*pp).x = 42;
    print_int(pt.x); putchar('\n');      // 42
    auto via_ptr = (*pp).y + 1;
    print_int(via_ptr); putchar('\n');   // 3

    return 0;
}
