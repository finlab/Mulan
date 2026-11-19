unit iface_default_self;

// An interface default whose signature mentions `Self` is materialized per implementor, exactly like a
// default whose body reads `this` (SPEC 3.15.6): both forms have no itable slot. `Self` is substituted
// with the implementor, so `Self self_val() { return this; }` becomes `S1 self_val()` for a struct and
// `C self_val()` for a class.

use std.cstdio;
use std.string;

interface IVal {
    int core();
    Self self_val() { return this; }
    bool same_core(Self o) { return this.core() == o.core(); }
    Self dup(Self x) { return x; }
}

struct S1 : IVal {
public:
    int v;
    string tag;
    int core() { return v; }
}

class C1 : IVal {
public:
    int v;
    C1(int k) { v = k; }
    override int core() { return v; }
}

int main() {
    S1 s;
    s.v = 3;
    s.tag = string("s");
    S1 copy = s.self_val();
    printf("struct %d %s\n", copy.v, copy.tag.cstr());

    S1 other;
    other.v = 3;
    printf("same %d\n", s.same_core(other));
    other.v = 4;
    printf("diff %d\n", s.same_core(other));
    S1 dup = s.dup(other);
    printf("dup %d\n", dup.v);

    C1 c = new C1(7);
    C1 d = c.self_val();
    printf("class %d\n", d.v);
    C1 e = new C1(7);
    printf("class same %d\n", c.same_core(e));
    C1 f = c.dup(e);
    printf("class dup %d\n", f.v);
    return 0;
}
