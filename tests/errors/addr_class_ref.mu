unit addr_class_ref;
class C { int x; }
int main() {
    C c = new C();
    int* p = &c;
    return 0;
}
