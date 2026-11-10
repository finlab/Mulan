unit addr_ref_struct;
class C { int v; }
struct S { C c; int n; }
int main() {
    S s = {};
    S* p = &s;
    return 0;
}
