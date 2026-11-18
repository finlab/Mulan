// move_assign — move assignment is synthesized as move-and-swap: the destination's old value is destroyed,
// the source is emptied, and the buffer changes owner exactly once
// (agents/value-semantics.md §8.11).
unit move_assign;

extern int printf(const char* fmt, ...);

int buffers = 0;

struct Buf {
    int* p;
    Buf(int n) {
        p = alloc(n) as int*;
        p[0] = n;
        buffers = buffers + 1;
    }
    Buf(const ref Buf o) {
        p = alloc(o.p[0]) as int*;
        p[0] = o.p[0];
        buffers = buffers + 1;
    }
    Buf(move Buf o) {
        p = o.p;
        o.p = null;
    }
    ~Buf() {
        // A moved-from value is a live object whose destructor runs (SPEC 3.13), so it must release
        // only what it still owns.
        if p != null {
            free(p);
            buffers = buffers - 1;
        }
    }
}

int main() {
    {
        Buf a = Buf(8);
        Buf c = Buf(16);
        if buffers != 2 { printf("setup %d\n", buffers); return 1; }

        c = move a;                       // old c is destroyed, a's buffer changes owner
        if c.p == null { printf("destination\n"); return 2; }
        if c.p[0] != 8 { printf("payload %d\n", c.p[0]); return 3; }
        if buffers != 1 { printf("after move assign %d\n", buffers); return 4; }
    }
    if buffers != 0 { printf("leak %d\n", buffers); return 5; }

    printf("buffers %d\n", buffers);
    printf("move_assign ok\n");
    return 0;
}


