// A base list cannot carry a `const` qualifier: cv belongs to a view of an object, not to its
// inheritance (SPEC 3.7).
unit base_const_qualifier;

class Base {
    public int v;
    Base(int x) { v = x; }
}

class Leaf : const Base {
    Leaf() : Base(1) { }
}

int main() { return 0; }
