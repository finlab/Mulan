// ref_param_write — assigning to a `ref` parameter writes the caller's variable, for every value type
// (the old rule allowed only scalars and refused a struct: "cannot rebind"). A `ref` cannot be
// rebound — the language has no references — so a whole-value assignment is a write through it. This
// is what an out-parameter and `std.mem.replace` need.
unit ref_param_write;

use std.cstdio;

int allocs = 0;
int frees = 0;

struct P {
    int x;
}

struct Buf {
    int* p;
    Buf() { p = null; }
    Buf(int n) { p = alloc(8) as int*; p[0] = n; allocs = allocs + 1; }
    ~Buf() {
        if (p != null) { frees = frees + 1; free(p); p = null; }
    }
}

void set_x(ref P p, int v) {
    P other = {};
    other.x = v;
    p = other;                 // replaces the caller's whole value
}

void refill(ref Buf b, int n) {
    b = Buf(n);                // the old value is released, the new one is moved in
}

int main() {
    P a = {};
    a.x = 1;
    set_x(a, 42);
    printf("%d\n", a.x);

    {
        Buf buf = Buf(5);
        refill(buf, 6);
        printf("%d\n", buf.p[0]);
    }
    printf("%d %d\n", allocs, frees);
    return 0;
}
