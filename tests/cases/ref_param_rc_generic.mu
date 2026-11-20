// ref_param_rc_generic — a generic `ref T` parameter may borrow a reference-holding concrete type.
// This case used to be a must-fail: the rule rejected every borrow of such a value, with a note that
// allowing it had once corrupted the heap. The borrow itself is scoped (it cannot be stored or
// rebound), so the callee reaches the object only through the language rules; the loop below checks
// that counting stays exact over many calls, which is what that corruption showed up as.
unit ref_param_rc_generic;

use std.cstdio;

int live = 0;

class C {
    C() { live = live + 1; }
    ~C() { live = live - 1; }
}

struct Box {
    int[] data;
    C c;
}

void touch<T>(ref T x) { }

void set_first(ref Box b, int v) { b.data[0] = v; }

int main() {
    int total = 0;
    for k in 0..200 {
        Box b = {new int[4], new C()};
        b.data[0] = k;
        touch<Box>(b);
        set_first(b, k + 1);
        total = total + b.data[0];
    }
    printf("%d %d\n", total, live);
    return 0;
}
