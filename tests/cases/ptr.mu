// ptr.mu — &x / *p, pointer parameters, p[i], alloc/free, null checks, const int*
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

// writes to the caller's variable through a pointer
void add_to(int* p, int d) {
    *p += d;
}

void swap(int* a, int* b) {
    int t = *a;
    *a = *b;
    *b = t;
}

// read-only pointer parameter: interface contract
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
    *px += 5;                        // compound assignment through the pointer
    print_int(x); putchar('\n');     // 20

    // ---------- pointer parameters ----------
    add_to(&x, 7);
    print_int(x); putchar('\n');     // 27
    swap(&x, &y);
    print_int(x); putchar(' ');      // 20
    print_int(y); putchar('\n');     // 27

    // ---------- pointer to an array: p[i] and pointer arithmetic ----------
    int[4] arr = {5, 6, 7, 8};
    int* p = &arr[0];
    print_int(p[2]); putchar('\n');      // 7
    print_int(*(p + 3)); putchar('\n');  // 8
    p[0] += 1;                           // points at the array itself, writes go through
    print_int(arr[0]); putchar('\n');    // 6

    // ---------- const int* ----------
    print_int(sum_readonly(&arr[0], 4)); putchar('\n');   // 6+6+7+8 = 27

    // ---------- alloc / free + null check ----------
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

    // ---------- null pointer ----------
    int* empty = null;
    if empty == null {
        puts("null ok");
    }

    // ---------- pointing at a struct: (*pp).field ----------
    Point pt = {1, 2};
    Point* pp = &pt;
    (*pp).x = 42;
    print_int(pt.x); putchar('\n');      // 42
    auto via_ptr = (*pp).y + 1;
    print_int(via_ptr); putchar('\n');   // 3

    return 0;
}
