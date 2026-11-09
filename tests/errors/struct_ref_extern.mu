unit struct_ref_extern;
class C { int v; }
struct H { C c; int n; }
extern void f(H h);
int main() { return 0; }
