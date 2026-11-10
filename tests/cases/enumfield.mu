unit enumfield;

extern int puts(const char* s);

enum Color { Red, Green, Blue }

struct S { Color c; int n; }

int main() {
    S s = {Green, 7};
    if s.c == Green and s.n == 7 { puts("enum field ok"); }
    S z = {};
    if z.c == Red { puts("enum zero ok"); }
    return 0;
}
