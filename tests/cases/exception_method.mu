unit exception_method;

extern int printf(const char* fmt, ...);

class Boom : Exception {
    Boom() { super("m"); }
}

interface I {
    int f();
}

class A : I {
    override int f() { throw new Boom(); }
}

class B {
    int f() { throw new Boom(); }
}

int main() {
    A a = new A();
    int r = 0;
    try {
        r = a.f();
    } catch (e: Boom) {
        r = 1;
    }

    B b = new B();
    try {
        r = r * 10 + b.f();
    } catch (e: Boom) {
        r = r * 10 + 2;
    }
    printf("%d\n", r);
    return 0;
}
