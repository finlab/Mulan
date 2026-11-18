unit audit_h1_base_field;
extern int puts(const char* s);

class Base {
    public int v;
    Base(int x) { v = x; }
    int get() const { return v; }
}

const class Leaf : Base {
public:
    int own;
    Leaf(int x) : Base(x), own(1) { }
}

int main() {
    Leaf l = new Leaf(1);
    l.v = 5;                 // base field of a `const class`: must be rejected
    if l.get() != 1 { puts("base field was written"); return 1; }
    puts("ok");
    return 0;
}
