// An element constructor that throws while an aggregate is being built must not leave the rest of
// the aggregate alive: the parts already built are released, exactly once (SPEC 3.4).
unit agg_init_throw;

extern int puts(const char* s);

int live = 0;

class Boom : Exception {
    Boom(string m) : Exception(m) { }
}

class C {
    public int v;
    C(int x) {
        v = x;
        live = live + 1;
    }
    ~C() { live = live - 1; }
}

struct S {
    public C c;
    S(int v) {
        c = new C(v);
        if v == 2 { throw new Boom("element 2"); }
    }
}

struct Pair {
    public S a;
    public S b;
}

int main() {
    try {
        S[2] arr = {S(0), S(2)};
    } catch (e: Exception) {
        // expected
    }
    if live != 0 { puts("array aggregate leaked"); return 1; }

    try {
        Pair p = {a = S(0), b = S(2)};
    } catch (e: Exception) {
        // expected
    }
    if live != 0 { puts("struct aggregate leaked"); return 2; }

    try {
        S[] sl = {S(0), S(2)};
    } catch (e: Exception) {
        // expected
    }
    if live != 0 { puts("slice aggregate leaked"); return 3; }

    // The same shapes without a throw still build and release normally.
    {
        S[2] ok = {S(0), S(1)};
        if live != 2 { puts("array counts"); return 4; }
    }
    if live != 0 { puts("array release"); return 5; }

    {
        S[] ok = {S(0), S(1)};
        if live != 2 { puts("slice counts"); return 6; }
    }
    if live != 0 { puts("slice release"); return 7; }

    puts("agg init throw ok");
    return 0;
}
