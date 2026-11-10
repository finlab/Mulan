unit struct_rc_fields;
extern int puts(const char* s);
int live = 0;
int dtor_runs = 0;

interface I { int get(); }

class C : I {
    int v;
    C(int x) { v = x; live += 1; }
    ~C() { live -= 1; }
    override int get() { return v; }
}

struct H {
    C c;
    int n;
}

struct N {
    C[2] cs;
}

struct P {
    C c;
    int n;
    P(C x, int k) { c = x; n = k; }
}

struct Wrap {
    P p;
    int tag;
}

struct S {
    I i;
    C c;
    ~S() { dtor_runs += 1; }
}

class Box {
    H h;
}

int take_h(H h) { return h.c.get() * 10 + h.n; }

H make_h(C c) {
    H h = {c, 0};
    return h;
}

P make_p(C c) {
    P p = P(c, 3);
    return p;
}

void fill_s(S* out, C c) {
    (*out).i = c;
    (*out).c = c;
}

int main() {
    C c = new C(5);
    if live != 1 { puts("bad live 1"); return 1; }
    {
        H h = {c, 1};
        if live != 1 { puts("bad copy init"); return 2; }
        H h2 = h;
        if live != 1 { puts("bad copy ctor"); return 3; }
        h2.c = new C(8);
        if live != 2 { puts("bad field assign"); return 4; }
        if h.c.get() != 5 { puts("bad shared old"); return 5; }
        if h2.c.get() != 8 { puts("bad new value"); return 6; }
        if take_h(h) != 51 { puts("bad param"); return 7; }
        if take_h(make_h(c)) != 50 { puts("bad temp param"); return 8; }
        H h3;
        h3.c = h.c;
        h3.n = 2;
        if take_h(h3) != 52 { puts("bad zero init + assign"); return 9; }
    }
    if live != 1 { puts("bad scope cleanup"); return 10; }

    {
        P p = make_p(c);
        if p.c.get() != 5 { puts("bad return value"); return 11; }
        if p.n != 3 { puts("bad return field"); return 12; }
        P q = p;
        if q.n != 3 { puts("bad copy of return"); return 13; }
        Wrap w = {};
        w.p = P(c, 6);
        w.tag = 9;
        Wrap w2 = w;
        if w2.p.n != 6 { puts("bad nested copy"); return 14; }
        if w2.tag != 9 { puts("bad nested tag"); return 15; }
        if w2.p.c.get() != 5 { puts("bad nested ref"); return 16; }
    }
    if live != 1 { puts("bad nested cleanup"); return 17; }

    {
        H[2] a = {{c, 0}, {c, 0}};
        if live != 1 { puts("bad array init"); return 18; }
        a[0].c = new C(2);
        if live != 2 { puts("bad array elem assign"); return 19; }
        if a[1].c.get() != 5 { puts("bad array shared"); return 20; }
        if a[0].c.get() != 2 { puts("bad array new"); return 21; }
        N n = {{c, c}};
        n.cs[1] = new C(3);
        if live != 3 { puts("bad ref array field"); return 22; }
        if n.cs[0].get() != 5 { puts("bad ref array field 0"); return 23; }
    }
    if live != 1 { puts("bad array cleanup"); return 24; }

    {
        Box b = new Box();
        b.h.c = new C(4);
        if live != 2 { puts("bad class field"); return 25; }
    }
    if live != 1 { puts("bad class field cleanup"); return 26; }

    {
        S s = {};
        fill_s(&s, c);
        if s.i.get() != 5 { puts("bad interface field"); return 27; }
        if s.c.get() != 5 { puts("bad class field in dtor struct"); return 28; }
        if live != 1 { puts("bad dtor struct retain"); return 29; }
    }
    if dtor_runs != 1 { puts("bad dtor runs"); return 30; }
    if live != 1 { puts("bad dtor struct cleanup"); return 31; }

    puts("struct rc fields ok");
    return 0;
}
