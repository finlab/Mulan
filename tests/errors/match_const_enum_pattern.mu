unit match_const_enum_pattern;
enum C { A, B }
int f(C c) {
    match c {
        0: { return 1; }
        B: { return 2; }
    }
}
int main() { return f(A); }

