unit implicit_ctor_arity;
struct S { int v; implicit S(int a, int b) { v = a + b; } }
int main() { return 0; }
