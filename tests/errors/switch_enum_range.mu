unit switch_enum_range;
enum C { A, B, Cc }
int f(C c) {
    switch c {
        A..Cc: { return 1; }
        else: { return 0; }
    }
}
int main() { return f(A); }

