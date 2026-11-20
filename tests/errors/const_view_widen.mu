// A read-only view never widens to a mutable one implicitly: a `const` handle needs `as`
// (SPEC 3.6, 3.7).
unit const_view_widen;

class Base {
    public int v;
    Base(int x) { v = x; }
    void poke() { v = v + 1; }
}

int main() {
    const Base cb = new Base(1);
    Base b = cb;          // read-only view -> mutable handle
    return 0;
}
