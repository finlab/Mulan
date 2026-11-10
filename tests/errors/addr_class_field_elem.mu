unit addr_class_field_elem;
class H { int[3] v; }
int main() {
    H h = new H();
    int* p = &h.v[0];
    return 0;
}
