// A delegating constructor owns no field step, so the type'"'"'s default member initializers must not be
// turned into prologue items for it (SPEC 3.12): the target constructor applies them.
unit ctor_delegate_default;

extern int puts(const char* s);

class Cfg {
public:
    const int id = 3;
    const int k = 2;
    Cfg(int i) : id(i) { }
    Cfg() : Cfg(5) { }
    int get() const { return id * 10 + k; }
}

class Frozen {
public:
    const int a = 7;
    const int b;
    Frozen(int x) : b(x) { }
    Frozen() : Frozen(4) { }
    int sum() const { return a + b; }
}

int main() {
    Cfg c = new Cfg();
    if c.get() != 52 { puts("class default + delegate"); return 1; }
    Frozen f = new Frozen();
    if f.sum() != 11 { puts("class default + delegate"); return 2; }
    puts("ctor delegate default ok");
    return 0;
}
