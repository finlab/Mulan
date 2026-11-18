// value_semantics_matrix — the pieces of the value-semantics work exercised together: a resource-owning
// struct (ctor / copy / move / dtor) as a struct field, as array elements and inside a class; implicit
// member construction; synthesized assignment; `swap`; a `nocopy` move-only type; and reference counting
// next to value semantics. Every counter must end at zero (agents/value-semantics.md).
unit value_semantics_matrix;

extern int printf(const char* fmt, ...);

int buf_live = 0;   // resource-owning structs alive
int cat_live = 0;   // class instances alive
int copies = 0;
int moves = 0;

struct Buf {
    int* p;
    int tag;
    // `buf_live` counts *resources* (a moved-from or default-constructed shell owns none), so the
    // default constructor does not increment it and the destructor decrements only when it frees.
    Buf() { p = null; tag = 0; }
    Buf(int n) {
        p = alloc(n) as int*;
        p[0] = n;
        tag = n;
        buf_live = buf_live + 1;
    }
    Buf(const ref Buf o) {
        tag = o.tag;
        p = alloc(tag) as int*;
        p[0] = o.p[0];
        buf_live = buf_live + 1;
        copies = copies + 1;
    }
    Buf(move Buf o) {
        p = o.p;
        tag = o.tag;
        o.p = null;
        moves = moves + 1;
    }
    ~Buf() {
        // The destructor of a moved-from value runs too (SPEC 3.13), so it releases only what it owns.
        if p != null {
            free(p);
            buf_live = buf_live - 1;
        }
    }
}

nocopy struct Handle {
    int fd;
    Handle() { fd = 0; }
    Handle(move Handle o) { fd = o.fd; o.fd = -1; }
}

class Cat {
    public int v;
    Cat(int x) { v = x; cat_live = cat_live + 1; }
    ~Cat() { cat_live = cat_live - 1; }
}

struct Holder { Buf b; Cat c; int n; }     // a resource member and a reference-counted member
struct Cards { Buf[2] pair; int n; }       // resource elements

Buf make(int n) {
    Buf b = Buf(n);
    return move b;
}

int main() {
    // 1. resource member + reference-counted member, aggregate-initialized in place
    {
        Holder h = { Buf(4), new Cat(9), 7 };
        if h.b.tag != 4 { printf("holder buf %d\n", h.b.tag); return 1; }
        if h.c == null { printf("holder cat\n"); return 2; }
        if h.c.v != 9 { printf("holder cat v %d\n", h.c.v); return 3; }
        if h.n != 7 { printf("holder n\n"); return 4; }
        if buf_live != 1 { printf("holder live %d\n", buf_live); return 5; }
        if cat_live != 1 { printf("holder cats %d\n", cat_live); return 6; }
    }
    if buf_live != 0 { printf("holder leak %d\n", buf_live); return 7; }
    if cat_live != 0 { printf("holder cat leak %d\n", cat_live); return 8; }

    // 2. default-constructed array elements of a resource type, plus swap
    {
        Cards cs;
        // The two elements are default-constructed: empty shells, so they own no resource.
        if buf_live != 0 { printf("cards live %d\n", buf_live); return 9; }
        // A resource type cannot be constructed inside an expression (the temporary would need a
        // statement-end dtor), so the new value is built in a local and moved in.
        Buf t0 = Buf(11);
        cs.pair[0] = move t0;       // assignment over a live value (move-and-swap)
        Buf t1 = Buf(22);
        cs.pair[1] = move t1;
        if buf_live != 2 { printf("cards assign %d\n", buf_live); return 10; }
        if cs.pair[0].tag != 11 { printf("cards 0 %d\n", cs.pair[0].tag); return 11; }
        if cs.pair[1].tag != 22 { printf("cards 1 %d\n", cs.pair[1].tag); return 12; }
        swap(cs.pair[0], cs.pair[1]);
        if cs.pair[0].tag != 22 { printf("cards swap 0 %d\n", cs.pair[0].tag); return 13; }
        if cs.pair[1].tag != 11 { printf("cards swap 1 %d\n", cs.pair[1].tag); return 14; }
        if cs.pair[0].p[0] != 22 { printf("cards payload %d\n", cs.pair[0].p[0]); return 15; }
    }
    if buf_live != 0 { printf("cards leak %d\n", buf_live); return 16; }

    // 3. copy construction and move construction
    {
        int c0 = copies;
        int m0 = moves;
        Buf a = Buf(8);
        Buf b = a;                  // deep copy
        Buf c = move a;             // transfer, `a` is out of reach afterwards
        if b.tag != 8 { printf("copy tag %d\n", b.tag); return 17; }
        if b.p[0] != 8 { printf("copy payload\n"); return 18; }
        if c.tag != 8 { printf("move tag %d\n", c.tag); return 19; }
        if buf_live != 2 { printf("live buffers (move transfers, it does not allocate) %d\n", buf_live); return 20; }
        if copies - c0 != 1 { printf("copies %d\n", copies - c0); return 21; }
        if moves - m0 != 1 { printf("moves %d\n", moves - m0); return 22; }
    }
    if buf_live != 0 { printf("copy leak %d\n", buf_live); return 23; }

    // 4. a move-only value crossing a function boundary
    {
        int m1 = moves;
        Buf r = make(5);            // `return move b;` inside make
        if r.tag != 5 { printf("make tag %d\n", r.tag); return 24; }
        if moves - m1 != 1 { printf("make moves %d\n", moves - m1); return 25; }
    }
    if buf_live != 0 { printf("make leak %d\n", buf_live); return 26; }

    // 5. `nocopy` move-only type: a move of a small struct next to the checks above
    {
        Handle h;
        h.fd = 3;
        Handle k = move h;
        if k.fd != 3 { printf("handle %d\n", k.fd); return 27; }
    }
    // 6. reference counting is untouched by the value machinery
    {
        Cat c1 = new Cat(1);
        Cat c2 = c1;                 // handle copy: no second instance
        if cat_live != 1 { printf("rc %d\n", cat_live); return 28; }
        c1 = null;
        if cat_live != 1 { printf("rc after null %d\n", cat_live); return 29; }
        swap(c2, c2);
        if cat_live != 1 { printf("rc after swap %d\n", cat_live); return 30; }
        c2 = null;
        if cat_live != 0 { printf("rc release %d\n", cat_live); return 31; }
    }

    printf("buf_live %d cat_live %d copies %d moves %d\n", buf_live, cat_live, copies, moves);
    printf("value_semantics_matrix ok\n");
    return 0;
}

