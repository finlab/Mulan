unit match_move_ref;
variant V {
    A int;
    B;
}
int main() {
    V v = A(1);
    match move v {
        A(ref x): { x = 2; }
        B: { }
    }
    return 0;
}
