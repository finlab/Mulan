// float_arith.mu — float/double arithmetic, conversions and variadic promotion
//
// The only float coverage before was `floatlit` (literal parsing) and `dbgvars`
// (debug info); this case locks the arithmetic itself.
unit float_arith;

use std.cstdio;

int main() {
    // ---- arithmetic in double ----
    double d = 2.25;
    double sum = 1.5 + d;
    double diff = 5.0 - 1.25;
    double prod = 1.5 * 2.0;
    double quot = 7.0 / 2.0;
    printf("double %.2f %.2f %.2f %.2f\n", sum, diff, prod, quot);

    // ---- float arithmetic ----
    float f = 1.5;
    float g = 2.5;
    float h = f + g;
    printf("float %.1f\n", h);

    // ---- a float argument of a variadic call is promoted to double ----
    printf("promo %.3f %.3f\n", f, d);

    // ---- `as` truncates toward zero ----
    int t = 3.99 as int;
    int t2 = -3.99 as int;
    uint tu = 2.9 as uint;
    printf("trunc %d %d %u\n", t, t2, tu);

    // ---- a float keeps 32 bits, a double keeps 64 ----
    float f32 = 0.1 as float;
    double d64 = 0.1;
    printf("prec %.9f %.9f\n", f32 * 3.0, d64 * 3.0);

    // ---- widening a float to a double keeps the 32-bit value ----
    double wide = f32;
    printf("wide %.9f\n", wide);

    puts("float arith ok");
    return 0;
}
