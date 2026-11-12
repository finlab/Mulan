// generic_inherit.mu — 泛型继承（`class Derived<T> : Base<T>`）
unit generic_inherit;

extern int puts(const char* s);

class Base<T> {
    T base_val;

    Base(T x) { base_val = x; }

    T get_base() { return base_val; }
}

class Derived<T> : Base<T> {
    T extra;

    Derived(T a, T b) {
        super(a);
        extra = b;
    }

    T total() { return get_base() + extra; }
}

int main() {
    Derived<int> d = new Derived<int>(10, 32);
    int t = d.total();
    int b = d.get_base();

    if (t == 42 and b == 10) { puts("generic inherit ok"); }
    return 0;
}
