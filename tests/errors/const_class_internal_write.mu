// Construction is the only writable phase of a `const class`: a method body is read-only, and that
// includes the mutable base subobject (SPEC 3.7).
unit const_class_internal_write;

class Base {
    public int v;
    Base(int x) { v = x; }
    virtual void poke() { v = v + 1; }
}

const class Leaf : Base {
    Leaf() { super(10); }
    void bump() { v = v + 5; }
}

int main() { return 0; }
