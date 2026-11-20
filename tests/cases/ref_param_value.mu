// ref_param_value — a `ref` parameter may borrow any non-union value type, including one that owns a
// resource or holds reference-counted fields. A borrow is scoped (it cannot be stored or rebound), so
// the callee reaches the object only through the language's own rules; `&` stays refused for such a
// type because a raw pointer outlives the borrow.
unit ref_param_value;

use std.cstdio;

int live = 0;

class Cat {
public:
    int age;
    Cat(int a) { age = a; live = live + 1; }
    ~Cat() { live = live - 1; }
}

struct Box {
    Cat c;
    int n;
}

struct Owned {
    int* p;
    Owned(int v) { p = alloc(8) as int*; p[0] = v; }
    ~Owned() { if (p != null) { free(p); p = null; } }
}

void bump(ref Box b) {
    b.n = b.n + 1;
    b.c.age = 9;                 // through the borrow: the same Cat, no copy
}

void refill(ref Owned o, int v) {
    o.p[0] = v;
}

int main() {
    Cat c = new Cat(1);
    Box b = {c, 41};
    bump(b);
    printf("%d %d\n", b.n, b.c.age);
    printf("%d\n", live);        // one Cat: the borrow copied nothing

    Owned o = Owned(5);
    refill(o, 77);
    printf("%d\n", o.p[0]);
    return 0;
}
