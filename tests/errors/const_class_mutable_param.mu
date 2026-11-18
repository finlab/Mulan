unit audit210_neg_param;
class Base { int v; Base(int x) { v = x; } void poke() { v = v + 1; } }
const class Leaf : Base { public Leaf(int x) : Base(x) { } }
void take(Base b) { b.poke(); }
int main() {
    Leaf l = new Leaf(1);
    take(l);
    return 0;
}
