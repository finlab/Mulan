// generic_ref_ret.mu — a `ref T` return of a generic struct keeps the borrow at instantiation:
// `c.at().x = 5` and `c.at().bump()` must write through to `c.v` (a copy would be discarded).
unit generic_ref_ret;

extern int puts(const char* s);

struct Pt {
    int x;

    void bump() { x += 1; }
    int get() { return x; }
}

struct Cell<T> {
    T v;

    ref T at() { return v; }
}

struct Num {
    int v;

    ref int at() { return v; }
}

int main() {
    Cell<Pt> c;
    c.v.x = 0;
    c.at().x = 5;
    c.at().bump();
    int a = c.v.get();

    Num n;
    n.v = 0;
    n.at() = 7;

    if (a == 6 and n.v == 7) { puts("generic ref ret ok"); }
    return 0;
}
