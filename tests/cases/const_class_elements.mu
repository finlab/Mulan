unit audit210_elements;
extern int puts(const char* s);

class Base { int v; Base(int x) { v = x; } void poke() { v = v + 1; } int get() const { return v; } }
const class Leaf : Base {
public:
    Leaf(int x) : Base(x) { }
    int twice() const { return get() * 2; }
}

const class Frozen<T> {
public:
    T v;
    Frozen(T x) : v(x) { }
    T peek() const { return v; }
}

class Mutable<T> {
public:
    T v;
    Mutable(T x) : v(x) { }
    T peek() const { return v; }
}

int main() {
    Leaf[] xs = new Leaf[2];
    xs[0] = new Leaf(3);
    if xs[0].twice() != 6 { puts("slice element read"); return 1; }

    Frozen<Leaf> f = new Frozen<Leaf>(new Leaf(9));
    if f.peek().twice() != 18 { puts("generic read"); return 2; }

    Mutable<Leaf> m = new Mutable<Leaf>(new Leaf(9));
    m.v = new Leaf(1);          // the container is mutable: replacing the handle is fine
    if m.peek().twice() != 2 { puts("generic field assign"); return 3; }

    puts("elements ok");
    return 0;
}
