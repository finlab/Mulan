// exception_member_op — a member `operator+` / unary `operator-` is a method call: a throw inside it
// must reach the caller's `catch` when the operator is used inside another function. Free operators
// were already followed; member ones were not, so the wrapper counted as nothrow.
unit exception_member_op;

extern int printf(const char* fmt, ...);

class Boom : Exception {
    Boom() : Exception("m") { }
}

struct S {
    int v;
    S operator+(S o) { throw new Boom(); }
    S operator-() { throw new Boom(); }
}

int add_via(S a, S b) {
    S c = a + b;
    return c.v;
}

int neg_via(S a) {
    S c = -a;
    return c.v;
}

int main() {
    S a = {};
    S b = {};
    int r = 0;
    try {
        r = add_via(a, b);
    } catch (e: Boom) {
        r = r + 1;
    }
    try {
        r = r + neg_via(a);
    } catch (e: Boom) {
        r = r + 2;
    }
    printf("caught %d\n", r);
    return 0;
}
