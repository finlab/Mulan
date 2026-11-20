// uninit_flow — definite initialization of scalar locals (SPEC 4): a local declared without an
// initializer is usable once every path that reaches the read has assigned it. These programs are
// legal; the must-fail cases cover the rejected shapes.
unit uninit_flow;

use std.cstdio;

void fill(ref int slot) {
    slot = 7;                     // an out-parameter: the callee writes the caller's variable
}

int pick(bool c) {
    int y;
    if c {
        y = 1;
    } else {
        y = 2;
    }
    return y;                     // both paths assign
}

int main() {
    int x;
    x = 3;
    printf("%d\n", x);

    printf("%d %d\n", pick(true), pick(false));

    int m;
    fill(m);
    printf("%d\n", m);

    // a guard loop with the assignment before the only exit still counts (the loop may not run, so the
    // variable is assigned *before* the loop here)
    int n = 0;
    int acc;
    acc = 0;
    for i in 0..3 {
        acc = acc + i;
    }
    n = acc;
    printf("%d %d\n", n, acc);
    return 0;
}
