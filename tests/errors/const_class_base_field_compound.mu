unit audit_h1_compound;
class Base { public int v; Base(int x) { v = x; } }
const class Leaf : Base { public Leaf(int x) : Base(x) { } }
int main() { Leaf l = new Leaf(1); l.v += 1; return 0; }
