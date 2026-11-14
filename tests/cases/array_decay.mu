// array_decay.mu — C99 array-to-pointer decay: initializer, argument, struct field, row, sizeof
unit array_decay;

extern int puts(const char* s);
extern int printf(const char* fmt, ...);

struct Box { int[3] v; }

int sum(int* p, int n) {
    int s = 0;
    for i in 0..n { s += p[i]; }
    return s;
}

// A parameter declared as an array is a pointer (C99 6.7.5.3p7): `sizeof` is the pointer size,
// and writing through it reaches the caller's array.
int first_or_set(int[3] xs, int v) {
    int old = xs[0];
    xs[0] = v;
    return old + (sizeof(xs) as int);
}

int main() {
    int[3] a = {1, 2, 3};

    // decay in a declaration initializer; the pointer shares the array's storage
    int* p = a;
    if p[1] != 2 { puts("init decay"); return 1; }
    p[1] = 20;
    if a[1] != 20 { puts("shared storage"); return 2; }

    // decay in an assignment and in an argument
    int* q = null;
    q = a;
    if q[1] != 20 { puts("assign decay"); return 3; }
    if sum(a, 3) != 24 { puts("argument decay"); return 4; }

    // an array-typed parameter is `int*`: writes propagate, sizeof is 1 word
    if first_or_set(a, 9) != 1 + 8 { puts("parameter decay"); return 5; }
    if a[0] != 9 { puts("parameter write"); return 6; }

    // a struct field of array type decays the same way
    Box b;
    b.v[0] = 5;
    int* r = b.v;
    if r[0] != 5 { puts("field decay"); return 7; }
    r[0] = 6;
    if b.v[0] != 6 { puts("field share"); return 8; }

    // a row of a static 2-D array decays to a pointer to its first element
    int[2,3] m = {{1, 2, 3}, {4, 5, 6}};
    int* row = m[1];
    if row[2] != 6 { puts("row decay"); return 9; }
    row[2] = 66;
    if m[1,2] != 66 { puts("row share"); return 10; }

    // no decay under sizeof: the array keeps its size, the pointer its own
    if sizeof(a) != 12 { puts("sizeof array"); return 11; }
    if (sizeof(p) as int) != 8 { puts("sizeof pointer"); return 12; }

    // a variadic argument decays too: the C side sees a `char*`, not a copied aggregate
    char[6] s = "hello";
    printf("<%s>\n", s);

    puts("array decay ok");
    return 0;
}
