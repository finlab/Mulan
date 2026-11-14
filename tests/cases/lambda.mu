// lambda.mu — non-capturing lambdas: expression body, block body with an inferred return type,
// void body, argument position, overload target type, array element, and a C callback
unit lambda;

extern int puts(const char* s);
extern int putchar(int c);
extern void qsort(void* base, usize n, usize size, (const void*, const void*) => int cmp);

type Op = (int, int) => int;
type Pred = (int) => bool;

int apply(Op f, int a, int b) { return f(a, b); }

int twice(int x) { return x * 2; }

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
    // expression body; the type is `(int) => int`
    auto sq = (int x) => x * x;
    print_int(sq(7)); putchar('\n');                       // 49

    // block body: the return type is inferred from the `return` expressions
    auto clamp = (int x) => {
        if (x < 0) { return 0; }
        if (x > 100) { return 100; }
        return x;
    };
    print_int(clamp(-5)); putchar(' ');
    print_int(clamp(50)); putchar(' ');
    print_int(clamp(500)); putchar('\n');                  // 0 50 100

    // no `return <value>` → the lambda returns void
    auto show = (int v) => { putchar('['); print_int(v); putchar(']'); };
    show(42); putchar('\n');                               // [42]

    // argument position
    print_int(apply((int a, int b) => a * 10 + b, 3, 4)); putchar('\n');  // 34

    // a boolean lambda used by name
    Pred is_even = (int x) => x % 2 == 0;
    if (is_even(8)) { puts("even"); }

    // ternary expression body
    auto pick = (int a, int b) => a > b ? a : b;
    print_int(pick(3, 9)); putchar('\n');                  // 9

    // the same lambda used twice keeps one identity
    Op acc = (int a, int b) => a + b;
    Op acc2 = acc;
    if (acc == acc2) { puts("same"); }

    // array of lambdas
    Op[] ops = {(int a, int b) => a - b, (int a, int b) => b - a};
    print_int(ops[0](9, 4)); putchar(' '); print_int(ops[1](9, 4)); putchar('\n');  // 5 -5

    // C callback
    int[] ys = {8, 2, 6};
    qsort(&ys[0], ys.length(), sizeof(int),
          (const void* a, const void* b) => *(a as const int*) - *(b as const int*));
    print_int(ys[0]); putchar(' '); print_int(ys[1]); putchar(' '); print_int(ys[2]); putchar('\n');
    // 2 6 8

    // a lambda and a named function are interchangeable
    auto t = (int x) => twice(x) + 1;
    print_int(t(5)); putchar('\n');                        // 11
    return 0;
}
