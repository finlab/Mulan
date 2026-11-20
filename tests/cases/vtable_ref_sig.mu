// vtable_ref_sig — a derived method differing from a base virtual method only in ref-ness is a
// different method; it must not take the base's vtable slot (#200).
unit vtable_ref_sig;

extern int puts(const char* s);

class A {
    virtual int m(int x) { return x; }
}

class B : A {
    int m(ref int x) {
        x = x + 1;
        return x;
    }
}

int main() {
    B b = new B();
    int v = 5;
    A a = b;
    int r = a.m(v);
    if r != 5 or v != 5 { puts("dispatch"); return 1; }
    puts("vtable ref sig ok");
    return 0;
}
