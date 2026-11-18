unit audit210_neg_global;
class Base { int v; Base(int x) { v = x; } void poke() { v = v + 1; } int get() const { return v; } }
const class Leaf : Base { public Leaf(int x) : Base(x) { } }

Leaf g = new Leaf(1);      // a global of a `const class` type

int main() {
    g.poke();
    return 0;
}
