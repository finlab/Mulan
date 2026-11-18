// `const class D : Base` is legal only with a mutable base; a base list cannot carry a `const` qualifier
// — the constness of a class is a property of its declaration, not of a view (SPEC 3.7).
unit base_const_qualifier;

class Base {
    public int v;
    Base(int x) { v = x; }
}

const class Leaf : const Base {
    Leaf() { super(1); }
}

int main() { return 0; }
