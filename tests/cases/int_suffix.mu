unit int_suffix;

// C-style integer suffixes fix the literal's type: `u` = uint, `l` = long, `ul` = ulong. The `ul`
// form allows the full unsigned 64-bit range, which an unsuffixed literal cannot express.

use std.cstdio;

int main() {
    ulong a = 18446744073709551615ul;
    uint b = 4294967295u;
    long c = 9223372036854775807l;
    ulong d = 0xFFul;
    ulong e = 7UL;
    printf("%llu %u %lld %llu %llu\n", a, b, c, d, e);
    return a == 18446744073709551615ul and b == 4294967295u and c > 0 and d == 255ul and e == 7ul ? 0 : 1;
}
