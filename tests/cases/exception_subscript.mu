// exception_subscript — a throw inside a user `operator[]` reaches the caller's `catch` even when the
// call sits inside another function. The may-throw analysis follows a member subscript like any other
// method call; missing it classified the wrapper as nothrow and silently dropped the exception.
unit exception_subscript;

extern int printf(const char* fmt, ...);

class Boom : Exception {
    Boom() : Exception("m") { }
}

struct S {
    int operator[](isize i) { throw new Boom(); }
}

class C {
    int operator[](isize i) { throw new Boom(); }
}

int via_struct(S s) { return s[0]; }

int via_class(C c) { return c[0]; }

int main() {
    S s = {};
    C c = new C();
    int r = 0;
    try {
        r = via_struct(s);
    } catch (e: Boom) {
        r = r + 1;
    }
    try {
        r = r + via_class(c);
    } catch (e: Boom) {
        r = r + 2;
    }
    printf("caught %d\n", r);
    return 0;
}
