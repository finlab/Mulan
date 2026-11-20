// ctor_member_init — the initializer list is **direct initialization** in declaration order
// (SPEC 3.12): `: a(x)` calls the field's constructor with `x` (no `implicit` needed), a field with a
// default constructor and an explicit initializer is constructed exactly once, and a throw in the
// middle destroys only the fields that finished.
unit ctor_member_init;

use std.cstdio;

int made = 0;
int destroyed = 0;

class Boom : Exception {
    Boom() : Exception("boom") { }
}

struct E {
    int id;
    E(int n) {
        made = made + 1;
        id = n;
        if n == 2 { throw new Boom(); }
    }
    ~E() { destroyed = destroyed + 1; }
}

struct Pair {
    E a;
    E b;
    Pair(int x) : a(x), b(x + 1) { }       // direct initialization, declaration order
}

struct Thrower {
    E a;
    E b;
    Thrower(int x) : a(1), b(2) { }        // b's constructor throws
}

struct D {
    int id;
    D() { made = made + 1; id = 0; }
    D(int n) { made = made + 1; id = n; }
}

struct Once {
    D d;
    Once() : d(7) { }                      // D has a default constructor *and* an initializer here
}

int main() {
    {
        Pair p = Pair(10);                 // a = 10, b = 11, one construction each
        printf("%d %d %d\n", p.a.id, p.b.id, made);
    }
    printf("pair destroyed %d\n", destroyed);

    int before = made;
    {
        Once o = Once();
        printf("once %d %d\n", o.d.id, made - before);   // exactly one construction
    }

    // a throw in the middle of the member list destroys the finished field only
    destroyed = 0;
    int caught = 0;
    try {
        Thrower t = Thrower(0);
    } catch (e: Boom) {
        caught = 1;
    }
    printf("throw caught %d destroyed %d\n", caught, destroyed);
    return 0;
}
