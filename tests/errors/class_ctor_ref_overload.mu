unit class_ctor_ref_overload;
class C { public int v; C(int a) { v = a; } C(ref int a) { v = a; } }
int main() { C c = new C(1); return c.v; }
