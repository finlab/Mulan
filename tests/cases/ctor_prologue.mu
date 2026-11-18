// The construction prologue (SPEC 3.12): `: Base(args)` calls the base constructor before the members, a
// `: Ctor(args)` item delegates to another constructor of the same type, and `: field(v)` initializes an
// (often `const`) field. Execution order is fixed — delegate → base → members → field initializers in
// declaration order → body — regardless of how the list is spelled.
unit ctor_prologue;

extern int puts(const char* s);

class Base {
    int v;
    Base(int x) { v = x; }
    int base_value() const { return v; }
}

const class Derived : Base {
public:
    int w;
    // The list is spelled field-first on purpose: the base still runs first and the fields are
    // initialized in declaration order.
    Derived(int x) : w(x * 2), Base(x) { }
    int sum() const { return base_value() + w; }
}

class Cfg {
public:
    const int id;
    const int scale;
    Cfg(int i, int s) : id(i), scale(s) { }
    Cfg(int i) : id(i), scale(1) { }
    Cfg() : Cfg(7) { }          // delegating constructor
}

class Pair {
public:
    int a;
    int b;
    Pair(int x, int y) : b(y), a(x) { }   // declaration order decides: a is initialized first
    int order() const { return a * 10 + b; }
}

int main() {
    Derived d = new Derived(3);
    if d.sum() != 9 { puts("base + const field"); return 1; }
    if d.base_value() != 3 { puts("base value"); return 2; }

    Cfg c0 = new Cfg();
    Cfg c1 = new Cfg(2);
    Cfg c2 = new Cfg(2, 5);
    if c0.id != 7 or c0.scale != 1 { puts("delegating constructor"); return 3; }
    if c1.id != 2 or c1.scale != 1 { puts("one-argument constructor"); return 4; }
    if c2.id != 2 or c2.scale != 5 { puts("two-argument constructor"); return 5; }

    Pair p = new Pair(3, 4);
    if p.order() != 34 { puts("declaration order"); return 6; }

    puts("ctor prologue ok");
    return 0;
}


