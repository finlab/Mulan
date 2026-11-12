// generic_class.mu — generic class instantiation (U34 M3e): constructor, methods, vtable
unit generic_class;

extern int puts(const char* s);

class Box<T> {
    T v;

    Box(T x) { v = x; }

    T get() { return v; }

    void set(T x) { v = x; }
}

class Counter<T> {
    T n;

    Counter() { n = 0; }

    void bump() { n = n + 1; }

    T value() { return n; }
}

int main() {
    Box<int> b = new Box<int>(7);
    int x = b.get();
    b.set(42);
    int y = b.get();

    Counter<long> c = new Counter<long>();
    c.bump();
    c.bump();
    c.bump();

    if (x == 7 and y == 42 and c.value() == 3) { puts("generic class ok"); }
    return 0;
}
