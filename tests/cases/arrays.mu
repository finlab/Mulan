// arrays.mu — int[N] aggregate init with zero fill, len, for-in value binding vs ref binding
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

// Array parameter is a copy: changes inside the function do not propagate back
int sum5(int[5] xs) {
    int s = 0;
    for i in 0..5 {
        s += xs[i];
    }
    xs[0] = 999;    // affects the copy only
    return s;
}

int main() {
    // ---------- Aggregate init; omitted elements are zero-filled ----------
    int[5] a = {1, 2, 3};
    print_int(a[0]); putchar(' ');   // 1
    print_int(a[2]); putchar(' ');   // 3
    print_int(a[3]); putchar(' ');   // 0 (zero-filled)
    print_int(a[4]); putchar('\n');  // 0 (zero-filled)

    // ---------- Arrays are value types: copies are independent ----------
    int[5] b = a;
    b[0] = 100;
    print_int(a[0]); putchar(' ');   // 1
    print_int(b[0]); putchar('\n');  // 100

    // ---------- Argument copy ----------
    print_int(sum5(a)); putchar('\n');   // 1+2+3+0+0 = 6
    print_int(a[0]); putchar('\n');      // 1 (not changed to 999)

    // ---------- for v in arr: value binding, writing v does not affect the array ----------
    for v in a {
        v = 0;
    }
    print_int(a[1]); putchar('\n');      // 2

    // ---------- Minimal form: declare first, fill element by element, then read ----------
    int[5] c;
    for i in 0..5 {
        c[i] = i * i;
    }
    int total = 0;
    for ref v in c {        // ref binding: elements are mutable
        total += v;
        v += 10;
    }
    // total = 0+1+4+9+16 = 30; c becomes 10 11 14 19 26
    print_int(total); putchar('\n');
    for v in c {
        print_int(v); putchar(' ');
    }
    putchar('\n');

    // ---------- len(arr) is the array length N ----------
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
