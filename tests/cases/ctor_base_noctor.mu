unit audit212_base_noctor;
extern int puts(const char* s);

class A {
    int v;
    int get() const { return v; }
}

class B : A {
    int w;
    B() { w = 3; }
    int sum() const { return get() + w; }
}

int main() {
    B b = new B();
    if b.sum() != 3 { puts("base without ctor"); return 1; }
    puts("base without ctor ok");
    return 0;
}
