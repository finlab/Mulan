unit class_stress3;
extern int puts(const char* s);
class A {
    int base_val;
    A(int v) { base_val = v; }
    A() { base_val = 0; }
    ~A() { }
    int f(int x) { return x + base_val; }
    int g() { return base_val; }
}
class B : A {
    int own;
    B(int v, int o) { super(v); own = o; }
    ~B() { }
    int f(string s) { return len(s) as int + own; }
    int call_super(int x) { return super.f(x); }
    int g2() { return g() + own; }
}
int main() {
    B b = new B(10, 20);
    if b.f(5) != 15 { puts("base overload fail"); return 1; }
    if b.f("abc") != 23 { puts("derived overload fail"); return 1; }
    if b.call_super(7) != 17 { puts("super call fail"); return 1; }
    if b.g2() != 30 { puts("inherited method fail"); return 1; }
    A a = new A(3);
    B b2 = new B(1, 2);
    A up = b2;
    if up.f(1) != 2 { puts("virtual base overload fail"); return 1; }
    del a;
    del b2;
    del up;
    del b;
    puts("stress3 ok");
    return 0;
}




