unit adv2;

extern int puts(const char* s);
extern int putchar(int c);

struct P { int x, y; }

P mk(int a, int b) {
    return P(a, b);
}

void show(string s) {
    for c in s {
        putchar(c);
    }
    putchar('\n');
}

int main() {
    // iterate a string byte by byte
    show("hi");

    // empty slice
    int[] e = {};
    int n = 0;
    for v in e { n += 1; }
    if len(e) == 0 and n == 0 { puts("empty slice ok"); }

    // zero-value construction
    P z = P();
    if z.x == 0 and z.y == 0 { puts("ctor zero ok"); }

    // struct return value + reassignment
    P a = mk(5, 6);
    P b = a;
    b.x = 9;
    if a.x == 5 and b.x == 9 { puts("return value ok"); }

    // global string constant
    const string g = "global";
    const string h = g;
    if h.length() == 6 and h[0] == 'g' { puts("const string ok"); }

    // 32-bit boundary literal
    uint u = 0xFFFFFFFF;
    if u == 4294967295 { puts("uint literal ok"); }

    // long integer
    long big = 1;
    for i in 0..40 { big *= 2; }
    if big == 1099511627776 { puts("long ok"); }
    return 0;
}
