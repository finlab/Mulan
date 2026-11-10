unit addr_iface_ref;
interface I { int f(); }
int main() {
    I i = null;
    int* p = &i;
    return 0;
}
