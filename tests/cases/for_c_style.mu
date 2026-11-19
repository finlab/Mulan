unit for_c_style;

// C-style `for (init; cond; step)`: every clause is optional, `continue` runs the step clause
// (C semantics), and the loop variable of the init declaration is visible in the condition and body.
// The for-in forms in the same file must keep parsing (the head is told apart by lookahead).

use std.cstdio;

int main() {
    int sum = 0;
    for (int i = 0; i < 5; i++) { sum = sum + i; }
    printf("%d\n", sum);

    // continue must run the step, otherwise this would not terminate
    int odds = 0;
    for (int i = 0; i < 10; i++) {
        if (i % 2 == 0) { continue; }
        odds = odds + i;
    }
    printf("%d\n", odds);

    // empty init, expression init, empty step
    int j = 0;
    for (; j < 3; j += 1) { }
    printf("%d\n", j);

    int k = 0;
    for (k = 10; k > 7; ) { k = k - 1; }
    printf("%d\n", k);

    // `for (;;)` leaves only through break / return
    int n = 0;
    for (;;) {
        n = n + 1;
        if (n == 4) { break; }
    }
    printf("%d\n", n);

    // several declarators in the init clause
    int last = 0;
    for (int a = 1, b = 5; a < b; a++) { last = a; }
    printf("%d\n", last);

    // nested loops: break and continue bind to the innermost one
    int hits = 0;
    for (int i = 0; i < 3; i++) {
        for (int q = 0; q < 5; q++) {
            if (q == 1) { continue; }
            if (q == 4) { break; }
            hits = hits + 1;
        }
    }
    printf("%d\n", hits);

    // a for-in inside the body still uses the for-in shape
    int total = 0;
    for (int r = 0; r < 2; r++) {
        for v in 1..3 { total = total + v; }
    }
    printf("%d\n", total);
    return 0;
}
