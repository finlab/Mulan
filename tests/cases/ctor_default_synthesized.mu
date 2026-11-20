// A type that declares default member initializers but no constructor of its own gets a synthesized
// zero-argument constructor (SPEC 3.7), so every construction site applies those initializers instead of
// leaving the fields zeroed. A declaration, an explicit `T()` and `new T()` all go through it.
unit ctor_default_synthesized;

extern int puts(const char* s);

struct Plain {
    public const int x = 7;
    public int y;
}

class NoCtor {
public:
    const int n = 5;
    int get() const { return n; }
}

struct Boxed<T> {
    public const int n = 2;
    public T v;
}

int main() {
    Plain a;                     // a declaration constructs when a zero-argument constructor exists
    if a.x != 7 { puts("struct declaration"); return 1; }

    Plain b = Plain();
    if b.x != 7 { puts("explicit struct construction"); return 2; }

    NoCtor c = new NoCtor();
    if c.get() != 5 { puts("class default member initializer"); return 3; }

    Boxed<int> g;
    if g.n != 2 { puts("generic instance default member initializer"); return 4; }

    puts("default ctor ok");
    return 0;
}
