// struct_dtor_assign_host — assignment of a host that contains a value with a user destructor goes through
// copy-and-swap with the host's own copy constructor, never a byte copy: a byte copy would leave two owners
// of the same block and free it twice (agents/value-semantics.md §8.11).
unit struct_dtor_assign_host;

extern int printf(const char* fmt, ...);

int copies = 0;
int frees = 0;

struct S {
    byte* p;
    isize n;
    S() { p = null; n = 0; }
    S(const ref S o) {
        copies = copies + 1;
        n = o.n;
        if o.p == null {
            p = null;
        } else {
            p = alloc(n) as byte*;
            for i in 0..n { p[i] = o.p[i]; }
        }
    }
    S(move S o) { p = o.p; n = o.n; }
    ~S() {
        if p != null { frees = frees + 1; free(p); }
    }
}

struct O {
    S s;
    int k;
    O() { k = 0; }
    O(const ref O o) { s = o.s; k = o.k; }
}

int main() {
    {
        O a = {};
        a.s.p = alloc(4) as byte*;
        a.s.n = 4;
        a.s.p[0] = 7;
        O b = {};
        b = a;
        if copies != 1 { printf("copies %d\n", copies); return 1; }
        if b.s.n != 4 or b.s.p == null { printf("copy payload\n"); return 2; }
        if b.s.p == a.s.p { printf("aliased\n"); return 3; }
        if b.s.p[0] != 7 { printf("copy contents\n"); return 4; }
        if a.s.p == null { printf("source consumed\n"); return 5; }
    }
    if frees != 2 { printf("frees %d\n", frees); return 6; }
    printf("struct_dtor_assign_host ok\n");
    return 0;
}
