// implicit_global — a global initialization goes through an implicit constructor like the local form
// (#159).
unit implicit_global;

extern int puts(const char* s);

struct Wrap {
    int v;
    implicit Wrap(int n) { v = n; }
}

Wrap g = 5;

int main() {
    Wrap w = 5;
    if g.v != 5 or w.v != 5 { puts("value"); return 1; }
    puts("implicit global ok");
    return 0;
}
