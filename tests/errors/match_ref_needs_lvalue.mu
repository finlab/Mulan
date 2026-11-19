unit match_ref_needs_lvalue;
variant V {
    A int;
    B;
}
V make() {
    V v = A(1);
    return move v;
}
int main() {
    match make() {
        A(ref x): { x = 2; }
        B: { }
    }
    return 0;
}
