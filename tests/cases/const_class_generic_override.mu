unit audit210_generic_override;
extern int puts(const char* s);

class Base {
    int v;
    Base(int x) { v = x; }
    virtual void poke() { v = v + 1; }
    int get() const { return v; }
}

const class G<T> : Base {
public:
    T payload;
    G(T p, int x) : Base(x), payload(p) { }
    override void poke() { }        // inert slot in a generic const class
    T data() const { return payload; }
}

int main() {
    G<int> g = new G<int>(5, 10);
    if g.get() != 10 or g.data() != 5 { puts("generic const class values"); return 1; }
    Base b = g as Base;
    b.poke();
    if g.get() != 10 { puts("generic override not inert"); return 2; }
    puts("generic const override ok");
    return 0;
}
