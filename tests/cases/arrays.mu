// arrays.mu — int[N] aggregate init with zero fill, C99 array-to-pointer decay, index loops
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

// An array parameter is a pointer (C99 6.7.5.3p7): `int[5] xs` is `int* xs`, so the bound is not
// part of the type and the callee shares the caller's storage.
int sum5(int[5] xs) {
    int s = 0;
    for i in 0..5 {
        s += xs[i];
    }
    return s;
}

// Writing through the decayed parameter reaches the caller's array.
void set_first(int[5] xs, int v) {
    xs[0] = v;
}

int main() {
    // ---------- Aggregate init; omitted elements are zero-filled ----------
    int[5] a = {1, 2, 3};
    print_int(a[0]); putchar(' ');   // 1
    print_int(a[2]); putchar(' ');   // 3
    print_int(a[3]); putchar(' ');   // 0 (zero-filled)
    print_int(a[4]); putchar('\n');  // 0 (zero-filled)

    // ---------- Decay: a pointer shares the array's storage ----------
    int* p = a;
    p[0] = 10;
    print_int(a[0]); putchar(' ');              // 10
    print_int(p[1]); putchar(' ');              // 2
    print_int(sizeof(p) as int); putchar('\n'); // 8 (a pointer, not the array)
    print_int((sizeof(a) / sizeof(a[0])) as int); putchar('\n');  // 5 (no decay under sizeof)

    // ---------- Argument decay: the callee shares the array ----------
    print_int(sum5(a)); putchar('\n');   // 10+2+3+0+0 = 15
    set_first(a, 7);
    print_int(a[0]); putchar('\n');      // 7 (written through the parameter)

    // ---------- a value binding is a copy: writing it does not affect the array ----------
    for i in 0..sizeof(a) / sizeof(a[0]) {
        int v = a[i];
        v = 0;
    }
    print_int(a[1]); putchar('\n');      // 2

    // ---------- Minimal form: declare first, fill element by element, then read ----------
    int[5] c;
    for i in 0..5 {
        c[i] = i * i;
    }
    int total = 0;
    for i in 0..5 {         // ref binding: elements are mutable
        total += c[i];
        c[i] += 10;
    }
    // total = 0+1+4+9+16 = 30; c becomes 10 11 14 19 26
    print_int(total); putchar('\n');
    for i in 0..5 {
        print_int(c[i]); putchar(' ');
    }
    putchar('\n');

    // ---------- sizeof(a) / sizeof(a[0]) is the element count ----------
    auto n = sizeof(a) / sizeof(a[0]);
    if n == 5 {
        puts("len ok");
    }
    int s2 = 0;
    for i in 0..sizeof(a) / sizeof(a[0]) {
        s2 += a[i];
    }
    print_int(s2); putchar('\n');    // 7+2+3+0+0 = 12

    return 0;
}
