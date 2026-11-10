unit struct_lifecycle;
extern int puts(const char* s);
int live = 0;
struct S {
    int v;
    S() { init(); live += 1; }
    S(int x) { v = x; live += 1; }
    ~S() { live -= 1; }
    void init() { v = 9; }
    int get() { return v; }
}
int early(int x) {
    S a = S(1);
    if x > 0 {
        S b = S(2);
        return a.get() + b.get();
    }
    return a.get();
}
int main() {
    {
        S a;
        S b = S(5);
        if a.get() != 9 { puts("default ctor failed"); return 1; }
        if b.get() != 5 { puts("arg ctor failed"); return 1; }
        if live != 2 { puts("ctor live failed"); return 1; }
    }
    if live != 0 { puts("dtor scope failed"); return 1; }
    if early(1) != 3 { puts("early return value failed"); return 1; }
    if live != 0 { puts("early return dtor failed"); return 1; }
    int i = 0;
    while i < 3 {
        S s = S(i);
        i += 1;
        if i == 2 { continue; }
        if i == 3 { break; }
    }
    if live != 0 { puts("loop dtor failed"); return 1; }
    puts("struct lifecycle ok");
    return 0;
}
