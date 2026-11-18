unit audit_h2_base_zero;
extern int puts(const char* s);

class Base {
    int v;
    Base() { v = 4; }
    int get() const { return v; }
}

const class Leaf : Base {
public:
    const int w = 6;
    int sum() const { return get() + w; }
}

int main() {
    Leaf l = new Leaf();
    if l.sum() != 10 { puts("synthesized ctor with base"); return 1; }
    puts("synthesized base ok");
    return 0;
}
