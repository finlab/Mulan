// iface_const_default — a const interface default method keeps its const/ref signature through
// materialization and itable dispatch, and const/non-const overloads are distinct slots (#165, #198,
// #199).
unit iface_const_default;

extern int puts(const char* s);

interface I {
    int f() const { return 2; }
}

class C : I { }

interface ICounter {
    int get() const { return 1; }
}

struct S : ICounter { }

interface IOver {
    int f() { return 1; }
    int f() const { return 2; }
}

class COver : IOver { }

int main() {
    C c = new C();
    I i = c;
    if i.f() != 2 { puts("interface value"); return 1; }

    {
        const S s = S();
        if s.get() != 1 { puts("const receiver"); return 2; }
    }
    {
        S s = S();
        if s.get() != 1 { puts("mutable receiver"); return 3; }
    }

    COver co = new COver();
    const COver cco = new COver();
    if co.f() != 1 { puts("mutable overload"); return 4; }
    if cco.f() != 2 { puts("const overload"); return 5; }

    puts("iface const default ok");
    return 0;
}
