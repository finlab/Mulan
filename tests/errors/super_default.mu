unit super_default;
interface I { int d() { return 1; } }
class C : I { C(){} ~C(){} }
class D : C { D(){super();} ~D(){} int call_default() { return super.d(); } }
int main() { return 0; }

