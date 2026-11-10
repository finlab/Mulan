unit addr_class_elem;
class C { int x; }
int main() {
    C[2] a = {};
    void* p = &a[0];
    return 0;
}
