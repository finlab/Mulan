unit array_field_assign;
struct S { int[3] v; }
int main() {
    S s;
    int[3] a = {1, 2, 3};
    s.v = a;
    return s.v[0];
}
