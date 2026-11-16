// slice_ctor_throw — a throwing element constructor during `new T[n]`: the elements built so far are
// destroyed, the block is freed, and the exception reaches the enclosing `try` (SPEC 3.14 partial
// construction, agents/value-semantics.md §7).
unit slice_ctor_throw;

extern int printf(const char* fmt, ...);

class Boom : Exception {
    Boom() { super("boom"); }
}

int live = 0;
int built = 0;
int dtors = 0;

struct S {
    int id;
    S() {
        built = built + 1;
        if built == 3 { throw new Boom(); }
        live = live + 1;
        id = built;
    }
    ~S() { dtors = dtors + 1; live = live - 1; }
}

int main() {
    try {
        S[] a = new S[5];
        printf("no throw\n");
        return 1;
    } catch (e: Boom) {
        printf("caught live=%d built=%d dtors=%d\n", live, built, dtors);
        if live != 0 { return 2; }
        if built != 3 { return 3; }
        if dtors != 2 { return 4; }
    }
    printf("slice_ctor_throw ok\n");
    return 0;
}
