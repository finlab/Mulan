unit audit_m1_explicit_base_empty;
extern int puts(const char* s);
class A {
    int v;
    int get() const { return v; }
}
class B : A {
    B() : A() { }
    int result() const { return get() + 1; }
}
int main() {
    B b = new B();
    if b.result() != 1 { return 1; }
    puts("explicit empty base ok");
    return 0;
}

