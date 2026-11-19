unit rvalue_assign;

// Assigning a prvalue to a struct that owns a resource: `a = T(args);` and `a = f();` move the value
// in place (the rhs is a fresh object, so this is a move-assign, not a copy). The old value is
// destroyed, the new one is owned by the destination — no leak, no double release.

use std.cstdio;
use std.string;

int g_live = 0;

struct Tagged {
    string tag;
    Tagged(const char* t) { tag = string(t); g_live = g_live + 1; }
    ~Tagged() { g_live = g_live - 1; }
}

nocopy struct MoveOnly {
    int v;
    MoveOnly(int k) { v = k; }
    MoveOnly(move MoveOnly other) { v = other.v; other.v = -1; }
    ~MoveOnly() { }
}

Tagged make(const char* s) { Tagged t = Tagged(s); return move t; }

int main() {
    Tagged a = Tagged("one");
    a = Tagged("two");
    printf("%s\n", a.tag.cstr());
    a = make("three");
    printf("%s\n", a.tag.cstr());

    MoveOnly m = MoveOnly(1);
    m = MoveOnly(9);
    printf("%d\n", m.v);

    int i = 0;
    while (i < 5000) {
        a = Tagged("loop");
        a = make("loop2");
        i = i + 1;
    }
    printf("%s\n", a.tag.cstr());
    printf("live=%d\n", g_live);
    return 0;
}
