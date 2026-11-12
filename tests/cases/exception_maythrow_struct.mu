unit exception_maythrow_struct;

extern int printf(const char* fmt, ...);

class E : Exception {
    E(string m) { super(m); }
}

struct S {
    int f() { throw new E("s"); }
    int g() { return 7; }
}

int main() {
    S s = S();
    int v = s.g();
    try {
        s.f();
    } catch (e: E) {
        printf("%d %d\n", v, e.message.length() as int);
    }
    return 0;
}
