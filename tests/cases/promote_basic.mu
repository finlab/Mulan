// promote_basic — `promote field;` forwards the field's public methods (SPEC 3.19): a struct field,
// a class field, a generic wrapper, the whitelist form, and the rule that a real method wins.
unit promote_basic;

extern int printf(const char* fmt, ...);

struct P {
    int v;
    public int get() { return v; }
    public void set(int x) { v = x; }
}

class Box<T> {
    private T _value;
    public Box(T value) { _value = move value; }
    promote _value;
}

class Inner {
    public int v;
    public int get() { return v; }
}

class Holder {
    private Inner _i;
    public Holder(Inner i) { _i = i; }
    promote _i;
}

class Named {
    private P _p;
    public Named(P p) { _p = move p; }
    promote _p(get);
    public int get() { return 100; }  // a real method wins over the promoted one
}

int main() {
    P p = {};
    p.v = 4;
    Box<P> b = new Box<P>(p);
    printf("%d\n", b.get());
    b.set(6);
    printf("%d\n", b.get());
    printf("%d\n", p.v);  // the box owns a copy

    Inner inn = new Inner();
    inn.v = 9;
    Holder h = new Holder(inn);
    printf("%d\n", h.get());
    h.get();  // a promoted call is an ordinary method call on the field

    Named n = new Named(p);
    printf("%d\n", n.get());
    return 0;
}
