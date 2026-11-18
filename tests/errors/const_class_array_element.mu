unit audit_h1_array_of_leaf;
const class Leaf { public const int v; Leaf(int x) : v(x) { } }
int main() { Leaf[] xs = new Leaf[2]; xs[0] = new Leaf(1); xs[0].v = 5; return 0; }
