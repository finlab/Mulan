// const_field_default.mu — `const T x = expr;` is a default member initializer: a constructor that does
// not name the field uses it, and an initializer-list entry wins over it (SPEC 3.7).
unit const_field_default;

extern int puts(const char* s);

struct Cfg {
    const int max = 5;
    const int id;
    Cfg(int i) : id(i) { }
    Cfg(int i, int m) : id(i), max(m) { }
}

class K {
    public const int base = 7;
    K() { }
}

int main() {
    Cfg a = Cfg(1);
    if a.max != 5 or a.id != 1 { puts("default member initializer failed"); return 1; }
    Cfg b = Cfg(2, 99);
    if b.max != 99 or b.id != 2 { puts("explicit initializer did not win"); return 2; }
    K k = new K();
    if k.base != 7 { puts("class default member initializer failed"); return 3; }
    puts("const field default ok");
    return 0;
}
