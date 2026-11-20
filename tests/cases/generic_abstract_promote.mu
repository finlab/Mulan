// generic_abstract_promote — a generic abstract class (a bodyless method) and a promoted method of a
// generic field: both resolve against the instantiated instance.
unit generic_abstract_promote;

extern int puts(const char* s);

class A<T> {
    public T v;
    A() {}
    T f();
}

class C<T> : A<T> {
    C() {}
    override T f() { return v; }
}

struct Inner<T> {
    public T v;
    Inner() {}
    T get() { return v; }
}

struct Outer<T> {
    public Inner<T> i;
    promote i;
    Outer() {}
}

int main() {
    C<int> c = new C<int>();
    c.v = 3;
    if c.f() != 3 { puts("abstract subclass"); return 1; }

    Outer<int> o = {};
    o.i.v = 4;
    if o.get() != 4 { puts("promoted method"); return 2; }

    puts("generic abstract promote ok");
    return 0;
}
