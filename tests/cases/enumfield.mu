unit enumfield;

extern int puts(readonly char* s);

enum Color { Red, Green, Blue }

struct S { Color c; int n; }

int main() {
    S s = {Green, 7};
    if s.c == Blue - 1 and s.n == 7 { puts("enum field ok"); }
    S z = 0;
    if z.c == Red { puts("enum zero ok"); }
    return 0;
}
