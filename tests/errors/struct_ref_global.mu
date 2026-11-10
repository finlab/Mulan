unit struct_ref_global;
class C { int v; }
struct H { C c; }
H g = {};
int main() { return 0; }
