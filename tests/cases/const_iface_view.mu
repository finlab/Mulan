// const_iface_view.mu — a `const` interface view keeps its methods: a `const` field of an interface type
// can call the interface's `const` methods (SPEC 3.7).
unit const_iface_view;

extern int puts(const char* s);

interface IC {
    int f() const;
}

class Impl : IC {
    public int v;
    Impl(int x) : v(x) { }
    override int f() const { return v; }
}

struct HoldsC {
    public const IC obj;
    HoldsC(IC o) : obj(o) { }
}

int main() {
    HoldsC h = HoldsC(new Impl(7));
    if h.obj.f() != 7 { puts("const interface view failed"); return 1; }
    puts("const iface view ok");
    return 0;
}
