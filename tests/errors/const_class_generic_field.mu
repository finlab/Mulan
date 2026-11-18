unit audit210_neg_generic;
class Base { int v; Base(int x) { v = x; } void poke() { v = v + 1; } int get() const { return v; } }
const class Leaf : Base { public Leaf(int x) : Base(x) { } }
class Holder<T> { public T v; Holder(T x) : v(x) { } }
int main() {
    Holder<Leaf> h = new Holder<Leaf>(new Leaf(1));
    h.v.poke();
    return 0;
}
