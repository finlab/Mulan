unit implicit_ctor_class;
class C { int v; implicit C(int x) { v = x; } }
int main() { return 0; }
