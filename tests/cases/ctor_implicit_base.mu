unit audit212_implicit_base_ctor;
extern int puts(const char* s);
int live = 0;

class Base {
    int v;
    Base() { v = 42; live += 1; }
    int get() const { return v; }
}

class D : Base { }

int main() {
    D d = new D();
    if live != 1 { puts("base constructor not called"); return 1; }
    if d.get() != 42 { puts("base constructor did not run"); return 2; }
    puts("implicit base ctor ok");
    return 0;
}
