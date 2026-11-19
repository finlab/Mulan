unit para_captured_method_write;

// A `para` section borrows the suspended caller's frame: a non-const method on a captured value
// would race with the other sections. Only `const` methods are allowed there.

struct S {
    int v;
    void poke() { v = 1; }
    int read() const { return v; }
}

int main() {
    S s = {};
    para { { s.poke(); } { s.read(); } }
    return 0;
}
