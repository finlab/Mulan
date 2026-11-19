unit self_default_probe;

use std.cstdio;

interface IVal {
    int core();
    Self self_val() { return this; }
    bool same_core(Self o) { return this.core() == o.core(); }
    Self dup(Self x) { return x; }
}

struct S1 : IVal {
public:
    int v;
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
    S1 s2 = s.self_val();
    printf("struct %d\n", s2.v);

    S1 s3;
    s3.v = 3;
    printf("same %d\n", s.same_core(s3));
    S1 s4 = s.dup(s3);
    printf("dup %d\n", s4.v);

    C1 c = new C1(7);
    C1 d = c.self_val();
    printf("class %d\n", d.v);
    C1 e = new C1(7);
    printf("class same %d\n", c.same_core(e));
    return 0;
}
