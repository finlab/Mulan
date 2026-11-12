unit adv3;
extern int puts(const char* s);
int main() {
    string lit = "abc"; if lit.length() == 3 { puts("len literal ok"); }
    int n = 0;
    string hi = "hi"; for c in hi { n += c; }
    if n == 209 { puts("iter literal ok"); }
    return 0;
}
