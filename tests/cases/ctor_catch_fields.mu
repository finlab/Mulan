// ctor_catch_fields — an exception caught inside a constructor body must not release the fields the
// constructor already built; the object continues and is fully usable (#226).
unit ctor_catch_fields;

use std.string;

extern int puts(const char* s);

class Boom : Exception {
    Boom() { }
}

class C {
    public string tag;
    public int n;
    C() {
        tag = "hello";
        n = 42;
        try { throw new Boom(); } catch (e: Exception) { }
    }
}

struct S {
    public string tag;
    public int n;
    S() {
        tag = "world";
        n = 7;
        try { throw new Boom(); } catch (e: Exception) { }
    }
}

int main() {
    C c = new C();
    if c.tag.length() != 5 or c.n != 42 { puts("class"); return 1; }
    S s;
    if s.tag.length() != 5 or s.n != 7 { puts("struct"); return 2; }
    puts("ctor catch fields ok");
    return 0;
}
