// copy_positions — a user copy constructor defines copying at every position (SPEC 3.4, rule of
// three): copy initialization, a by-value argument, a by-value return, and the `?:` result.
// The counters prove each position runs it exactly once; a missed copy would alias the same
// allocation and the process would double-free at exit.
unit copy_positions;

extern int printf(const char* fmt, ...);
extern int puts(const char* s);

int copies = 0;

struct Buf {
    int* p;
    Buf(int n) { p = alloc(8) as int*; p[0] = n; }
    Buf(const ref Buf o) { copies = copies + 1; p = alloc(8) as int*; p[0] = o.p[0]; }
    ~Buf() { if (p != null) { free(p); p = null; } }
}

Buf g = Buf(5);

Buf by_return() { return g; }

int by_value(Buf b) { return b.p[0]; }

int main() {
    Buf a = Buf(1);

    Buf b = a;                      // copy initialization
    printf("init %d %d\n", copies, b.p[0]);

    Buf c = by_return();            // return by value
    printf("return %d %d\n", copies, c.p[0]);

    int r = by_value(a);            // by-value argument
    printf("pass %d %d\n", copies, r);

    Buf d = a.p[0] == 1 ? a : b;    // one owned result
    printf("ternary %d %d\n", copies, d.p[0]);

    // Every copy owns its own block: writing through one must not reach another.
    if c.p == g.p { puts("returned value aliases the source"); return 1; }
    if d.p == a.p { puts("ternary value aliases a branch"); return 2; }
    return 0;
}
