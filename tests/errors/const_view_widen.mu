// A read-only view never widens to a mutable one implicitly: a `const` handle and any expression of a
// `const class` both need `as` (SPEC 3.6, 3.7).
unit const_view_widen;

class Base {
    public int v;
    Base(int x) { v = x; }
    void poke() { v = v + 1; }
}

const class Leaf : Base {
    Leaf() { super(1); }
}

int main() {
    Leaf l = new Leaf();
    Base b = l;          // read-only view -> mutable base handle
    return 0;
}
