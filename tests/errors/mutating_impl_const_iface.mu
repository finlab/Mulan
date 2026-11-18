// A read-only interface member must be implemented by a `const` method.
unit mutating_impl_const_iface;

interface Sized { int size() const; }

class C : Sized {
public:
    int v;
    C() { v = 1; }
    override int size() { v += 1; return v; }
}

int main() {
    C c = new C();
    Sized s = c;
    return s.size() - 1;
}
