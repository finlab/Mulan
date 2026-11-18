// A `const class` is a leaf: it can never be a base class, neither as `class E : D` nor as
// `const class D : const Base` (the latter is the same rule: `const Base` is a `const class`).
unit const_class_as_base;

const class Base {
    public int v;
    Base(int x) : v(x) { }
}

class Derived : Base {
    Derived() : Base(1) { }
}

int main() { return 0; }
