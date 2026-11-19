unit div_zero_check;

// Integer division / remainder checks (SPEC 8.2, `--bounds`): dividing or taking the remainder by a
// zero divisor, and `MIN / -1` / `MIN % -1`, throw std.exception.ArithmeticError instead of trapping.
// Divisor values come from `argc` so the optimizer cannot fold the branch away.

extern int printf(const char* fmt, ...);

int main(int argc, char** argv) {
    int ten = 10;
    int zero = argc - 1;          // 0 at run time
    int minus_one = argc - 2;     // -1 at run time
    int min_int = -2147483647 - 1;
    int caught = 0;

    try {
        printf("%d\n", ten / zero);
    } catch (e: ArithmeticError) {
        caught += 1;
        printf("div caught kind=%d value=%ld\n", e.kind, e.value);
    }
    try {
        printf("%d\n", ten % zero);
    } catch (e: ArithmeticError) {
        caught += 1;
        printf("mod caught kind=%d\n", e.kind);
    }
    try {
        printf("%d\n", min_int / minus_one);
    } catch (e: ArithmeticError) {
        caught += 1;
        printf("overflow caught kind=%d\n", e.kind);
    }
    try {
        printf("%d\n", min_int % minus_one);
    } catch (e: ArithmeticError) {
        caught += 1;
        printf("overflow mod caught kind=%d\n", e.kind);
    }

    // unsigned division by zero traps as well
    uint uzero = (argc - 1) as uint;
    uint u = 7u;
    try {
        printf("%u\n", u / uzero);
    } catch (e: ArithmeticError) {
        caught += 1;
        printf("unsigned caught kind=%d\n", e.kind);
    }

    // compound assignment takes the same path
    int acc = 100;
    try {
        acc /= zero;
        printf("unreachable\n");
    } catch (e: ArithmeticError) {
        caught += 1;
        printf("compound caught kind=%d\n", e.kind);
    }

    // normal division is unaffected (constant and variable divisors)
    if caught != 6 { printf("wrong catch count %d\n", caught); return 1; }
    printf("%d %d %d\n", ten / 3, ten % 3, ten / -3);
    int d = argc + 1;
    printf("%d %d\n", 100 / d, 100 % d);
    return 0;
}
