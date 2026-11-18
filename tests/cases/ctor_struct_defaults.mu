unit audit212_struct_delegate_defaults;
extern int puts(const char* s);

struct Pair {
    public const int a = 1;
    public int b;
    Pair(int x) : b(x) { }
    Pair() : Pair(2) { }
    int sum() const { return a + b; }
}

struct Tagged {
    public string tag;
    public const int code = 9;
    implicit Tagged(string t) : tag(t) { }
    int size() const { return tag.length() as int + code; }
}

int main() {
    Pair p = Pair();
    if p.sum() != 3 { puts("struct default + delegate"); return 1; }
    Tagged t = Tagged("abc");
    if t.size() != 12 { puts("implicit struct ctor with a list"); return 2; }
    puts("struct defaults ok");
    return 0;
}

