unit class_forward_base;
extern int puts(readonly char* s);
class Derived : Base {
    int d;
    Derived() { super(); d = 2; }
    ~Derived() { }
    int total() { return base_v() + d; }
}
class Base {
    int b;
    Base() { b = 1; }
    ~Base() { }
    int base_v() { return b; }
}
int main() {
    Derived x = new Derived();
    if x.total() != 3 { puts("forward base fail"); return 1; }
    del x;
    puts("forward base ok");
    return 0;
}

