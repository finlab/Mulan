unit audit210_views_struct;
extern int puts(const char* s);

class Base { int v; Base(int x) { v = x; } void poke() { v = v + 1; } int get() const { return v; } }
const class Leaf : Base {
public:
    Leaf(int x) : Base(x) { }
    int twice() const { return get() * 2; }
}

struct Holder { public Leaf l; }

int main() {
    Holder h = Holder(new Leaf(4));
    if h.l.twice() != 8 { puts("struct field read"); return 1; }
    h.l.poke();
    return 0;
}
