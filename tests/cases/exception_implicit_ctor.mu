// exception_implicit_ctor — an `implicit` constructor runs at the conversion site; a throw inside it
// must reach the caller's `catch` when the conversion sits inside another function.
unit exception_implicit_ctor;

extern int printf(const char* fmt, ...);

class Boom : Exception {
    Boom() : Exception("m") { }
}

struct S {
    int v;
    implicit S(int x) { throw new Boom(); }
}

int convert_via() {
    S s = 3;
    return s.v;
}

int main() {
    int r = 0;
    try {
        r = convert_via();
    } catch (e: Boom) {
        r = 1;
    }
    printf("caught %d\n", r);
    return 0;
}
