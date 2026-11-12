unit fn_method_value;
class C { int m(int x) { return x; } }
int main() {
    C c = new C();
    auto f = c.m;
    return 0;
}
