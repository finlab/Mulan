unit for_c_style_cleanup;

// `break` / `continue` in a C-style `for` still run the surrounding `finally` bodies and release the
// locals of the iteration (a `continue` releases them before the step clause runs).

use std.cstdio;

int g_live = 0;

struct Cnt {
    int id;
    Cnt() { id = 0; g_live = g_live + 1; }
    ~Cnt() { g_live = g_live - 1; }
}

int main() {
    // i=0: +1, +10 -> 11; i=1: 12, +10 -> 22; i=2: break, finally +10 -> 32
    int n = 0;
    for (int i = 0; i < 4; i++) {
        try {
            if (i == 2) { break; }
            n = n + 1;
        } finally {
            n = n + 10;
        }
    }
    printf("%d\n", n);

    // every iteration's local is released, including the ones exited through `continue`
    for (int i = 0; i < 5; i++) {
        Cnt c = Cnt();
        if (i % 2 == 0) { continue; }
    }
    printf("live=%d\n", g_live);
    return 0;
}
