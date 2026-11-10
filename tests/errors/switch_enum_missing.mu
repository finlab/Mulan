unit switch_enum_missing;
enum C { A, B, Cc }
int f(C c) {
    switch c {
        A: { return 1; }
        B: { return 2; }
    }
}
int main() { return f(A); }

