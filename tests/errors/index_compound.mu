unit idx_compound;
class V {
    int x;
    V() { x = 0; }
    ~V() {}
    int operator[](int i) { return x; }
}
void operator[]=(V v, int i, int x) { v.x = x; }
int main() {
    V v = new V();
    v[0] += 1;
    v = null;
    return 0;
}
