unit compound_wrap_uint;
use std.cstdio;

// Unsigned wrap-around is defined behaviour and must stay legal: `uint + int` has a `uint` result,
// so the compound assignment is not a narrowing store.
int main() {
    uint u = 0;
    u += 1;
    if u == 1 {
        printf("add ok\n");
    }
    u -= 2;
    if u != 0 {
        printf("wrap ok\n");
    }
    return 0;
}
