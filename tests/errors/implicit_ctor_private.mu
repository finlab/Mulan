unit implicit_ctor_private;
struct S { int v; private implicit S(int x) { v = x; } }
int main() { return 0; }
