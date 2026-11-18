// A `const class` method is implicitly `const`; a mutating interface method cannot be satisfied by it.
// The diagnostic names the direction instead of "override does not match anything".
unit const_class_mutating_iface;

interface Counter { void bump(); }

const class C : Counter {
public:
    int v;
    C() : v(0) { }
    override void bump() { }
}

int main() {
    C c = new C();
    Counter k = c;
    k.bump();
    return 0;
}
