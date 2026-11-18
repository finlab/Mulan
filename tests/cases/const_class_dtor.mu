unit audit210_dtor_write;
extern int puts(const char* s);
int seen = 0;

class Base {
    int v;
    Base(int x) { v = x; }
    void poke() { v = v + 1; }
    int get() const { return v; }
}

// Constructing and destroying are the only writable phases (SPEC 3.7): the destructor may write the base
// subobject, while every ordinary method body is read-only.
const class Resource : Base {
public:
    int* raw;
    Resource(int x) : Base(x), raw(null) { }
    ~Resource() { poke(); seen = get(); }
    int value() const { return get(); }
}

int main() {
    {
        Resource r = new Resource(3);
        if r.value() != 3 { puts("value"); return 1; }
        r = null;
    }
    if seen != 4 { puts("dtor could not write the base subobject"); return 2; }
    puts("const dtor ok");
    return 0;
}
