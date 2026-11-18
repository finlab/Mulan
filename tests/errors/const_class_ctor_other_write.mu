unit audit_h1_ctor_other;
class Base { public int v; Base(int x) { v = x; } }
const class Leaf : Base { public Leaf(int x) : Base(x) { } }
class Holder { Leaf item; Holder(Leaf l) { item = l; item.v = 9; } }
int main() { return 0; }
