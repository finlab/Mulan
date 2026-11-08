unit adv3;
extern int puts(readonly char* s);
int main() {
    if len("abc") == 3 { puts("len literal ok"); }
    int n = 0;
    for c in "hi" { n += c; }
    if n == 209 { puts("iter literal ok"); }
    return 0;
}
