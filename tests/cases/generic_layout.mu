// generic_layout — layout/copy/move modifiers survive instantiation: align(N), packed, nocopy and
// nomove are properties of the instance, not just of the template.
unit generic_layout;

extern int puts(const char* s);

align(16) struct A<T> {
    T v;
    A() {}
}

packed struct PK<T> {
    byte b;
    T v;
    PK() {}
}

nocopy struct NC<T> {
    T v;
    NC() {}
}

nomove struct NM<T> {
    T v;
    NM() {}
}

int main() {
    A<int> a = {};
    a.v = 1;
    PK<int> p = {};
    p.b = 2;
    p.v = 3;
    NC<int> c = {};
    c.v = 4;
    NM<int> m = {};
    m.v = 5;

    if sizeof(A<int>) != 16 or alignof(A<int>) != 16 { puts("align"); return 1; }
    if sizeof(PK<int>) != 5 { puts("packed"); return 2; }
    if a.v != 1 or p.b != 2 or p.v != 3 or c.v != 4 or m.v != 5 { puts("fields"); return 3; }
    puts("generic layout ok");
    return 0;
}
