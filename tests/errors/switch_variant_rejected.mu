unit switch_variant_rejected;
variant V {
    A int;
    B;
}
int main() {
    V v = A(1);
    switch v {
        A(n): { }
        B: { }
    }
    return 0;
}
