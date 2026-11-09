unit struct_ref_global;
class C { int v; }
struct H { C c; }
static H g = 0;
int main() { return 0; }
