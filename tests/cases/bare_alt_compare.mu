// bare_alt_compare — a bare alternative works on either side of a comparison (#155).
unit bare_alt_compare;

extern int puts(const char* s);

variant Opt { Some int; None; }

int main() {
    Opt v = None;
    if not (None == v) { puts("lhs"); return 1; }
    if not (v == None) { puts("rhs"); return 2; }
    puts("bare alt compare ok");
    return 0;
}
