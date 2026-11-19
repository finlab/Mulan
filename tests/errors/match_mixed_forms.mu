unit match_mixed_forms;
variant V {
    A int;
    B;
}
int main() {
    V v = A(1);
    match v {
        A(n): n;
        B: 0;
    }
    return 0;
}
