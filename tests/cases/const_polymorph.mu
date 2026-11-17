// const_polymorph.mu — a `const` class handle is a read-only *view*: a derived object converts to a
// `const` base view implicitly (upcast + add cv in one step), and virtual dispatch still reaches the
// derived override (SPEC 3.7/3.9).
unit const_polymorph;

extern int puts(const char* s);

class Animal {
    virtual int legs() const { return 4; }
}

class Spider : Animal {
    override int legs() const { return 8; }
}

int legs_of(const Animal a) { return a.legs(); }

int main() {
    const Animal a = new Spider();
    if a.legs() != 8 { puts("const view lost the override"); return 1; }
    if legs_of(new Spider()) != 8 { puts("const parameter view failed"); return 2; }
    Animal m = new Spider();
    const Animal v = m;
    if v.legs() != 8 { puts("const view of a mutable handle failed"); return 3; }
    puts("const polymorph ok");
    return 0;
}
