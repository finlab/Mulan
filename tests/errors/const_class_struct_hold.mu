unit audit_h1_struct_hold;
class Base { public int v; Base(int x) { v = x; } }
const class Leaf : Base { public Leaf(int x) : Base(x) { } }
struct Holder { public Leaf item; }
int main() { Holder h = Holder(new Leaf(1)); h.item.v = 5; return 0; }
