unit audit_h2_base_args;
class A { public int v; A(int x) { v = x; } }
const class B : A { public const int w = 1; }
int main() { B b = new B(); return 0; }
