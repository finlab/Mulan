unit addr_ref_struct_arr;
class C { int v; }
struct S { C c; int n; }
int main() {
    S[2] arr = {};
    S* p = &arr[0];
    return 0;
}
