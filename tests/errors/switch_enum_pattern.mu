unit switch_enum_pattern;
enum C { A, B }
int f(C c) {
    switch c {
        0: { return 1; }
        B: { return 2; }
    }
}
int main() { return f(A); }

