unit match_const_ref_write;
variant V {
    A int;
    B;
}
int main() {
    V v = A(1);
    match v {
        A(const ref x): { x = 2; }
        B: { }
    }
    return 0;
}
