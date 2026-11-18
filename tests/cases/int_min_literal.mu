// int_min_literal — the lowest value of each integer type is writable in the natural spelling.
// `2147483648` is typed as `long` on its own, but under a unary minus targeting `int` it denotes
// INT32_MIN (C/C++/Java rule); a hex literal that does not fit still converts with `as`.
unit int_min_literal;

use std.cstdio;

const int MIN = -2147483648;

int main() {
    int a = -2147483648;
    if a != MIN { printf("int min\n"); return 1; }
    if a != -2147483647 - 1 { printf("int min identity\n"); return 2; }
    int h = 0x80000000 as int;
    if h != a { printf("hex min\n"); return 3; }
    if a + 1 != -2147483647 { printf("increment\n"); return 4; }
    printf("int min ok\n");
    return 0;
}
