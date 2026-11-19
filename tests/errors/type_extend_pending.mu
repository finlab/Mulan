unit type_extend_pending;

interface I {
    void f();
}

struct S {
    void f() {}
}

type S += I;

int main() { return 0; }
