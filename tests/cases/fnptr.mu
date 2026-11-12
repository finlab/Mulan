// fnptr.mu — function types `(T1, T2) => R`: function values, indirect calls, null, identity, fields,
// arrays of function values, and a function value handed to a C API (qsort)
unit fnptr;

extern int puts(const char* s);
extern int putchar(int c);
extern void qsort(void* base, usize n, usize size, (const void*, const void*) => int cmp);

type Op = (int, int) => int;

int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

// unit-level function values are address constants
const Op TABLE_ADD = add;
Op TABLE_MUL = mul;

struct Cell {
    Op op;
    int seed;
}

int apply(Op f, int a, int b) { return f(a, b); }

int ascending(const void* a, const void* b) {
    return *(a as const int*) - *(b as const int*);
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

int main() {
    // a free function name is already a function value (no `&`)
    Op f = add;
    print_int(f(3, 4)); putchar('\n');            // 7
    f = mul;
    print_int(f(3, 4)); putchar('\n');            // 12
    print_int(apply(add, 10, 5)); putchar('\n');  // 15

    // null and identity
    Op g = null;
    if (g == null) { puts("null"); }
    g = add;
    if (g != null) { puts("set"); }
    if (g == add) { puts("eq"); }
    if (g == mul) { puts("bad"); } else { puts("ne"); }

    // the type is one word
    print_int(sizeof(Op) as int); putchar('\n');  // 8

    // function value stored in a struct field
    Cell c;
    c.op = mul;
    c.seed = 6;
    print_int(c.op(c.seed, 7)); putchar('\n');    // 42

    // arrays of function values
    Op[] ops = {add, mul};
    print_int(ops[0](1, 2) + ops[1](3, 4)); putchar('\n');  // 15

    // C interop: qsort with a named comparator
    int[] xs = {5, 3, 9, 1, 4};
    qsort(&xs[0], len(xs), sizeof(int), ascending);
    print_int(xs[0]); putchar(' '); print_int(xs[4]); putchar('\n');  // 1 9

    // C interop: qsort with a lambda comparator
    int[] ys = {2, 8, 6};
    qsort(&ys[0], len(ys), sizeof(int),
          (const void* a, const void* b) => *(b as const int*) - *(a as const int*));
    print_int(ys[0]); putchar(' '); print_int(ys[2]); putchar('\n');  // 8 2

    // unit-level function values and calls through them
    print_int(TABLE_ADD(2, 3) + TABLE_MUL(2, 3)); putchar('\n');      // 11

    // an `extern` function is a function value too
    auto out = putchar;
    out('!'); out('\n');                                              // !
    return 0;
}
