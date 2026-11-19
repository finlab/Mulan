unit for_c_style_infinite;

// `for (;;)` never falls out on its own: a body without `break` cannot reach the end, so a non-void
// function does not need a trailing `return` after it.

use std.cstdio;

int first_positive(int from) {
    for (;;) {
        if (from > 0) { return from; }
        from = from + 1;
    }
}

int count_until_break(int limit) {
    int n = 0;
    for (;;) {
        n = n + 1;
        if (n == limit) { break; }
    }
    // reachable: the loop above can leave through `break`, so this return is required and used
    return n;
}

int main() {
    printf("%d %d\n", first_positive(-3), count_until_break(4));
    return 0;
}
