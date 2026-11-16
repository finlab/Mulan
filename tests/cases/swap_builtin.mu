// swap_builtin — `swap(a, b)` exchanges the values at two places with a pure byte exchange: no
// constructor, destructor or reference-count operation runs (agents/value-semantics.md §14).
unit swap_builtin;

extern int printf(const char* fmt, ...);

int dtors = 0;
int live = 0;

struct Buf {
    int* p;
    int tag;
    Buf(int n) { p = alloc(n) as int*; p[0] = n; tag = n; }
    Buf(const ref Buf o) { p = alloc(o.tag) as int*; p[0] = o.p[0]; tag = o.tag; }
    Buf(move Buf o) { p = o.p; tag = o.tag; o.p = null; }
    ~Buf() {
        dtors = dtors + 1;
        free(p);
    }
}

struct P { int x; int y; }

class Cat {
    public int v;
    Cat() { live = live + 1; }
    ~Cat() { live = live - 1; }
}

struct Holder { Cat c; int n; }

int main() {
    // 1. plain value struct
    P a = {1, 2};
    P b = {3, 4};
    swap(a, b);
    if a.x != 3 or a.y != 4 { printf("pod a %d %d\n", a.x, a.y); return 1; }
    if b.x != 1 or b.y != 2 { printf("pod b %d %d\n", b.x, b.y); return 2; }

    // 2. self swap must be a no-op
    swap(a, a);
    if a.x != 3 or a.y != 4 { printf("self %d %d\n", a.x, a.y); return 3; }

    // 3. a type with a destructor: swapping must not run any destructor
    {
        Buf x = Buf(8);
        Buf y = Buf(16);
        int before = dtors;
        swap(x, y);
        if dtors != before { printf("dtor during swap %d\n", dtors - before); return 4; }
        if x.tag != 16 { printf("buf x %d\n", x.tag); return 5; }
        if y.tag != 8 { printf("buf y %d\n", y.tag); return 6; }
        if x.p == null or y.p == null { printf("buf null\n"); return 7; }
        if x.p[0] != 16 or y.p[0] != 8 { printf("buf payload %d %d\n", x.p[0], y.p[0]); return 8; }
    }

    // 4. a struct with a reference-counted field: the handle moves, the count does not change
    {
        Holder h1;
        Holder h2;
        h1.c = new Cat();
        h2.c = new Cat();
        h1.n = 1;
        h2.n = 2;
        if live != 2 { printf("live setup %d\n", live); return 9; }
        swap(h1, h2);
        if live != 2 { printf("live after swap %d\n", live); return 10; }
        if h1.n != 2 or h2.n != 1 { printf("holder n %d %d\n", h1.n, h2.n); return 11; }
        if h1.c == null or h2.c == null { printf("holder handle\n"); return 12; }
        h1.c = null;
        h2.c = null;
        if live != 0 { printf("live release %d\n", live); return 13; }
    }

    // 5. fields and array elements are places too
    {
        P[2] arr = {{1, 2}, {3, 4}};
        swap(arr[0], arr[1]);
        if arr[0].x != 3 or arr[1].x != 1 { printf("element %d %d\n", arr[0].x, arr[1].x); return 14; }
        P s = {5, 6};
        P t = {7, 8};
        swap(s.x, t.y);
        if s.x != 8 or t.y != 5 { printf("field %d %d\n", s.x, t.y); return 15; }

        Cat i1 = new Cat();
        Cat i2 = new Cat();
        Cat i3 = new Cat();
        swap(i1, i3);
        if live != 3 { printf("class live %d\n", live); return 16; }
        i1 = null;
        i2 = null;
        i3 = null;
        if live != 0 { printf("class release %d\n", live); return 17; }
    }

    printf("swap_builtin ok\n");
    return 0;
}


