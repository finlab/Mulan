// `const class` is a leaf type whose content is fixed after construction (SPEC 3.7): construction is the
// only writable phase (the base subobject included), every expression of the type is a read-only view,
// widening to a mutable view needs `as`, and an override of a mutating virtual of the base stays inert.
unit const_class_leaf;

extern int puts(const char* s);

interface IShow { int get() const; }

class Base : IShow {
    int v;
    Base(int x) { v = x; }
    virtual void poke() { v = v + 1; }
    void bump(int d) { v = v + d; }
    override int get() const { return v; }
}

const class Leaf : Base {
    Leaf(int x) { super(x); bump(10); }
    // The base's mutating slot: the body is read-only, so an `as` escape cannot write through it.
    override void poke() { }
}

int main() {
    Leaf l = new Leaf(5);
    if l.get() != 15 { puts("constructor could not write the base subobject"); return 1; }

    // An interface whose every member is `const` is a read-only contract: no `as` needed.
    IShow s = l;
    if s.get() != 15 { puts("read-only interface view failed"); return 2; }
    const IShow cs = l;
    if cs.get() != 15 { puts("const interface view failed"); return 3; }

    Base b = l as Base;   // explicit widening; writes through it would be UB
    b.poke();             // dispatches to Leaf.poke, which is inert
    if l.get() != 15 { puts("override is not inert"); return 4; }

    const Base cb = l;
    if cb.get() != 15 { puts("const base view failed"); return 5; }
    puts("const class leaf ok");
    return 0;
}
