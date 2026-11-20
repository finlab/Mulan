// ref_method — a struct or class method may take `ref` / `const ref` parameters (SPEC 3.8, extended
// from free functions). The borrow is part of the method's signature, the callee receives an address,
// and a mutable borrow may initialize the caller's variable (an out-parameter).
unit ref_method;

use std.cstdio;

struct Counter {
    int base;

    void add_to(ref int x) { x = x + base; }
    void write(int n, ref int out) { out = n + base; }      // a pure out-parameter
    int read_only(const ref int x) { return x + base; }
}

class Box {
public:
    int factor;

    void scale(ref int x) { x = x * factor; }
    void fill(int n, ref int out) { out = n * factor; }
}

int main() {
    Counter c = {3};
    int a = 5;
    c.add_to(a);
    printf("%d\n", a);

    int out;
    c.write(2, out);                 // `out` is initialized by the call
    printf("%d\n", out);

    const int k = 4;
    printf("%d\n", c.read_only(k));  // a const place binds to `const ref`

    Box b = new Box();
    b.factor = 4;
    int v = 10;
    b.scale(v);
    int produced;
    b.fill(3, produced);
    printf("%d %d\n", v, produced);

    // a field is an addressable place too
    Counter d = {1};
    d.add_to(c.base);
    printf("%d\n", c.base);
    return 0;
}
