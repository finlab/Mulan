// if_nested_cond.mu — a nested `#if` inside a member arm keeps its position, a constructor body may be
// conditional, and a dead arm's `#error` never fires.
unit if_nested_cond;
extern int puts(const char* s);
const bool ON = true;

struct S {
    int a;
#if ON {
#if ON {
    int b;
}
#else {
    int z;
}
}
#else {
#error "outer arm dead"
}
    int c;
}

struct C {
    int v;
    C(int x) : v(x) {
#if ON {
        if v != 0 { }
}
#else {
#error "ctor arm dead"
}
    }
}

int main() {
    S s;
    s.a = 1; s.b = 2; s.c = 3;
    if s.b != 2 { puts("nested member failed"); return 1; }
    C c = C(5);
    if c.v != 5 { puts("ctor failed"); return 2; }
    puts("nested ok");
    return 0;
}
