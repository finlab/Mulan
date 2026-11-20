// generic_iface_self — a `Self` return in a generic interface: a class spells the concrete type and
// the override check substitutes the implementor for the slot. Such a member has no itable slot and
// cannot be called through an interface value; concrete calls are tested here.
unit generic_iface_self;

extern int puts(const char* s);

interface I<T> {
    T get();
    Self make();
}

class C<T> : I<T> {
    public T v;
    C() {}
    override T get() { return v; }
    override C<T> make() {
        C<T> x = new C<T>();
        x.v = v;
        return x;
    }
}

struct S<T> : I<T> {
    T v;
    S() {}
    T get() { return v; }
    S<T> make() {
        S<T> x = {};
        x.v = v;
        return x;
    }
}

interface P {
    int get();
    Self make();
}

class PC : P {
    public int v;
    PC() {}
    override int get() { return v; }
    override PC make() {
        PC x = new PC();
        x.v = v;
        return x;
    }
}

int main() {
    C<int> c = new C<int>();
    c.v = 4;
    C<int> cm = c.make();
    if cm.get() != 4 { puts("class"); return 1; }

    S<int> s = {};
    s.v = 5;
    S<int> sm = s.make();
    if sm.get() != 5 { puts("struct"); return 2; }

    PC p = new PC();
    p.v = 6;
    PC pm = p.make();
    if pm.get() != 6 { puts("plain class"); return 3; }

    puts("generic iface self ok");
    return 0;
}
