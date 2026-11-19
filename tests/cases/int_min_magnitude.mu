unit int_min_magnitude;

// The minimum of a signed 64-bit type has no positive literal spelling: `-9223372036854775808` is
// the magnitude 2^63 negated. Only a directly applied unary `-` accepts that magnitude (C rule).

use std.cstdio;

int main() {
    long a = -9223372036854775808;
    long b = -9223372036854775808l;
    printf("%lld\n", a);
    printf("%d\n", a == b);
    printf("%lld\n", -9223372036854775808);
    printf("%d\n", -9223372036854775807 - 1 == a);
    printf("%d\n", a < 0 and a < -9223372036854775807);
    return 0;
}
