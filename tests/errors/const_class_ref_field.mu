unit audit_m3_ref_field;
void bump(ref int x) { x += 1; }
class Base { public int v; Base(int x) { v = x; } }
const class Leaf : Base { public Leaf(int x) : Base(x) { } }
int main() { Leaf l = new Leaf(1); bump(l.v); return 0; }
