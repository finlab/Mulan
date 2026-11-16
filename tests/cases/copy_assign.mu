// copy_assign — the copy constructor and the synthesized assignment (copy-and-swap) of a value type that
// owns a resource (agents/value-semantics.md §8.1, §8.11).
unit copy_assign;

extern int printf(const char* fmt, ...);

int buffers = 0;
int copies = 0;

struct Buf {
    int* p;
    int n;
    Buf(int size) {
        n = size;
        p = alloc(size) as int*;
        buffers = buffers + 1;
    }
    Buf(const ref Buf other) {          // copy constructor: deep copy
        n = other.n;
        p = alloc(n) as int*;
        p[0] = other.p[0];
        buffers = buffers + 1;
        copies = copies + 1;
    }
    ~Buf() {
        free(p);
        buffers = buffers - 1;
    }
}

int main() {
    {
        Buf a = Buf(8);
        a.p[0] = 42;

        Buf b = a;                            // copy construction: a distinct buffer
        if b.p == null { printf("copy buffer\n"); return 1; }
        if b.p[0] != 42 { printf("copy content %d\n", b.p[0]); return 2; }
        if buffers != 2 { printf("buffers after copy %d\n", buffers); return 3; }

        b = a;                                // assignment over a live value: destroy-and-rebuild
        if b.p[0] != 42 { printf("assign content\n"); return 4; }
        if buffers != 2 { printf("buffers after assign %d\n", buffers); return 5; }

        b = b;                                // self-assignment must stay safe
        if b.p[0] != 42 { printf("self assign %d\n", b.p[0]); return 6; }
        if buffers != 2 { printf("buffers after self %d\n", buffers); return 7; }
    }
    if buffers != 0 { printf("leak %d\n", buffers); return 8; }

    printf("copies %d buffers %d\n", copies, buffers);
    printf("copy_assign ok\n");
    return 0;
}


