unit generic_virtual;
use std.cstdio;

// A generic class's `virtual` method keeps its slot after instantiation: a subclass's `override` is
// accepted and dynamic dispatch reaches the most derived body. The clone table used to drop the flag.
class Base<T> {
    T v;
    Base(T x) { v = x; }
    virtual int f() { return 1; }
}

class Derived : Base<int> {
    Derived() { super(0); }
    override int f() { return 2; }
}

int main() {
    Base<int> b = new Derived();
    if b.f() != 2 {
        printf("static dispatch: %d\n", b.f());
        return 1;
    }
    printf("virtual ok\n");
    return 0;
}
