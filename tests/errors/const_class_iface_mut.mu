// A `const class` leaf cannot even be reached through a mutating interface: the interface handle is a
// mutable view, so the conversion needs `as` (SPEC 3.7).
unit const_class_iface_mut;

interface IMut { void poke(); }

class Base : IMut {
    public int v;
    Base(int x) { v = x; }
    override void poke() { v = v + 1; }
}

const class Leaf : Base {
    Leaf() { super(100); }
}

int main() {
    Leaf l = new Leaf();
    IMut i = l;
    return 0;
}
