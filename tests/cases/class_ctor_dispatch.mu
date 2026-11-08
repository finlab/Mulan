unit class_ctor_dispatch;
extern int puts(readonly char* s);
class A {
    int seen;
    A() { seen = probe(); }
    ~A() { }
    int probe() { return 1; }
    int get_seen() { return seen; }
}
class B : A {
    int marker;
    B() { marker = 42; }
    ~B() { }
    override int probe() { return marker; }
}
int main() {
    B b = new B();
    int s = b.get_seen();
    del b;
    if s != 1 { puts("ctor dispatch not C++-like"); return 1; }
    puts("ctor dispatch ok");
    return 0;
}

