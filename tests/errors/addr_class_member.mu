unit addr_class_member;
class C { int x; }
int main() {
    C c = new C();
    int* p = &c.x;
    return 0;
}
