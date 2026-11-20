// mangle_user_prefix — user identifiers starting with C/D and a free function named op_add no longer
// share a symbol with a constructor, a destructor or an operator (#197).
unit mangle_user_prefix;

extern int puts(const char* s);

class foo {
    public int v;
    foo() { v = 1; }
    ~foo() { }
}

void Cfoo() { }
void Dfoo() { }

struct P { public int v; }
P operator+(P a, P b) {
    P r = {};
    r.v = a.v + b.v;
    return r;
}
int op_add(int a, int b) { return a + b; }

int main() {
    foo f = new foo();
    if f.v != 1 { puts("ctor"); return 1; }
    Cfoo();
    Dfoo();
    P a = { v = 2 };
    P b = { v = 3 };
    if (a + b).v != 5 { puts("operator"); return 2; }
    if op_add(2, 3) != 5 { puts("free op_add"); return 3; }
    puts("mangle user prefix ok");
    return 0;
}
